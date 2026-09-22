**Langues**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← Index de la documentation](README.md)

# Émulation des pilotes Windows

L’émulateur de pilotes optionnel de NeverD exécute le point d’entrée PE d’un
pilote WDM x64 pris en charge et peut parcourir un scénario explicite de requêtes
synchrones avant de le décharger. Il utilise Unicorn pour l’exécution CPU et le
modèle Windows borné propre à NeverD. Il ne charge pas le pilote dans le noyau
hôte et ne transmet pas les appels d’API invités aux services du système hôte.

## Compiler et exécuter

La fonctionnalité nécessite une activation explicite, indépendante de `BUILD_TESTING` :

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON
cmake --build build-release --target neverd --parallel 4
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --instruction-limit 100000 > driver-report.json
```

Le rapport est toujours émis en JSON sur stdout ; les diagnostics des requêtes
et de la préparation vont sur stderr. Les limites par défaut sont de 100000
instructions invitées, 64 MiB de mémoire invitée, 10000 événements enregistrés
et 5000 millisecondes. La limite d’instructions doit être positive. L’épuisement
d’un budget arrête l’exécution en conservant les observations partielles.

| Code de sortie | Signification |
|----------------|---------------|
| `0` | L’initialisation et toutes les opérations demandées et terminées ont réussi |
| `1` | Entrée/options invalides, échec de préparation ou fonctionnalité désactivée à la compilation |
| `2` | L’initialisation ou une requête terminée a renvoyé un `NTSTATUS` d’échec |
| `3` | L’exécution s’est arrêtée avant la fin du scénario, par exemple à cause d’une API non prise en charge, d’une faute ou d’une limite de budget |

Un statut d’échec renvoyé constitue une observation achevée de cette opération.
Un retour réussi décrit uniquement cette exécution modélisée ; il ne démontre
pas que le pilote fonctionne sous Windows.

## Compatibilité des pilotes

La compatibilité dépend du chemin de code exécuté et de ses dépendances, et non
de l’extension `.sys`. Les preuves de validation actuelles couvrent des fixtures
autonomes originales et le chemin bufferisé de l’exemple WDM SIOCTL de Microsoft.
Elles ne démontrent pas une compatibilité avec des pilotes tiers quelconques.

| Classe de pilote ou exigence | Périmètre actuel | Environnement manquant |
|-----------------------------|------------------|------------------------|
| Pilote WDM logiciel x64 utilisant les API listées | Initialisation bornée et un cycle de vie de fichier synchrone | Chaque API supplémentaire exécutée doit avoir un modèle défini |
| IOCTL `METHOD_BUFFERED` | Pris en charge dans le scénario explicite de requêtes | Plusieurs fichiers ouverts et l’achèvement asynchrone ne sont pas disponibles |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT`, `METHOD_NEITHER` | Rejetés | MDL, pages verrouillées, vérification des accès et durée de vie des tampons utilisateur |
| Pilote KMDF / UMDF | Non pris en charge | Liaison au framework, objets, files, callbacks et environnement d’exécution hôte approprié |
| Pilote PnP de bus, de fonction ou de filtre | L’initialisation peut s’exécuter dans le sous-ensemble d’API ; le cycle de vie de la pile de périphériques n’est pas pris en charge | Attachement de périphériques, dispatch vers les pilotes inférieurs, IRP PnP et d’alimentation |
| Pilotes de stockage, réseau, affichage, système de fichiers et minifiltres | Contrats de sous-systèmes non pris en charge | Frameworks de port/classe/miniport, NDIS/WFP, services graphiques ou de système de fichiers |
| Pilote utilisant des threads de travail, timers, DPC, APC, attentes ou annulations | Non pris en charge | Ordonnancement, transitions IRQL, synchronisation et responsabilité asynchrone des ressources |
| Pilote utilisant des callbacks de processus/thread, handles, opérations de registre/fichier ou découverte de modules noyau | Non pris en charge hors des API listées | Gestionnaire d’objets, état système et producteurs de callbacks/événements |
| Pilote matériel, DMA, PCI, d’interruption ou de virtualisation | Environnement non pris en charge | Modèles de périphériques, mémoire physique, bus, interruptions et état CPU privilégié |
| Pilote Windows x86 ou ARM64 | Rejeté | Chargement, ABI et modèle d’exécution propres à l’architecture |
| Image x64 exigeant CFG, une configuration de chargement non prise en charge, TLS ou d’autres fonctions PE rejetées | Rejetée au chargement | Sémantique explicite du chargeur et de l’exécution pour ces exigences |

Un import non pris en charge mais inutilisé peut rester lié. Atteindre une
opération non prise en charge arrête l’exécution avec un diagnostic et les
observations déjà recueillies. La seule réussite de DriverEntry ne prouve pas
la prise en charge des chemins ultérieurs de dispatch, de matériel ou de
framework. Le tableau des API ci-dessous définit le sous-ensemble pris en charge.

## Contrat d’exécution

Le profil modélise un unique cycle de vie WDM x64 monothread à `PASSIVE_LEVEL`.
L’exécution commence au point d’entrée PE et conserve l’enveloppe d’entrée du
compilateur lorsqu’elle existe. DriverEntry doit renvoyer `STATUS_SUCCESS` pour
initialiser le pilote ; un autre statut de réussite non nul ou un statut en
attente arrête l’exécution comme contrat d’initialisation non pris en charge.
Un statut d’échec est conservé comme résultat d’initialisation achevé.
Tous les objets, chaînes, piles, pointeurs de fonction et allocations résident
en mémoire invitée. Le modèle fournit un `DRIVER_OBJECT` et un chemin de registre
pour le nom de service configuré (`NeverDDriver` par défaut).
L’adaptateur utilise le mode TLB virtuel d’Unicorn pour préserver les adresses
virtuelles invitées, y compris les adresses noyau canoniques hautes, sans
synthétiser de tables de pages Windows. La valeur initiale de RFLAGS est `0x202` ;
le profil de périphérique logiciel utilise une ligne de cache fixe de 64 octets.
Ce sont des propriétés explicites de ce scénario d’exécution.

Les imports inconnus sont liés à des pièges déclenchés à l’utilisation. Un import
inutilisé n’empêche pas l’exécution ; exécuter son thunk ou lire une donnée
exportée non modélisée arrête l’exécution avec `unsupported_api`. Les effets
non pris en charge sur l’environnement CPU provoquent aussi un arrêt explicite.
NeverD ne remplace pas les appels non implémentés par des valeurs de réussite.
Les images malformées et les exigences de chargement non prises en charge
échouent avant l’exécution.

Ce profil n’implémente ni noyau Windows complet, ni runtime KMDF, ni cycle de
vie PnP/alimentation, ni IRP asynchrones ou en attente, ni IOCTL direct/neither,
ni interruptions, ni ordonnancement multithread. Les callbacks s’exécutent
uniquement lorsque le scénario les demande explicitement ; une initialisation
seule s’arrête toujours après DriverEntry.

Les images utilisent leur base préférée sauf si un scénario sélectionne une
adresse de relocalisation valide. Elles doivent être des exécutables PE32+ x64
avec le sous-système natif. Les imports peuvent provenir de `ntoskrnl.exe` ou
de `ntkrnlmp.exe`. Le chargeur d’exécution prend en charge les relocalisations
de base x64 `DIR64` validées et une configuration de chargement limitée pour le
cookie de sécurité, initialisé avant l’enveloppe d’entrée avec un cookie invité
déterministe. CFG et les autres champs de configuration de chargement non
modélisés, TLS, les imports différés/liés, les imports par ordinal et les images
managées sont rejetés. Les images doivent aussi satisfaire des contrôles stricts
de plages et d’alignement.

Le modèle initial d’API possède volontairement un contrat limité :

| API | Comportement modélisé et restrictions |
|-----|--------------------------------------|
| `RtlInitUnicodeString` | Construit une `UNICODE_STRING` invitée pour une source bornée terminée par NUL |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Allocations de données pour les types de pool `0`, `1` et `512` ; taille/tag positifs, tags correspondants lors des libérations avec tag, aucune réutilisation d’adresse |
| `IoCreateDevice`, `IoDeleteDevice` | Type de périphérique `0x22`, caractéristiques `0` ou `0x100`, extensions bornées, noms ASCII `\Device\Name` |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` ou `\??\Name` dans un espace de noms de session, ciblant `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Texte littéral ASCII et `%%`, au plus 512 octets de sortie ; le formatage variadique arrête l’exécution ; tous les filtres du débogueur sont activés |
| `IoGetCurrentIrpStackLocation` | Renvoie l’emplacement de pile de l’IRP modélisé actif ; les macros WDM compilées habituelles lisent le même champ invité |
| `KeGetCurrentIrql` | Renvoie `PASSIVE_LEVEL` |
| `IofCompleteRequest`, `IoCompleteRequest` | Termine l’IRP modélisé synchrone actif avec `IO_NO_INCREMENT` ; aucun nouvel accès à un IRP terminé ou à son tampon n’est autorisé |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Opérations bornées sur les tampons invités, au plus 1 MiB par appel ; les API de copie sans chevauchement rejettent les chevauchements |

La structure RegistryPath d’origine et son tampon expirent au retour de
DriverEntry. Les pilotes qui ont besoin de la chaîne ultérieurement doivent
la copier pendant l’initialisation.

L’arène d’objets/pool fait 1 MiB. Les octets de pool non initialisés ont le
contenu déterministe `0xCD` ; ceux libérés contiennent `0xDD`. Il s’agit d’un
scénario d’exécution concret. Les accès CPU et les API de tampons modélisées
rejettent les allocations de pool libérées, les périphériques supprimés, les
octets d’arène non alloués, les champs d’objet opaques et les écritures dans les
champs d’objet en lecture seule. Ces contrôles couvrent la durée de vie des
objets de ce modèle ; ils ne constituent pas une analyse générale de sûreté
mémoire des pilotes. Les entrées non écrites de la table de dispatch indiquent
zéro pour « non enregistré ». Une requête de scénario pour une fonction majeure
non enregistrée est terminée par le gestionnaire par défaut modélisé avec
`STATUS_INVALID_DEVICE_REQUEST` ; l’échec reste visible dans les statuts de
dispatch et d’E/S. Le modèle n’invente pas d’adresse de fonction invitée pour
ce gestionnaire et la lecture par l’invité d’une entrée non écrite reste non
prise en charge. Enregistrer explicitement un callback nul est une erreur.

## Scénarios de requêtes

Passez un fichier JSON avec `--scenario` pour sélectionner les requêtes et un
éventuel déchargement :

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --scenario scenario.json > driver-report.json
```

Pour un pilote qui crée `\Device\NeverDIO` et accepte l’IOCTL bufferisé
`0x222000`, voici un exemple de `scenario.json` :

```json
{
  "requests": [
    {"kind": "create", "device": "\\Device\\NeverDIO"},
    {"kind": "ioctl", "code": "0x222000", "input": "00112233", "output_size": 4},
    {"kind": "cleanup"},
    {"kind": "close"}
  ],
  "unload": true
}
```

Le nom de périphérique et le code IOCTL doivent correspondre au pilote.
L’absence de `device` sélectionne l’unique périphérique actif ; une sélection
ambiguë échoue. Ce modèle suit un seul fichier ouvert et exige create, les
IOCTL, cleanup puis close dans cet ordre. Seuls les IOCTL `METHOD_BUFFERED`
sont pris en charge. Le dispatch doit terminer chaque IRP de façon synchrone ;
renvoyer `STATUS_PENDING`, ne pas terminer l’IRP, fournir des longueurs de sortie
invalides ou accéder à un IRP terminé provoque un échec explicite. Le déchargement
demandé ne doit laisser aucun périphérique, lien symbolique, allocation de pool
ou objet fichier actif.

Le champ racine optionnel `"load_address": "0x190000000"` demande un changement
de base ; son absence ou `"0x0"` utilise l’adresse préférée. L’image doit
satisfaire les exigences de relocalisation. La commande d’initialisation et
l’API C d’origine n’impliquent aucun scénario.

Seuls `load_address`, `requests` et `unload` sont acceptés à la racine. Les
champs de requête sont `kind`, éventuellement `device` et, uniquement pour
`ioctl`, le champ obligatoire `code` ainsi que les champs optionnels `input`
et `output_size`. Les champs inconnus ou dupliqués sont rejetés. `code` accepte
un entier JSON non signé de 32 bits ou une chaîne hexadécimale préfixée par
`0x`. `input` est une chaîne d’octets hexadécimaux de longueur paire sans préfixe
ni espaces ; son absence signifie une entrée vide. `output_size` est un entier
JSON non signé ; son absence signifie zéro. Les fractions numériques et les
notations à virgule flottante sont rejetées.

Le texte du scénario est limité à 2 MiB, avec au plus 64 requêtes, 65536 octets
par tampon d’entrée ou de sortie et 512 KiB d’octets d’entrée et de sortie
cumulés. Les budgets d’instructions, d’observations, de mémoire invitée et de
temps s’appliquent à l’ensemble du scénario. L’arène de 1 MiB contient aussi
les objets et métadonnées : une image peut donc épuiser la mémoire du modèle
avant de consommer les tailles maximales des tampons du scénario.

## Validation avec l’exemple Microsoft

Le [script de validation](../../scripts/validate_windows_driver_sample.py),
à lancer explicitement, télécharge le source SIOCTL de Microsoft à la révision
figée dans le [manifeste de validation](../../unittests/emulation/fixtures/sioctl-validation.json),
vérifie les empreintes SHA-256 et compile le source non modifié avec les en-têtes
DDK de MinGW-w64. Il conserve la licence et la provenance amont, les commandes
de compilation, le scénario et les rapports dans le répertoire de sortie choisi.
Il nécessite un accès réseau, Clang, `lld-link`, `nm` et les en-têtes DDK de MinGW-w64 :

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

Utilisez `--headers` pour un répertoire d’inclusion MinGW-w64 différent de celui
par défaut. Le script génère une bibliothèque d’importation MS COFF à partir
des dépendances de l’objet compilé. La vérification exécute DriverEntry, create,
l’IOCTL bufferisé de l’exemple, cleanup, close et unload. L’exemple amont
n’enregistre pas de gestionnaire cleanup ; le gestionnaire par défaut modélisé
termine donc cleanup avec `STATUS_INVALID_DEVICE_REQUEST` (`0xC0000010`).
Le pilote est néanmoins fermé puis déchargé, et l’IOCTL réussi renvoie les
octets attendus. Pour ce scénario complet, le code de sortie attendu de la CLI
est **2** et `scenario_success` vaut false. Le script lui-même ne réussit que
si tous ces résultats correspondent, y compris l’échec visible de cleanup ;
il ne réécrit pas l’exemple pour masquer ce résultat.

## Rapports et SDK

Le rapport JSON distingue `stop_reason`, les champs pouvant être null
`nt_status` et `nt_success`, le PC à l’arrêt et le nombre d’instructions. Il
conserve les appels d’API et l’état observable recueillis avant l’arrêt, notamment
les objets périphériques et les adresses des callbacks du pilote. Les adresses
invitées sont des chaînes hexadécimales afin que les consommateurs JSON ne
perdent pas de précision sur 64 bits. L’objet `configuration` enregistre les
limites et le nom du service de l’exécution. Le profil est
`wdm-x64-synchronous-v1`. `nt_status` reste le résultat de DriverEntry, tandis
que `scenario_success` décrit conjointement l’initialisation et les requêtes
terminées. `phase`, `requests` et `unload_completed` identifient les parties du
cycle demandé qui ont été exécutées. Chaque appel d’API et écriture CPU indique
aussi sa phase (`driver_entry`, `request:N` ou `unload`). Chaque requête rapporte
les statuts de dispatch et d’E/S, l’achèvement, la longueur d’information et
les octets renvoyés dans `output_hex`. `preferred_image_base` décrit la base
PE d’origine. `security_cookie` est l’adresse invitée du cookie initialisé,
ou `"0x0"` si aucun n’était nécessaire. Les champs des requêtes sont `kind`,
`device`, `code`, `irp`, `completed`, `dispatch_status`, `io_status`,
`information` et `output_hex`.

`instructions` compte les tentatives d’instructions invitées admises. Une
instruction rejetée par la politique d’exécution n’est pas comptée ; une
instruction admise qui provoque une faute CPU est comptée. Le dispatch d’API
synthétique et la sentinelle de retour n’incrémentent pas ce compteur.

Chaque entrée `writes` possède `semantics: "attempted_guest_write"` : elle
enregistre une tentative d’écriture CPU hors de la pile, y compris si elle
provoque ensuite une faute ou est arrêtée par un budget. Elle ne garantit pas
que l’écriture a abouti et n’inclut pas les écritures des modèles d’API. Les
instantanés des objets périphérique et pilote décrivent l’état observé à l’arrêt.

Incluez `neverd/sdk/NeverDCAPIEmulation.h` (ou l’en-tête global de l’API C),
créez une session et appelez `neverd_emulate_driver_json(session, path, options)`.
Un chemin explicite non vide passe directement à la validation stricte préalable
à l’exécution, sans chargement préalable par l’API d’analyse générale. La CLI
utilise cette voie. `NULL` pour options sélectionne les valeurs par défaut.
Les options explicites `neverd_driver_options_v1` exigent la valeur exacte
de `struct_size` et des budgets positifs d’instructions, de mémoire, d’événements
et de temps. Libérez le résultat avec `neverd_free_string`.

Passer `NULL` comme chemin exige au contraire une session chargée et reparcourt
son fichier indépendamment de l’analyse IR et du chargement limité à certaines
fonctions. Les deux voies préservent l’image de la session. Gardez le fichier
d’entrée disponible et inchangé pendant l’appel. Les échecs de requête/préparation
renvoient `NULL` et définissent `neverd_last_error` ; les arrêts d’exécution
renvoient du JSON. L’API reste disponible dans les builds désactivés et indique
comment activer la fonctionnalité.

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)`
utilise les mêmes options v1 et règles de propriété, en ajoutant une entrée de
scénario stricte. Une chaîne JSON non NULL terminée par NUL est requise. L’ABI
originale `neverd_emulate_driver_json` reste inchangée et limitée à l’initialisation.
Le parseur C++ `driverOptionsFromScenarioJSON` fournit la même validation de
scénario aux appelants de `emulateDriver`.

Le point d’entrée C++ interne est `neverd::emulation::emulateDriver` dans
`include/neverd/emulation/DriverSession.h`. L’analyse du format appartient au
chargeur existant ; le comportement des objets/API Windows appartient à
`lib/emulation/windows` ; l’état CPU et l’exécution appartiennent à l’adaptateur
Unicorn. L’adaptateur et le modèle utilisent la même interface de mémoire
invitée. Aucun comportement d’API Windows n’a sa place dans le fork Unicorn.
