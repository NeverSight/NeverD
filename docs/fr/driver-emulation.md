**Langues**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← Index de la documentation](README.md)

# Émulation des pilotes Windows

L’émulateur de pilotes optionnel de NeverD exécute le point d’entrée PE d’un
pilote WDM x64 pris en charge et peut parcourir un scénario explicite de requêtes
sérielles avant de le décharger. Il utilise Unicorn pour l’exécution CPU et le
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
et 5000 millisecondes. La limite d’instructions doit être positive. L’épuisement d’un budget d’exécution arrête le traitement en conservant les observations partielles. Les API d’allocation conservent toutefois leur résultat documenté en cas de manque de place ; une pénurie de mappage MMIO renvoie par exemple NULL.

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

La compatibilité dépend du chemin de code exécuté et de ses dépendances,
non de l’extension `.sys`. Les validations réalisées couvrent des fixtures
autonomes originales ainsi que les chemins bufferisés, in-direct et out-direct
de l’exemple WDM SIOCTL de Microsoft, y compris sa compilation avec journalisation
de débogage, et l’exemple public Zero de Pavel Yosifovich pour READ/WRITE directs
et les statistiques. Elles n’établissent pas une compatibilité avec des pilotes
tiers arbitraires.

| Classe de pilote ou exigence | Périmètre actuel | Environnement manquant |
|-----------------------------|------------------|------------------------|
| Pilote WDM logiciel x64 utilisant les API listées | Initialisation WDM x64 bornée, requêtes sérielles buffered/direct, travail, timers, DPC, événements et attentes, rapports et limites | Toute API supplémentaire exécutée nécessite un modèle défini |
| IOCTL `METHOD_BUFFERED` | E/S buffered/direct sérielles, achèvement par travail ou DPC | Sous-ensemble d’API ci-dessous seulement ; ni soumissions concurrentes de scénarios publics ni annulation générale de requête WDM |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | MDL propres aux requêtes, mappages système, identités physiques partagées et SG DMA | mappages utilisateur et autres interfaces DMA |
| MDL alloués par le pilote | Descripteurs indépendants pour pool non paginé ou une allocation utilisateur avec pages physiques partagées | Sans association IRP, chaînes MDL ni processus arbitraires |
| READ/WRITE | E/S buffered/direct/neither sérielles, achèvement par travail ou DPC | API répertoriées uniquement ; sans IRP simultanés, annulation WDM générale ni position implicite du fichier |
| WDM `METHOD_NEITHER` | Tampons utilisateur distincts, sondes, MDL verrouillés, identités synthétiques, révocation de VA ou sortie après distribution et annulation limitée | Pas d’attachement général de processus ni de mappages ou remappages arbitraires |
| Pilote KMDF 1.33 non-PnP | Liaison, objets/contextes, périphériques de contrôle nommés, files séquentielles ou parallèles par défaut à limite finie ou illimitée et requêtes tamponnées/directes/neither avec callbacks exécutés | Pas de périphériques PnP, ordonnancement général des files, extensions de classe ou UMDF |
| Pilote PnP de bus, de fonction ou de filtre | PDO explicites sans ressources ou à banque de registres fixe, AddDevice invité et huit fonctions mineures courantes du cycle PnP | Autres opérations PnP, politique générale d’alimentation, autres modèles matériels/de ressources et KMDF PnP |
| Pilotes de stockage, réseau, affichage, système de fichiers et minifiltres | Contrats de sous-systèmes non pris en charge | Frameworks de port/classe/miniport, NDIS/WFP, services graphiques ou de système de fichiers |
| Travail, timers, DPC, événements et attentes | L’IRQL courant est `PASSIVE_LEVEL` pour le dispatch et le travail, et `DISPATCH_LEVEL` pour les DPC | Sous-ensemble d’API ci-dessous seulement ; ni soumissions concurrentes de scénarios publics ni annulation générale de requête WDM |
| Opérations de registre via les API Zw listées | Arborescence explicite de session et droits par handle | ACL, privilèges, vues alternatives et persistance |
| Pilote utilisant des callbacks de processus/thread, d’autres handles, des opérations de fichier ou la découverte de modules noyau | Non pris en charge hors des API listées | Gestionnaire d’objets, état système et producteurs de callbacks/événements |
| Pilote matériel, DMA, PCI, d’interruption ou de virtualisation | Banque explicite de registres mémoire, MMIO, interruptions latched exclusives et DMA cohérent borné par tampons communs/SG/canaux pris en charge | Autres modèles de périphériques, RAM physique arbitraire, PCI, ports, autres modes d’interruption, autres interfaces DMA et état CPU privilégié |
| Pilote Windows x86 ou ARM64 | Rejeté | Chargement, ABI et modèle d’exécution propres à l’architecture |
| CFG x64 | Tables de cibles validées et appels check/dispatch ; les pointeurs de repli invités restent inchangés si CFG est inactif | XFG, suppression des exports, configuration de chargement non modélisée et TLS restent rejetés |

Un import non pris en charge mais inutilisé peut rester lié. Atteindre une
opération non prise en charge arrête l’exécution avec un diagnostic et les
observations déjà recueillies. La seule réussite de DriverEntry ne prouve pas
la prise en charge des chemins ultérieurs de dispatch, de matériel ou de
framework. Le tableau des API ci-dessous définit le sous-ensemble pris en charge.

## Contrat d’exécution

Le profil modélise un cycle WDM x64 sur CPU0 avec un ordonnancement coopératif déterministe. L’exécution commence au point d’entrée PE et conserve l’enveloppe d’entrée du
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
Les lectures x64 de CR8 en ligne observent le même `PASSIVE_LEVEL` / `DISPATCH_LEVEL` ; les écritures
de CR8 et les autres opérations sur les registres de contrôle restent non prises en charge.

Les imports inconnus sont liés à des pièges déclenchés à l’utilisation. Un import
inutilisé n’empêche pas l’exécution ; exécuter son thunk ou lire une donnée
exportée non modélisée arrête l’exécution avec `unsupported_api`. Les effets
non pris en charge sur l’environnement CPU provoquent aussi un arrêt explicite.
NeverD ne remplace pas les appels non implémentés par des valeurs de réussite.
Les images malformées et les exigences de chargement non prises en charge
échouent avant l’exécution.

Les éléments `DelayedWorkQueue` s’exécutent à `PASSIVE_LEVEL`, les callbacks DPC invités à `DISPATCH_LEVEL` avec les quatre arguments prévus. L’ordonnancement coopératif déterministe sur CPU0 intervient aux retours d’appels et aux attentes bloquantes. Les timers relatifs, absolus et périodiques utilisent un temps virtuel qui avance jusqu’à la prochaine échéance de timer, d’attente ou d’annulation lorsqu’aucun cadre ne peut s’exécuter. Les événements/timers de notification et de synchronisation conservent leurs règles distinctes de consommation du signal. Chaque callback dispose de sa pile invitée ; plusieurs cadres bloqués gardent leurs variables locales et contextes CPU complets, avec une mémoire partagée. Win64 passe les quatre premiers arguments en registres, les suivants sur la pile. Les requêtes restent sérielles : un dispatch marquant l’IRP en attente doit retourner `STATUS_PENDING` et terminer avant la requête suivante. Sans producteur disponible, une requête en attente ou une attente infinie s’arrête avec un `model_error` de blocage. Les budgets d’instructions, mémoire, observations et temps réel restent communs.

Ce modèle borné n’offre pas tout l’asynchronisme Windows. Attentes alertables ou utilisateur, APC, annulation générale des requêtes WDM, soumissions concurrentes de scénarios publics, UMDF, périphériques PnP KMDF et ordonnancement général des files, PnP/alimentation complet, matériel général, autres interfaces DMA et autres modes d’interruption restent non pris en charge. L’initialisation seule exécute les callbacks explicitement mis en file sans créer de requêtes ni déchargement implicites.

L’élément est retiré de la file avant le début de son callback, qui peut donc le libérer. La libération d’un élément encore en file, la double mise en file, les objets expirés et les cibles hors mémoire invitée exécutable échouent explicitement. La référence au périphérique reste détenue jusqu’au retour. Le déchargement exige la libération de tous les éléments et l’achèvement du travail en file. Les contextes CPU préservent les registres généraux, SIMD, FPU et l’état de contrôle ; la mémoire invitée reste partagée et restaurer un contexte ne permet pas de reprendre un CPU en faute.
La suppression est différée tant que subsistent des objets fichiers ou des références de travail en file/en cours. L’allocation d’un élément retourne NULL si l’arène d’objets est épuisée.

Les images utilisent leur base préférée sauf si un scénario sélectionne une
adresse de relocalisation valide. Elles doivent être des exécutables PE32+ x64
avec le sous-système natif. Les imports peuvent provenir de `ntoskrnl.exe`,
`ntkrnlmp.exe` ou `WDFLDR.SYS`. Le chargeur d’exécution prend en charge les relocalisations
de base x64 `DIR64` validées et une configuration de chargement limitée pour le
cookie de sécurité, initialisé avant l’enveloppe d’entrée avec un cookie invité
déterministe. Les autres champs de configuration de chargement non
modélisés, TLS, les imports différés/liés, les imports par ordinal et les images
managées sont rejetés. Les images doivent aussi satisfaire des contrôles stricts
de plages et d’alignement.

Control Flow Guard (CFG) actif valide les indicateurs PE, les emplacements de pointeurs et les cibles exécutables triées. Les helpers check/dispatch acceptent seulement les entrées déclarées de l’image ou les thunks API enregistrés, préservent l’état d’appel Win64 et rejettent les autres cibles. Une instrumentation sans CFG actif conserve les pointeurs de repli invités d’origine. XFG actif, suppression des exports et autres politiques non modélisées restent rejetés ; une adresse exécutable n’est pas automatiquement une cible valide.

Une pile WDM peut contenir plusieurs objets de périphérique appartenant au même pilote invité. `IoAttachDeviceToDeviceStack` place une source isolée au-dessus du sommet actuel de la cible et renvoie cet ancien sommet. Il définit `StackSize` et `AlignmentRequirement` sans modifier la liste `NextDevice` ni copier les indicateurs de tampon. `IoDetachDevice` reçoit le périphérique inférieur mémorisé et exige `PASSIVE_LEVEL` ; l’attachement autorise un IRQL jusqu’à `DISPATCH_LEVEL`. Ouvrir le périphérique inférieur nommé déclenche le dispatch au sommet actuel, tandis que `FILE_OBJECT.DeviceObject` et le rapport gardent l’identité nommée. READ/WRITE utilise les indicateurs du sommet sélectionné. La route mémorisée conserve tous les périphériques jusqu’au retour du dispatch, même après détachement/suppression ; les références internes n’augmentent pas `ReferenceCount`, qui compte les handles ouverts.

`IofCallDriver` et l’aide `IoCallDriver` appellent la cible exacte de cette route. Les véritables fonctions inline `IoCopyCurrentIrpStackLocationToNext`, `IoSkipCurrentIrpStackLocation` et `IoSetCompletionRoutine` manipulent l’IRP invité original ; curseur, nombre et indicateurs de contrôle sont validés. Le dispatch inférieur renvoie son vrai statut, distinct de `IoStatus` et du retour des callbacks de fin. Le déroulement avance le curseur, choisit les callbacks selon succès/erreur/annulation et propage pending vers le haut. Un callback exécuté assume cette propagation, même après un précédent retour `STATUS_PENDING`. `STATUS_MORE_PROCESSING_REQUIRED` suspend le déroulement en conservant IRP, MDL et tampons ; un appel ultérieur reprend la fin. Une fin imbriquée exige le résultat d’arrêt extérieur et ne libère le stockage qu’une fois, au terme du déroulement. Les continuations identifiées par sous-système conservent les cadres WDM/WDF et l’IRQL hérité. Chaque emplacement inférieur consommé est remis à zéro avant le callback de fin supérieur.

Les politiques générales d’alimentation, les IRP alloués par le pilote, les autres fonctions mineures PnP et les autres modèles matériels/de ressources restent exclus. L’attachement/transfert WDF, l’attachement sur une pile ayant des fichiers ou callbacks actifs, le détachement d’une couche intermédiaire, le changement de fonction majeure et les cibles hors route échouent explicitement. Le fixture facultatif `driver_wdm_stack.c`, compilé avec le véritable WDK, utilise `NEVERD_WDM_STACK_FIXTURE` et `NEVERD_WDM_STACK_CFG_FIXTURE`. Les tests natifs et C API/CLI incluent la relocalisation ; les artefacts absents sont explicitement ignorés. Les preuves d’exécution restent limitées à Linux.

Un scénario peut configurer explicitement au plus 64 `pnp_devices`. Chaque entrée exige `id`, `bus: "resource_free"` ou `bus: "register_bank"`, `initial_device_power: "D0"` et `initial_system_power: "working"` ; les faits manquants ne sont pas devinés. Les ID sont ASCII, sensibles à la casse, longs de 1–64 octets, commencent par un caractère alphanumérique et ne contiennent ensuite que des caractères alphanumériques, `_`, `-`, `.`. Une requête ordinaire peut choisir un `device_id` configuré au lieu de `device`, jamais les deux. `kind: "pnp"` exige `device_id`, `minor` et `bus_completion` ; les fonctions admises sont `start`, `query_remove`, `cancel_remove`, `remove`, `query_stop`, `stop`, `cancel_stop` et `surprise_removal`. `bus_completion.status` est obligatoire, entier 32 bits ou chaîne hexadécimale ; `delay_100ns`, facultatif, est un entier positif ou nul jusqu’à INT64_MAX depuis la réception réelle du fournisseur. `STATUS_PENDING` n’est pas un état final ; stop/cancel-stop/surprise-removal/cancel-remove/remove exigent exactement `STATUS_SUCCESS` (0). Les champs de fichier, transfert et annulation sont refusés pour PnP, même à zéro. L’API C++ applique le même contrôle préalable.

```json
{
  "pnp_devices": [
    {"id": "sensor0", "bus": "resource_free", "initial_device_power": "D0", "initial_system_power": "working"}
  ],
  "requests": [
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0", "delay_100ns": 10}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "surprise_removal", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "remove", "bus_completion": {"status": "0x0"}}
  ],
  "unload": true
}
```

Après un DriverEntry réussi, `AddDevice` s’exécute une fois par PDO configuré, avec un `DRIVER_OBJECT` distinct appartenant au fournisseur ; l’invité ne peut supprimer ni usurper ses objets. Les IRP PnP sont `KernelMode`, sans fichier, avec ressources START selon le bus configuré, initialement `STATUS_NOT_SUPPORTED`. La réponse du bus n’est consommée que si le transfert atteint le PDO ; la fin différée utilise l’horloge virtuelle et les continuations existantes. La fin supérieure valide ou annule la transition indépendamment du statut du bus. Un succès PnP exige une fin réelle du fournisseur ; un échec anticipé START/QUERY_STOP/QUERY_REMOVE peut laisser les observations du bus nulles. Un retrait normal depuis Started exige query réussi, fichiers fermés, requêtes antérieures terminées et détachement/suppression par l’invité. L’identité des appareils/fichiers persiste après détachement. Un échec AddDevice propre retire seulement le fournisseur ; toute fuite de nouvel appareil invité, même détaché, cause `model_error`. Tous les fournisseurs doivent être absents avant unload. Autres fonctions PnP, politique générale d’alimentation, autres modèles matériels/de ressources et KMDF PnP restent exclus.

`configuration.pnp_devices` conserve la configuration initiale. Les `pnp_devices` observés contiennent `id`, `pdo`, `add_device_status` nullable, `attached` actuel, `pnp_state`, `provider_present` ; après retrait, `attached` vaut false. Les phases AddDevice sont `add_device:<ID>` ; leurs échecs affectent `scenario_success` sans remplacer le `nt_status` de DriverEntry. Chaque requête ajoute `device_id` et `pnp` nullables ; PnP utilise `file: null`. `pnp` contient `minor`, `state_before`, `state_after`, `bus_status` nullable, `bus_received_at_100ns`, `bus_completed_at_100ns`. Le statut configuré devient une observation seulement à la fin réelle du bus ; la réception est indépendante. Les types existants restent inchangés.

Les requêtes ordinaires CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE atteignent le vrai dispatch invité tant que l’appareil existe hors Removing/Removed. Le modèle ne fabrique pas de refus à partir de Stopped, StopPending, RemovePending ou de l’alimentation : le pilote décide de terminer l’I/O logiciel, de le refuser ou de le retenir. L’exécution publique reste sérielle ; un IRP retenu sans producteur disponible ne peut être libéré par un start/cleanup ultérieur du scénario et s’arrête en `model_error` bloqué. Fermer les fichiers et terminer les requêtes antérieures avant Remove sont des limites du profil. Un `query_stop` terminé par `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) est refusé au contrôle préalable et à la fin invitée, car il exige une nouvelle requête de ressources non modélisée ; voir [le contrat QUERY_STOP Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device). Stop/restart et surprise-removal n’ajoutent ni rééquilibrage de ressources, ni politique générale d’alimentation, ni KMDF PnP.

Le PnP sans ressources utilise le véritable WDK avec le fixture original `driver_wdm_pnp.c`, les chemins facultatifs `NEVERD_WDM_PNP_FIXTURE`/`NEVERD_WDM_PNP_CFG_FIXTURE` et des tests natifs/C API/CLI. Les artefacts absents sont explicitement ignorés ; les preuves restent limitées à Linux.

Le bus synthétique `bus: "register_bank"` ajoute des ressources mémoire fixes et explicites à un PDO. Le tableau `resources` est facultatif si `interrupts` est non vide ; les deux listes doivent contenir ensemble au moins une ressource. Chaque entrée mémoire contient `id`, `raw_start`, `translated_start`, `length` et `registers` ; chaque registre exige `offset`, `width`, `access` (`read_only` ou `read_write`) et une `value` initiale. `DriverResources.h`／`DriverResources.def` définissent le contrat commun C++/JSON. Les ID de ressources suivent les règles des identifiants ASCII bornés et sont uniques dans chaque PDO. Les limites sont 8 ressources par PDO／32 au total,256 registres par ressource／4096 au total et 1–1048576 octets par ressource. Seuls les accès naturellement alignés d’exactement 1／2／4 octets sont pris en charge ; les valeurs doivent tenir dans cette largeur. Les deux intervalles physiques doivent être sans débordement ; les plages brutes ne se chevauchent pas dans un PDO et les plages traduites ne se chevauchent pas globalement. Un tableau `registers` vide déclare toute la banque inaccessible. Les adresses et valeurs initiales sont des faits déclarés, jamais du matériel hôte ni une mémoire implicitement remplie de zéros. `resource_free` conserve l’inventaire omis et les pointeurs START null.

START reçoit deux allocations distinctes, en lecture seule, de `CM_RESOURCE_LIST` brute et traduite. Leurs descripteurs Memory correspondent dans le même ordre : un descripteur complet, interface Internal, bus 0, version/révision 1, partage DeviceExclusive et indicateurs de plage READ_WRITE. Le droit RO propre à chaque registre reste indépendant. Le succès du START inférieur rend l’affectation disponible avant les callbacks d’achèvement supérieurs. Chaque START depuis NotStarted/Stopped crée une nouvelle génération de ressources avec la même affectation fixe. Les valeurs sont initialisées une fois par PDO et survivent à unmap, STOP et au redémarrage. Un START échoué et un STOP/REMOVE réussi exigent que le pilote libère ses mappages avant l’achèvement final de l’IRP ; aucun nettoyage implicite n’a lieu. Surprise-removal interdit immédiatement les nouveaux mappages et les accès aux registres, mais autorise encore unmap. L’achèvement réellement réussi d’un SET de périphérique chez le fournisseur modifie l’accessibilité matérielle : D 3 interdit l’accès, D 0 ne l’autorise qu’avec une affectation disponible. D 3 permet encore de mapper sans accès aux registres ; les changements d’alimentation ne suppriment ni mappages ni valeurs.

`MmMapIoSpace` prend en charge NonCached ; `MmMapIoSpaceEx` accepte PAGE_NOCACHE avec PAGE_READONLY ou PAGE_READWRITE. Ils acceptent seulement une sous-plage traduite déclarée dans une seule affectation, en conservant son décalage de page. Les alias partagent une banque, gardent leurs permissions indépendantes et exigent la base initiale et la longueur exacte pour `MmUnmapIoSpace`. L’épuisement du nombre de mappages, de la fenêtre virtuelle ou du budget mémoire configuré renvoie NULL ; les défaillances du backend restent explicites. Les adresses démappées ne redeviennent pas valides lors d’un mappage ultérieur. Les instructions scalaires et REP réelles des tampons de registres passent par les contrôles MMIO du CPU. Trous, mauvaises largeurs, désalignement, écritures RO, franchissement de mappages et exécution échouent avant tout effet sur les registres. Les API mémoire peuvent exécuter une transaction naturellement alignée d’exactement 1／2／4 octets sur un seul registre. Les plages plus grandes touchant MMIO échouent explicitement, sans découpage automatique en transactions de registres. Cela ne fournit ni RAM physique arbitraire, ni rééquilibrage de ressources, ports, autres modes d’interruption, autres interfaces DMA ou comportement matériel général.

Le [scénario de banque de registres](../examples/driver-register-bank-scenario.json), exécutable, utilise le fixture original `driver_wdm_resources.c` compilé avec le vrai WDK via `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`. Il exécute 14 requêtes couvrant START différé, I/O de fichier, STOP, redémarrage et retrait, puis observe les valeurs persistantes par l’IOCTL du pilote. `configuration.pnp_devices[].resources` enregistre les faits initiaux et les adresses physiques hexadécimales sans perte ; ce n’est pas un second rapport d’état de la banque. C API et Python conservent `scenario_json` et `neverd_driver_options_v 1` inchangé. Les artefacts réels absents sont explicitement ignorés ; les preuves d’exécution actuelles se limitent à Linux.

Le même fournisseur `register_bank` peut déclarer `interrupts` seul ou avec `resources` ; au moins une liste doit être non vide. `resource_free` refuse un champ explicite `interrupts`, y compris `[]`. Chaque interruption exige `id`, `raw_vector`, `raw_level`, `raw_affinity`, `translated_vector`, `translated_level`, `translated_affinity`, `mode: "latched"` et `share: "device_exclusive"`. `DriverInterrupts.h` / `DriverInterrupts.def` possèdent les types, noms et limites : 8 interruptions par PDO / 32 au total, ID ASCII bornés uniques dans chaque PDO, vecteurs traduits globalement exclusifs, niveau brut 0–65535 et DIRQL traduit 3–12. Les vecteurs conservent leurs 32 bits sans relation vecteur/IRQL supposée. Les deux masques d’affinité doivent valoir 1 : CPU0 et groupe0 sont des faits explicites du fournisseur. Valeurs brutes et traduites sont indépendantes. Les descripteurs mémoire gardent leur ordre, suivis des descripteurs d’interruption ordonnés dans la même `CM_RESOURCE_LIST` ; ceux-ci utilisent le type 2, le partage DeviceExclusive et les indicateurs LATCHED. Ce profil synthétique déclenché sur front ne représente pas les lignes PCI partagées de niveau.

Les requêtes READ/WRITE/IOCTL peuvent déclarer `interrupt_events`, chaque événement contenant explicitement `after_100ns`, `device_id` et `interrupt_id` ; les autres types refusent le champ, même vide. Au plus 64 événements par requête / 1024 au total sont admis, avec un délai positif ou nul jusqu’à INT64_MAX. La soumission réussie fixe l’origine du délai et capture une interruption déjà connectée ainsi que la génération de ressources de son PDO. Ce sont des impulsions externes indépendantes : terminer l’IRP source ne les annule pas, et écrire des registres n’implique aucun comportement d’activation, d’état ou d’acquittement. Le temps n’avance qu’à vide ; les événements échus sont exécutés à la prochaine frontière de callback prise en charge. Ainsi `after_100ns: 0` ne promet ni préemption d’instruction ni livraison avant la première instruction invitée. Les producteurs simultanés sont prévalidés ensemble pour leur capacité ; la publication matérielle du fournisseur précède l’éligibilité et les ISR précèdent DPC/travailleurs. Connexion perdue, génération périmée/indisponible ou D3 physique inscrit `undelivered_reason` puis arrête en `model_error` ; l’événement n’est jamais réaffecté ni différé silencieusement.

`IoConnectInterrupt` utilise ses onze arguments réels et l’affectation traduite exacte. `IoConnectInterruptEx` accepte FullySpecified (1), LineBased (2, une ligne affectée sur le PDO explicite) et FullySpecifiedGroup (4, groupe 0) ; `IoDisconnectInterruptEx` exige la version et le contexte correspondants. Connexion et déconnexion nécessitent PASSIVE_LEVEL. Seuls un verrou d’interruption privé, le mode latched exclusif, CPU0/groupe0 et l’absence de sauvegarde de l’état flottant sont admis. L’IRQL de synchronisation doit égaler le DIRQL affecté ; zéro en LineBased sélectionne ce niveau. `KINTERRUPT` est opaque et son adresse n’est jamais réutilisée. L’ISR réelle reçoit `(Interrupt, ServiceContext)` et retourne BOOLEAN dans AL ; `FALSE` signifie non revendiqué, sans être un échec NTSTATUS. `KeSynchronizeExecution` exécute le véritable callback à un argument sous le même verrou au DIRQL, retourne son BOOLEAN et restaure IRQL/CR8 de l’appelant. `KeAcquireInterruptSpinLock` / `KeReleaseInterruptSpinLock` imposent la propriété non récursive, l’exécution d’origine et l’IRQL sauvegardé ; aucun verrou détenu ne peut subsister au retour d’un callback. Les attentes dans une interruption, verrous partagés fournis par l’appelant, interruptions partagées/de niveau/MSI/passives et préemption d’instruction restent non pris en charge. START échoué et STOP/REMOVE réussi exigent la déconnexion avant l’achèvement final ; les callbacks supérieurs peuvent effectuer le démontage auparavant, et la déconnexion ne supprime jamais silencieusement un DPC en file.

Les rapports séparent déclarations et observations. `configuration.pnp_devices[].interrupts` conserve les ressources ; `configuration.interrupt_events` aplatit les événements d’entrée avec `source_request_index` (index de la requête configurée) et `event_index`, tous deux à partir de zéro. Les lignes racines `interrupts` ajoutent `device_id`, `interrupt_id`, `epoch`, `due_at_100ns` absolu et les champs nullables `occurred_at_100ns`, `delivered_at_100ns`, `returned_at_100ns`, `interrupt_object`, `return_value`, `claimed` et `undelivered_reason`. `claimed` dérive uniquement de l’octet bas réellement retourné ; les horodatages sont des observations, pas des résultats de callback fabriqués. Pour `scenario_success`, chaque événement configuré doit revenir sans échec de livraison ; une ISR qui ne revendique pas l’interruption reste valide. Les effets des DPC apparaissent par les achèvements réels de requêtes, appels API et messages. Le [scénario d’interruption](../examples/driver-interrupt-scenario.json) exécutable utilise le fixture original compilé avec le vrai WDK `driver_wdm_interrupts.c`, via `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE` facultatifs : sept requêtes incluent START différé, IOCTL en attente terminé par ISR→DPC, nettoyage/fermeture de fichier et retrait. La frontière C/Python `scenario_json` et la structure `neverd_driver_options_v1` ne changent pas. Les artefacts absents sont explicitement ignorés ; les preuves d’exécution sont limitées à Linux.

`DriverDMA.h` / `DriverDMA.def` ajoutent un objet `dma` facultatif à un PDO `register_bank`, aux côtés de ses affectations mémoire/interruption. DMA seul ne remplace aucune de ces listes de ressources. Les sept champs sont explicites : `address_bits` (32 ou 64), `maximum_length` (1–1048576 octets), `map_registers` (1–256), `alignment` (puissance de deux de 1–4096), `logical_base` (non nul, aligné sur une page), `logical_length` (aligné sur une page, 4096–1073741824 octets) et le booléen `scatter_gather`. La plage logique doit tenir dans la largeur d’adresse sans débordement. Chaque PDO possède un domaine logique indépendant : des adresses égales sur des périphériques différents ne sont pas des alias. Les ressources MMIO traduites ne doivent pas chevaucher la plage de RAM du modèle réservée `[0x1000000000, 0x1000100000)`. Ces déclarations décrivent un maître de bus synthétique cohérent, jamais la mémoire physique de l’hôte ni un périphérique PCI.

`IoGetDmaAdapter` accepte les champs historiques de `DEVICE_DESCRIPTION` version 0/1 pour un maître de bus Internal et publie un `DMA_ADAPTER` version 1 avec sa véritable table `DMA_OPERATIONS` de 104 octets. Les sondages version 2/3 renvoient NULL sans lire la fin d’une description moderne. Chaque méthode indirecte est liée à cet adaptateur actif précis, indépendamment des imports noyau. Les méthodes implémentées sont `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `GetScatterGatherList`, `PutScatterGatherList`, `PutDmaAdapter`, `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers` et `FreeMapRegisters`. `FreeAdapterChannel` et `ReadDmaCounter` restent des erreurs nommées, car ce profil ne modélise aucun contrôleur DMA subordonné/système. Allocation/libération de tampons communs et consultation de l’alignement exigent PASSIVE_LEVEL ; Get/PutScatterGatherList exigent DISPATCH_LEVEL. La libération de l’adaptateur accepte un IRQL inférieur ou égal à DISPATCH_LEVEL. x64 ignore `CacheEnabled`. Les sondages de version non prise en charge, les capacités déclarées incompatibles et les pénuries d’allocation documentées renvoient NULL. Les sélections d’interface malformées ou non modélisées et les défaillances du backend restent des erreurs explicites.

`AllocateAdapterChannel` exige DISPATCH_LEVEL et réserve un jeton opaque non NULL de registres de mappage. Tampons communs, listes SG et réservations de canal partagent le même quota et la même FIFO par PDO. Le succès accepte un véritable `AdapterControl` immédiat ou en attente ; un nombre demandé excessif renvoie `STATUS_INSUFFICIENT_RESOURCES` sans callback. Un seul callback d’allocation inachevé est autorisé par périphérique invité. Appeler AllocateAdapterChannel depuis AdapterControl est refusé, même via un callback imbriqué. Les quatre arguments du callback contiennent la valeur réelle de `DEVICE_OBJECT.CurrentIrp` capturée à l’enregistrement. Ce champ précis de huit octets est inscriptible sur les périphériques invités ; zéro ou un IRP actif routé par ce périphérique est admis. Un callback en attente retient le paquet jusqu’à son entrée, après quoi il peut l’achever. Ce profil n’a toujours pas de StartIo ; l’argument IRP inutilisé du callback SG distinct reste NULL.

`AdapterControl` renvoie une `IO_ALLOCATION_ACTION` de 32 bits. Les bits hauts de RAX sont ignorés, et l’action ne remplace jamais le `STATUS_SUCCESS` d’AllocateAdapterChannel. `DeallocateObject` libère les registres inutilisés ou entièrement vidés au retour du callback. `DeallocateObjectKeepRegisters` les conserve jusqu’à un FreeMapRegisters utilisant exactement l’adaptateur, le jeton et le nombre initial. Renvoyer `KeepObject` exige un contrôleur système non modélisé et échoue explicitement. L’allocation nouvellement livrée ne peut pas être libérée comme allocation conservée avant le retour de son callback ; une autre allocation précédemment conservée peut être libérée selon son propre contrat. Identité du callback, registres conservés et octets activement mappés ont des durées de vie distinctes, toutes vérifiées avant le démontage de l’adaptateur ou du périphérique.

`MapTransfer` et `FlushAdapterBuffers` acceptent un IRQL inférieur ou égal à DISPATCH_LEVEL ; allocation de canal et libération de registres exigent DISPATCH_LEVEL. MapTransfer reçoit une position relative au MDL, lit et met à jour une véritable longueur ULONG, puis renvoie l’adresse logique par valeur. Le profil SG borné fournit un fragment de page sous-jacente par appel ; les positions suivantes contiguës dans le même MDL et la même direction prolongent une seule opération. Sans SG, toute l’étendue demandée est mappée en un appel si elle tient dans le nombre réservé, sans raccourcissement. Le premier mappage réserve une plage logique non réutilisée dimensionnée par la réservation de registres ; d’autres allocations peuvent s’intercaler sans la chevaucher. Tous les fragments partagent un seul maintien physique qui s’agrandit. Les transactions du périphérique peuvent couvrir toute l’opération actuellement mappée. L’accès CPU, la libération du MDL et l’achèvement qui retire un stockage retenu restent bloqués jusqu’au vidage global. Flush doit correspondre à la position initiale, au MDL, à la direction et à la longueur totale réellement mappée. Il libère les octets mappés sans libérer les registres ; le jeton conservé peut servir à une autre opération. Les vidages partiels, opérations mêlant plusieurs MDL et autres formes de MapTransfer sont hors de ce profil, sans être supposés invalides sur tout système Windows.

`KeFlushIoBuffers` valide un MDL actif verrouillé/non paginé. La plateforme modélisée est cohérente : aucune valeur de ReadOperation ou DmaOperation ne nécessite de copie de cache distincte. Cet appel ne libère pas la propriété DMA et ne remplace pas FlushAdapterBuffers. Le [scénario de canal](../examples/driver-dma-channel-scenario.json) exécute le code original `driver_wdm_dma_channel.c` avec deux appels MapTransfer, une transaction du périphérique traversant une frontière de page, un IRQ/DPC déclaré séparément, un vidage global et une libération exacte des registres. Les véritables images normales/CFG utilisent `NEVERD_WDM_DMA_CHANNEL_FIXTURE` et `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`.

`KernelPhysicalMemory` attribue au RAM existant au plus 256 pages physiques du modèle de 4096 octets. Adresses virtuelles CPU, identités de pages physiques et adresses logiques de périphérique sont distinctes. Les tableaux PFN des MDL construits exposent ces identités partagées en lecture seule ; les descripteurs non construits n’ont pas de PFN utilisables. De petites allocations voisines peuvent partager un PFN tout en conservant des plages d’octets et des durées de vie distinctes. Tampons communs, pools et tampons de requête utilisent les mêmes octets déjà détenus par `GuestMemory`, sans seconde copie DMA. Un mappage SG actif retient sa plage exacte de données et son descripteur. Achèvement, libération de pool/MDL et démantèlement refusent les dépendances actives avant de retirer le stockage. Démapper un MDL direct révoque uniquement son mappage système CPU ; DMA peut toujours atteindre sa mémoire sous-jacente verrouillée. `DmaWritable` enregistre le contrat de verrouillage en écriture séparément des permissions CPU : les écritures du périphérique exigent READ/OUT_DIRECT direct ou du stockage non paginé accessible en écriture. WRITE/IN_DIRECT n’obtiennent pas cette autorisation simplement parce que leur mappage CPU est inscriptible.

`GetScatterGatherList` valide CurrentVa/Length dans la plage originale du MDL et crée des fragments logiques de pages sur sa mémoire existante. Des registres de mappage disponibles permettent au véritable callback void `AdapterListControl` à quatre arguments de s’exécuter immédiatement, avant le retour de l’API. Sinon, l’admission retient données/descripteur et réserve un callback dans la FIFO du PDO jusqu’à libération des ressources. Ce profil ne possède pas de propriété StartIo : le deuxième argument IRP du callback vaut NULL. Le retour du callback ne libère pas le mappage. L’accès CPU aux données d’un SG actif exige d’abord Put ; un callback attendant des registres de mappage n’a pas encore confié ces octets au périphérique. `PutScatterGatherList` peut s’exécuter dans le callback. Après Put, le pilote peut achever la requête et libérer le dernier adaptateur, tandis que la continuation du callback et la référence au périphérique restent actives jusqu’au retour. Libérer un tampon commun exige son adaptateur, sa longueur, son adresse logique et son adresse CPU d’origine. Les adresses logiques ne sont jamais réutilisées pendant la session, même après redémarrage. Une attente de ressources sans producteur réel signale explicitement un blocage et n’invente ni achèvement ni échéance.

Seules les requêtes READ/WRITE/IOCTL acceptent `dma_events`. Chaque événement exige `after_100ns`, `device_id`, `logical_address`, `direction` et `length`. `write_memory` exige aussi `data_hex` de longueur exacte, tandis que `read_memory` refuse ce champ. Les directions sont vues depuis le périphérique. Les limites sont 64 événements par requête, 1024 au total, 16 MiB de données transactionnelles au total et 1 MiB par transaction ; le délai va de zéro à INT64_MAX. La soumission capture l’époque actuellement affectée au PDO et l’origine du temps virtuel, sans exiger un mappage que le dispatch suivant n’a pas encore créé. À la livraison, toute la plage logique active et sa direction sont résolues ; D0 physique et tous les octets sous-jacents sont validés avant tout effet. L’achèvement de l’IRP source n’annule pas l’événement. Un mappage absent/libéré, une époque périmée, un retrait surprise ou D3 consignent un échec et arrêtent l’exécution. Les événements ne se réassocient pas et n’inventent ni interruption, ni protocole de registres, ni achèvement d’IRP. À une même frontière d’ordonnancement, le fournisseur publie d’abord l’état matériel, puis les octets DMA sont transférés, puis les impulsions d’interruption déclarées indépendamment sont traitées. Le temps reste coopératif, sans préemption à chaque instruction.

Les rapports conservent `configuration.pnp_devices[].dma` et la liste aplatie `configuration.dma_events`. Les lignes racine `dma_transfers` identifient `source_request_index`, `event_index`, `device_id`, `epoch`, `logical_address`, `direction`, `length` et `due_at_100ns`, avec les champs pouvant valoir null `occurred_at_100ns`, `completed_at_100ns`, `mapping`, `adapter` et `failure_reason`. `data_hex` contient les octets réellement transférés. Chaque transaction déclarée doit s’achever sans échec pour obtenir `scenario_success`. Le [scénario DMA](../examples/driver-dma-scenario.json) utilise le code original `driver_wdm_dma.c` compilé avec le véritable WDK, via les options `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`. Il manipule un tampon commun par les véritables pointeurs d’adaptateur et s’achève par un ISR→DPC déclaré séparément. C/Python utilisent toujours `scenario_json`, sans changer `neverd_driver_options_v1`. Les artefacts absents entraînent un saut explicite ; les preuves d’exécution se limitent à Linux. Contrôleurs subordonnés, méthodes V2/V3, moteurs matériels de descripteurs, DMA KMDF général et autres modèles de périphériques restent non pris en charge.

Les remove-locks WDM exécutent les vrais exports `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx`, `IoReleaseRemoveLockEx` et `IoReleaseRemoveLockAndWaitEx` ; les noms WDK sans Ex sont des macros. Le propriétaire est le DEVICE_OBJECT précis dont l’extension contient tout le stockage aligné, indépendamment de l’état PDO et de la forme du Tag. L’initialisation avant attachement est permise. Les tailles retail 32/DBG 120 octets exigent un argument de taille séparé concordant ; toute la zone enregistrée est opaque. Les Tags NULL/répétés sont comptés par verrou, jamais déréférencés : la libération après fin d’IRP reste valide. Initialisation/AndWait exigent `PASSIVE_LEVEL`, acquire/release autorisent `DISPATCH_LEVEL`.

AndWait ferme l’admission, libère une acquisition concordante et suspend le vrai cadre invité jusqu’à libération des autres. Acquire retourne ensuite `STATUS_DELETE_PENDING` sans obligation de libération. La dernière libération mémorise la disponibilité avant le retour du callback ; un worker peut libérer puis attendre un événement du REMOVE repris. Aucun callback synthétique, timeout ou succès sans producteur n’est inventé. Il faut une route REMOVE active associée contenant le propriétaire et une réception réelle par le fournisseur (`bus_received_at_100ns` peut valoir zéro), pas une fin inférieure. Une mise en file inférieure avant réception fournisseur est hors profil ; ce contrôle n’est pas OutsideRemoveDevice/Driver Verifier complet. Fichiers fermés et anciennes requêtes terminées restent requis avant REMOVE, mais les callbacks libérant les verrous peuvent subsister. La route reste retenue pendant attente, détachement/suppression et fin inférieure pending jusqu’au retour des cadres restants.

Stockage inconnu/incompatible, libération sans acquisition, drain répété, réinitialisation ou suppression avec acquisitions/attente drain non consommée échouent avant mutation. Un échec AddDevice propre peut supprimer un verrou initialisé inutilisé. Les verrous ne remplacent pas les références réelles périphérique/worker ; l’enregistrement ne disparaît qu’à la libération physique de l’extension. Les métadonnées debug n’activent pas les limites temps/high-water du Verifier. Le véritable `driver_wdm_remove_lock.c` utilise `NEVERD_WDM_REMOVE_LOCK_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`. Les artefacts absents sont explicitement ignorés ; les preuves natives et C API/CLI restent Linux uniquement, sans gestionnaire complet de suppression ni drainage général d’E/S concurrentes.

Les requêtes d’alimentation WDM utilisent `kind: "power"` et un `device_id` configuré. Chaque paquet exige explicitement `minor` (`query`/`set`), `power_type` (`device`/`system`), `power_state` (`D0`/`D3` ou `working`/`sleeping3`), `power_action` (`none`/`sleep`), `system_context` (entier 32 bits ou chaîne hexadécimale) et `bus_completion`. System Query vers Working n’est pas pris en charge ; les champs fichier, transfert et annulation sont interdits. Tout `system_context` reste une donnée opaque, sans déduire de parent, d’hibernation ou de démarrage rapide. Les routes exigent `DO_POWER_PAGABLE` sans `DO_POWER_INRUSH` ; le dispatch et `PoRequestPowerIrp` s’exécutent à `PASSIVE_LEVEL`. `PoCallDriver` transmet le même IRP géré ; `PoStartNextPowerIrp` suit le contrat Vista+ sans protocole de sérialisation supplémentaire. Politique générale d’alimentation, WAIT_WAKE, autres états/actions, arrêt/hibernation, courant d’appel, routes non paginables, matériel général et KMDF PnP restent exclus.

Chaque entrée `pnp_devices` peut préciser `initial_reported_device_power: "D0"` ou `"D3"`, indépendamment du cycle de vie initial obligatoire D0/working. Le PDO et chaque DEVICE_OBJECT invité nouvellement associé possèdent chacun leur état de notification ; `PoSetPowerState` retourne et modifie uniquement la valeur précédente du périphérique appelant. Sans valeur explicite, l’appel échoue au lieu de supposer D0. `requested_device_power` contient facultativement des modèles device avec les six mêmes données obligatoires,64 au total pour tous les PDO. Seul un vrai `PoRequestPowerIrp` correspondant au PDO, minor et état cible consomme la tête FIFO du PDO. Une entrée absente/incompatible échoue ; une entrée inutilisée ne crée rien, et le contexte ne détermine pas de parent. Chaque enfant a son propre IRP et sa ligne : `origin: "PoRequestPowerIrp"`, `response_index` à partir de zéro ; les lignes du scénario utilisent `origin: "scenario"` et un index null. Un enfant synchrone peut appeler le callback void à cinq arguments avant le retour `STATUS_PENDING`. Les callbacks peuvent attendre ; System S0 peut finir avant son enfant D0 indépendant. Le snapshot IO_STATUS_BLOCK reste valide jusqu’au retour du callback.

Les rapports ajoutent `power` nullable ; les lignes d’alimentation ont `file: null`. `power` contient les faits du paquet, `device_state_before`/`device_state_after`, `system_state_before`/`system_state_after`, `requested_device_object` nullable et les observations réelles `bus_status`/`bus_received_at_100ns`/`bus_completed_at_100ns`. Les états finaux PnP ajoutent `device_power`/`system_power`, les périphériques vivants `reported_device_power` nullable. `scenario_success` compte seulement les lignes du scénario face à la configuration, mais exige la réussite de toutes les requêtes réellement créées, enfants compris ; les modèles inutilisés ne constituent pas un échec. Le véritable `driver_wdm_power.c` utilise `NEVERD_WDM_POWER_FIXTURE`/`NEVERD_WDM_POWER_CFG_FIXTURE` pour les tests natifs et C API/CLI normaux/active-CFG. Les artefacts absents sont explicitement ignorés ; les preuves d’exécution restent limitées à Linux.

Le [scénario complet d’alimentation](../examples/driver-power-scenario.json), passé avec `--scenario`, exécute le vrai pilote : démarrage, requête système/veille/reprise et suppression, avec trois réponses enfants explicites.

KMDF 1.33 utilise exactement l’ABI 1.33.0 : 458 entrées de fonction possèdent une identité invitée stable, et les 52 API ci-dessous ont une sémantique d’exécution. `WdfVersionBind` et `WdfVersionUnbind` gèrent les liaisons invitées autour du véritable wrapper WDK `FxDriverEntry`. `WdfGetDriver` lit les variables globales publiques du pilote. Pilotes non-PnP, objets génériques, périphériques de contrôle, files et requêtes entrantes partagent contextes typés, compteurs de références et callbacks de nettoyage/destruction/déchargement exécutés. Tous les appels et callbacks modélisés du framework exigent actuellement `PASSIVE_LEVEL` ; l’ajout de références après la fin du nettoyage reste hors de ce profil. Les entrées non modélisées, `WdfLdrQueryInterface`, les extensions de classe et UMDF arrêtent explicitement l’exécution.

Les périphériques de contrôle exigent un nom copié en ASCII imprimable et exactement la SDDL `D:P(A;;GA;;;WD)`. Elle accorde un accès universel sans inventer de jeton d’appelant ; les autres descripteurs de sécurité, périphériques sans nom et noms automatiques ne sont pas pris en charge. L’initialisation possède un périphérique WDM. Les requêtes peuvent le sélectionner dans l’espace de noms de session existant via les alias de liens symboliques `\DosDevices\Name` ou `\??\Name` ; le rapport conserve le nom canonique du périphérique. Une création réussie consomme l’objet d’initialisation et efface son pointeur ; un échec annule la propriété partiellement établie du périphérique. `WdfControlFinishInitializing` autorise la transmission des E/S. La suppression retire le périphérique et ses liens uniquement lorsque les fichiers, éléments de travail et requêtes modélisés le permettent ; l’annulation ou la vidange des requêtes pendant la suppression reste exclue.

La structure `WDF_IO_QUEUE_CONFIG` de 96 octets prend en charge les files par défaut ou non, manuelles, séquentielles et parallèles à limite finie ou illimitée, avec exécution passive explicite et sans synchronisation du framework. Les files des périphériques de contrôle ne sont pas gérées par l’alimentation. Les callbacks READ/WRITE/IOCTL spécifiques priment sur le callback par défaut. Les requêtes acceptées dans la file renvoient `STATUS_PENDING` même en cas d’achèvement synchrone ; le registre de retour d’un callback void n’achève pas sa requête. L’achèvement différé utilise l’ordonnanceur existant. Sans gestionnaire, la requête s’achève avec `STATUS_INVALID_DEVICE_REQUEST` ; READ/WRITE de longueur nulle s’achève sans transmission, sauf activation de celle-ci. Le paquet de fichiers par défaut achève CREATE/CLEANUP/CLOSE avec succès et Information=0. Les files manuelles non définies par défaut reçoivent les requêtes via `WdfRequestForwardToIoQueue` ; `WdfIoQueueRetrieveNextRequest` les rend dans l’ordre FIFO. Si une requête est annulée avant sa récupération, le framework la retire et la termine avec `STATUS_CANCELLED`. Les files automatiques non définies par défaut transmettent les requêtes transférées à leurs propres callbacks ; la file manuelle par défaut retient les requêtes entrantes jusqu’à récupération. `WdfIoQueueRetrieveNextRequest` fonctionne avec les files manuelles et séquentielles ; les files parallèles renvoient `STATUS_INVALID_DEVICE_STATE`. Sans callback correspondant, la file automatique achève la requête avec `STATUS_INVALID_DEVICE_REQUEST` dès qu’une place de présentation se libère. Les callbacks de fichier, les périphériques PnP et PnP/alimentation complet restent exclus. `WdfRequestRequeue` replace une requête récupérée en tête de la même file manuelle. `NumberOfPresentedRequests` limite les requêtes présentées en parallèle ; les autres attendent la fin ou l’annulation d’une requête présentée. Une file séquentielle par défaut accepte d’autres requêtes pendant qu’une requête est présentée ; elles attendent en ordre FIFO qu’une place se libère et peuvent être annulées avant transmission. `WdfIoQueueStop` suspend la transmission tout en continuant à accepter les requêtes. `WdfIoQueueStart` transmet les requêtes en attente et `WdfIoQueueGetState` donne les nombres en file et déjà transmis. La récupération pendant l’arrêt renvoie `STATUS_WDF_PAUSED` ; le callback de fin d’arrêt reçoit le contexte fourni lorsque toutes les requêtes déjà transmises sont terminées ou ont quitté la file ; les requêtes en attente ne le retardent pas. Un second enregistrement pendant son attente est rejeté.

`WdfIoQueueDrain` refuse les nouvelles requêtes avec `STATUS_INVALID_DEVICE_STATE` et transmet celles déjà en file. Son callback attend que les nombres de requêtes en file et détenues par le pilote soient nuls. Un transfert vers cette file renvoie `STATUS_WDF_BUSY` ; `WdfIoQueueStart` rétablit l’acceptation.

Les paramètres de requête utilisent la disposition `WDF_REQUEST_PARAMETERS` de 40 octets. Les accesseurs d’entrée/sortie renvoient les longueurs logiques, en conservant les alias des tampons et les mappages MDL existants des E/S directes ; l’entrée d’un IOCTL direct reste tamponnée. Les directions incorrectes et les tampons insuffisants renvoient les statuts documentés. L’achèvement nettoie la requête pendant que les tampons restent valides, termine l’IRP et libère les pages verrouillées de la requête ; les objets enfants et la requête sont ensuite détruits lorsque les références le permettent. Les nouveaux appels aux accesseurs de tampons et paramètres sont refusés dès le début de l’achèvement ; les pointeurs de tampon déjà obtenus restent utilisables pendant le nettoyage. Une référence externe conserve le contexte, sans maintenir l’accès à l’IRP achevé.

L’annulation est modélisée pour les requêtes des files de périphériques de contrôle décrites ci-dessus. Si l’annulation a déjà eu lieu, `WdfRequestMarkCancelableEx` renvoie `STATUS_CANCELLED` sans appeler de callback. Un `WdfRequestUnmarkCancelable` réussi retire le callback ; une annulation ultérieure enregistre seulement l’état annulé. `WdfRequestIsCanceled` lit cet état sur une requête vivante non marquée annulable. Après un marquage réussi, l’achèvement exige un retrait réussi ou le début de livraison du callback d’annulation : sa simple mise en file ne suffit pas. Une fois livré, le callback peut coordonner l’achèvement avec un élément de travail, même s’il attend. Une référence interne distincte conserve la requête jusqu’au retour du callback ; l’achèvement invalide néanmoins l’IRP en premier, et la continuation finale de destruction peut elle-même attendre. Les DPC ont priorité, puis les callbacks d’annulation en ordre FIFO, puis les éléments de travail ordinaires ; les callbacks d’annulation passent aussi avant la reprise des attentes passives prêtes.

Pour une requête déjà annulée, l’ancien `WdfRequestMarkCancelable`, de type void, exécute un callback invité synchrone avant de revenir. Cette continuation enfant peut attendre, achever la requête via un nettoyage imbriqué et exécuter la destruction finale avant la reprise de l’API. Une annulation postérieure à l’enregistrement suit le chemin planifié décrit plus haut. Cela reproduit le code public à `PASSIVE_LEVEL` avec `WdfSynchronizationScopeNone` ; [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) demande aux pilotes sans synchronisation automatique d’utiliser Ex. Cette exécution assure la compatibilité, sans recommander l’ancien appel dans cette configuration.

`WdfRequestGetInformation` et `WdfRequestSetInformation` partagent le champ 64 bits `IoStatus.Information` de l’IRP d’origine, y compris les écritures directes invitées. Set affecte la valeur ; la longueur de transfert est vérifiée à l’achèvement. `WdfRequestCompleteWithInformation` écrit ce même champ avant le nettoyage ; les modifications via un IRP préalablement conservé pendant le nettoyage déterminent l’Information finale, même si GetInformation renvoie déjà zéro à ce stade. `WdfRequestGetIoQueue` renvoie la file d’origine. Avec la configuration de fichiers par défaut, `WdfRequestGetFileObject` renvoie NULL sans inventer d’objet fichier WDF à partir du FILE_OBJECT WDM. `WdfRequestWdmGetIrp` renvoie le même IRP ; les appels invités `IoCompleteRequest`/`IofCompleteRequest` ne peuvent contourner l’achèvement WDF. Tant que le handle reste valide pendant ou après l’achèvement, GetInformation/GetIoQueue renvoient zéro ; la récupération MDL efface d’abord un emplacement de sortie valide vers NULL, puis renvoie `STATUS_INTERNAL_ERROR`. SetInformation/GetFileObject/WdmGetIrp restent alors refusés. Les restrictions des accesseurs de tampons et paramètres restent inchangées.

`WdfRequestRetrieveInputWdmMdl` et `WdfRequestRetrieveOutputWdmMdl` décrivent à la demande le SystemBuffer existant pour l’entrée WRITE, la sortie READ et les entrées/sorties IOCTL tamponnées. Chaque direction doit être valide et non vide avant l’emploi de l’unique descripteur en cache par requête ; la première récupération fixe ByteCount, même si l’autre direction a une longueur logique différente. `MmGetSystemAddressForMdlSafe` renvoie son adresse virtuelle d’origine ; mappage supplémentaire, démappage et libération par le pilote sont refusés. Les sorties READ et IOCTL directes ainsi que l’entrée WRITE directe renvoient plutôt `IRP.MdlAddress` existant, sans créer de mappage lors de la récupération ; l’entrée IOCTL directe utilise le cache SystemBuffer. Descripteurs, IRP et tampons expirent à l’achèvement. Les références internes d’annulation ou externes conservent seulement le contexte WDF. La récupération MDL de WDF pour `METHOD_NEITHER` reste exclue ; le WDFMEMORY associé à la requête utilise un mappage séparé de pages verrouillées. Les tableaux PFN des descripteurs construits exposent les identités du modèle en lecture seule ; l’accès aux PFN non construits est refusé.

Pour les requêtes KMDF tamponnées, directes et neither, `WdfDeviceInitSetIoInCallerContextCallback` s’exécute avant la file dans le processus demandeur à `PASSIVE_LEVEL`. Le callback doit terminer la requête ou appeler une fois `WdfDeviceEnqueueRequest` pour la file par défaut. Pour un IOCTL `METHOD_NEITHER` ou une lecture/écriture neither, `WdfRequestRetrieveUnsafeUserInputBuffer` et `WdfRequestRetrieveUnsafeUserOutputBuffer` donnent les adresses utilisateur d’origine uniquement dans ce callback. `WdfRequestProbeAndLockUserBufferForRead` et `WdfRequestProbeAndLockUserBufferForWrite` vérifient les droits des pages et les verrouillent pour la requête ; `WdfMemoryGetBuffer` fournit un alias système utilisable dans le callback de file hors du contexte demandeur. L’achèvement libère les pages et alias. Les pointeurs utilisateur intégrés et les mappages arbitraires restent exclus.

API KMDF modélisées: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetIoType`, `WdfDeviceInitSetIoInCallerContextCallback`, `WdfDeviceCreate`, `WdfDeviceEnqueueRequest`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfIoQueueGetDevice`, `WdfIoQueueGetState`, `WdfIoQueueStop`, `WdfIoQueueStart`, `WdfIoQueueDrain`, `WdfIoQueueRetrieveNextRequest`, `WdfRequestForwardToIoQueue`, `WdfRequestRequeue`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveUnsafeUserInputBuffer`, `WdfRequestRetrieveUnsafeUserOutputBuffer`, `WdfRequestProbeAndLockUserBufferForRead`, `WdfRequestProbeAndLockUserBufferForWrite`, `WdfMemoryGetBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

La validation WDK facultative compile séparément `driver_kmdf_lifecycle.c` et `driver_kmdf_control.c` avec la véritable bibliothèque d’entrée KMDF. `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` sélectionnent les images de cycle de vie ; `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` sélectionnent les images de périphérique de contrôle normale/CFG actif. Les artefacts externes absents entraînent un saut explicite. Voir les [tests](testing.md) pour la couverture native et C API/CLI. Les preuves d’exécution actuelles se limitent aux hôtes Linux.

Le modèle initial d’API possède volontairement un contrat limité :

| API | Comportement modélisé et restrictions |
|-----|--------------------------------------|
| `RtlInitUnicodeString` | Construit une `UNICODE_STRING` invitée pour une source bornée terminée par NUL |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Copie UTF-16 avec longueur explicite et comparaison sensible à la casse ; une comparaison insensible à la casse exige une table de casse Windows et provoque un arrêt |
| `ExRaiseStatus`, `ExRaiseAccessViolation`, `ExRaiseDatatypeMisalignment` | Lèvent une exception invitée pour les handlers C `__except` constants pris en charge ; aucun retour API normal, filtres/finally et reprise après défaut CPU restent exclus |
| `ExAllocatePool2` | Allocations NX paginées/non paginées, initialisées à zéro par défaut ; indicateurs de non-initialisation et d’alignement sur le cache modélisés ; des indicateurs requis invalides renvoient NULL ; les pools à quotas/exécutables et les exceptions d’allocation provoquent un arrêt |
| `MmGetSystemRoutineAddress` | Résout un nom invité de longueur explicite via l’inventaire partagé des exports |
| `MmMapIoSpace`, `MmMapIoSpaceEx`, `MmUnmapIoSpace` | Sous-plages traduites déclarées ; RO/RW non caché, alias partagés et unmap exact ; aucune mémoire physique arbitraire |
| `IoConnectInterrupt`, `IoDisconnectInterrupt`, `IoConnectInterruptEx`, `IoDisconnectInterruptEx` | Ligne latched exclusive exactement affectée, ancienne ABI et versions Ex 1/2/4 à PASSIVE_LEVEL ; connexion opaque et durée précise de génération |
| `KeSynchronizeExecution`, `KeAcquireInterruptSpinLock`, `KeReleaseInterruptSpinLock` | Véritable callback BOOLEAN et même verrou non récursif au DIRQL affecté ; IRQL et propriété de l’appelant d’origine restaurés |
| `IoGetDmaAdapter` | Description explicite Internal maître de bus version 0/1 et table liée version 1 à PASSIVE_LEVEL ; les sondages de versions plus récentes renvoient NULL |
| `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `PutDmaAdapter` | Méthodes de table sur RAM cohérente partagée, identité exacte d’allocation et durée de vie indépendante de l’adaptateur |
| `GetScatterGatherList`, `PutScatterGatherList` | Méthodes de table à DISPATCH_LEVEL ; vrai callback immédiat ou en attente de ressources, vue MDL retenue et libération explicite du mappage |
| `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters` | Callbacks de canal de maître de bus traduit, quota partagé de registres, fragments MDL contigus, vidage global et libération exacte des registres conservés |
| `KeFlushIoBuffers` | Vidage cohérent du cache CPU sur un MDL actif verrouillé/non paginé, sans libérer les mappages DMA |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | Arborescence de registre explicite limitée à la session, droits par handle, requêtes avec sorties de taille précise et durée de vie après suppression ; voir les scénarios de registre |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | MDL propres aux requêtes avec mappages KernelMode en cache et permissions ; les MDL du pool non paginé réutilisent le mappage initial via la macro sûre |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | Descripteurs autonomes, plage complète dans une allocation active du pool non paginé, durées de vie indépendantes du descripteur et du tampon ; sans IRP, chaîne ni quota |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Allocations de données pour les types de pool `0`, `1` et `512` ; taille/tag positifs, tags correspondants lors des libérations avec tag, aucune réutilisation d’adresse |
| `IoCreateDevice`, `IoDeleteDevice` | Type de périphérique `0x22`, caractéristiques `0` ou `0x100`, extensions bornées, noms ASCII `\Device\Name` |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | Attachement du même pilote ; renvoie l’ancien sommet, le détachement reçoit le périphérique inférieur mémorisé ; limites ci-dessus |
| `IofCallDriver`, `IoCallDriver` | Dispatch vers la cible exacte de la route retenue ; curseur validé et NTSTATUS inférieur distinct |
| `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx` | Propriétaire exact dans l’extension,32/120 octets opaques, Tags NULL/répétés |
| `IoReleaseRemoveLockEx`, `IoReleaseRemoveLockAndWaitEx` | Libération concordante et attente REMOVE reprenable après réception fournisseur |
| `PoCallDriver`, `PoStartNextPowerIrp` | Transfert du même IRP géré ; Vista+ sans protocole supplémentaire |
| `PoSetPowerState`, `PoRequestPowerIrp` | Notifications indépendantes et vrais enfants issus des FIFO PDO explicites ; limites ci-dessus |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` ou `\??\Name` dans un espace de noms de session, ciblant `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Formatage variadique Win64 vérifié, au plus 512 octets en sortie ; tous les filtres du débogueur sont activés |
| `IoGetCurrentIrpStackLocation` | Renvoie l’emplacement de pile de l’IRP modélisé actif ; les macros WDM compilées habituelles lisent le même champ invité |
| `KeGetCurrentIrql` | Lit l’IRQL/CR8 courant, y compris les élévations et restaurations explicites ; le dispatch et les travaux démarrent à `PASSIVE_LEVEL`, les DPC à `DISPATCH_LEVEL` |
| `KfRaiseIrql`, `KeLowerIrql` | Vrais imports WDK x64 de montée/descente ; l’IRQL sauvegardé doit être restauré en ordre LIFO sur la même exécution avant retour ou suspension. Les lectures de CR8 voient chaque changement ; pas de préemption instruction par instruction. |
| `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`, `KeReleaseSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `KeReleaseSpinLockFromDpcLevel`, `KeTryToAcquireSpinLockAtDpcLevel` | Verrous exécutifs résidents et alignés sur CPU0 ; propriétaire, appariement acquisition/libération et restauration de l’IRQL sont vérifiés. Une acquisition bloquante en contention s’arrête explicitement. |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | Éléments opaques appartenant au périphérique ; `DelayedWorkQueue` uniquement, périphérique et contexte passés à `PASSIVE_LEVEL` ; un élément encore en file ne peut être libéré |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | DPC opaque, quatre arguments invités, `DISPATCH_LEVEL`, doublons/retrait et importance ; cible CPU0 uniquement |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | Timers notification/synchronisation ; échéances relatives/absolues en 100 ns, périodes en millisecondes, réarmement/annulation et signaux en temps virtuel |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | Événements notification/synchronisation avec consommation distincte ; `KeSetEvent` accepte uniquement Increment=0 et Wait=FALSE |
| `KeInitializeSemaphore`, `KeReleaseSemaphore`, `KeReadStateSemaphore` | Sémaphore compteur résident avec limite positive ; chaque attente réussie consomme une unité. Libération avec Increment=0 et Wait=FALSE ; dépasser la limite lève `STATUS_SEMAPHORE_LIMIT_EXCEEDED`. |
| `KeInitializeMutex`, `KeReleaseMutex`, `KeReadStateMutex` | KMUTEX résident avec acquisition récursive propre à une exécution ; KeReleaseMutex renvoie l’état signé précédent, exige le propriétaire et le même contexte DISPATCH_LEVEL, et accepte seulement Wait=FALSE. Un mutex détenu interdit retour, réinitialisation et libération du stockage. Une libération par un autre exécutant lève `STATUS_MUTANT_NOT_OWNED`. |
| `PsCreateSystemThread`, `PsTerminateSystemThread`, `ObReferenceObjectByHandle`, `ObfDereferenceObject`, `ZwClose` | Threads bornés du processus système à PASSIVE_LEVEL. Le handle et la référence à l’objet thread opaque ont des durées de vie distinctes ; PsTerminateSystemThread termine sans retour et signale l’objet attendable. APC, priorités et références typées ne sont pas modélisés. |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeEnterGuardedRegion`, `KeLeaveGuardedRegion`, `KeAreApcsDisabled`, `KeAreAllApcsDisabled` | État imbriqué de désactivation APC par thread. Les régions critiques et un KMUTEX détenu bloquent les APC normaux ; les régions protégées et IRQL >= APC_LEVEL bloquent tous les APC. Les threads système démarrent dans une région critique. Sorties non appariées et retours déséquilibrés échouent ; la livraison APC n’est pas modélisée. |
| `KeWaitForSingleObject` | Un événement, timer, sémaphore ou mutex initialisé ; `KernelMode` non alertable, raison `Executive` ; polling zéro, attente relative/absolue finie ou infinie ; attente non nulle/infinie à IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | Délai relatif/absolu `KernelMode` non alertable à IRQL <= APC_LEVEL ; reprise du cadre invité après progression du temps virtuel |
| `IoMarkIrpPending` | Marque l’IRP actif ; l’écriture équivalente de la macro WDM dans le contrôle de pile est aussi modélisée ; le dispatch doit retourner `STATUS_PENDING` |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT` ; déroule la fin avec arrêt/reprise et libère IRP/MDL/tampons uniquement à la limite finale |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Opérations bornées sur les tampons invités, au plus 1 MiB par appel ; les API de copie sans chevauchement rejettent les chevauchements |

Les plafonds IRQL proviennent de `KernelAPIIRQL.def` ; le modèle propriétaire vérifie les restrictions dépendant des arguments. Un DPC ne peut ni appeler le registre ni allouer, libérer ou accéder au pool paginé. Les conversions Unicode de `DbgPrint` exigent `PASSIVE_LEVEL`, tandis que l’ANSI et les opérations non paginées pris en charge restent utilisables à `DISPATCH_LEVEL`. Les piles sont bornées : un pointeur de pile sortant ne peut atteindre celle d’un autre worker bloqué. Un timer armé dans l’extension empêche la destruction prématurée du périphérique.

L’expiration satisfait les attentes déjà inscrites avant qu’un DPC puisse réinitialiser ou réarmer le timer. Les DPC en file passent avant la reprise des cadres `PASSIVE_LEVEL` réveillés. Si le stockage d’une requête contient encore un DPC en file, l’achèvement IRP refuse sa libération avant de terminer ou d’invalider le tampon.

Le formatage de `DbgPrint` prend en charge les entiers `d/i/u/o/x/X`, les
pointeurs `p`, le texte `s/c`, `%%`, l’Unicode de longueur explicite `wZ/lZ`,
les chaînes larges `ls/ws`, les indicateurs, largeur/précision dont `*`, et les
modificateurs Windows de longueur des entiers. Au plus 32 arguments variables et
1024 octets de format sont lus. Largeur et précision sont limitées à 512.
Les flottants, `%n`, les combinaisons inconnues et les conversions de texte non
ASCII provoquent un arrêt explicite ; le modèle ne devine pas la page de codes
Windows et n’appelle pas le printf hôte avec des données invitées.

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

Le nom du périphérique et le code IOCTL doivent correspondre au pilote.
Lors de create, l’omission de `device` sélectionne le seul périphérique actif ;
une sélection ambiguë échoue. Les requêtes suivantes utilisent le périphérique
de leur fichier, sauf si un nom explicite correspondant est fourni. Le champ
facultatif `file` est une identité de scénario non signée sur 32 bits, nulle par
défaut. Chaque identité possède ses propres FILE_OBJECT et FsContext et exige
create, transferts, cleanup puis close dans cet ordre. Les requêtes de fichiers
indépendants peuvent être entrelacées. Les périphériques exclusifs rejettent une
seconde ouverture. Ces identités représentent des objets fichiers, et non des
handles dupliqués. Les méthodes IOCTL bufferisée et les deux méthodes directes
sont prises en charge. Le dispatch doit terminer de façon synchrone ou respecter le contrat de callbacks en attente décrit ci-dessus. Les longueurs de sortie invalides et l’accès à un IRP terminé provoquent un échec explicite. Le déchargement
demandé ne doit laisser aucun périphérique, lien symbolique, allocation de pool
ou objet fichier actif.

Le champ racine optionnel `"load_address": "0x190000000"` demande un changement
de base ; son absence ou `"0x0"` utilise l’adresse préférée. L’image doit
satisfaire les exigences de relocalisation. La commande d’initialisation et
l’API C d’origine n’impliquent aucun scénario.

Seuls `load_address`, `requests`, `unload`, `kernel_exports`, `registry` et `pnp_devices` sont acceptés
à la racine. Les requêtes ordinaires de fichier acceptent `kind`, ainsi que `device` ou `device_id` (exclusifs) et `file`, facultatifs. Les IOCTL exigent `code` et acceptent `input`,
`output_size` et `direct_input`. Un `read` accepte `output_size` et `byte_offset` ;
un `write` accepte `input` et `byte_offset`. Les offsets valent zéro par défaut,
acceptent des entiers ou des chaînes hexadécimales et doivent tenir dans une
valeur signée non négative sur 64 bits. Les requêtes de cycle de vie rejettent
les champs de transfert. Les champs inconnus ou dupliqués sont rejetés.
`code` accepte un entier JSON non signé sur 32 bits ou une chaîne hexadécimale
`0x`. `input` est une chaîne hexadécimale de longueur paire, sans préfixe ni
espaces ; son omission signifie une entrée vide. `output_size` est un entier
JSON non signé ; son omission signifie zéro. Les fractions et les notations
à virgule flottante sont rejetées.

Seules les requêtes READ/WRITE/IOCTL acceptent `cancel_after_100ns`, entier JSON facultatif compris entre 0 et `INT64_MAX` (9223372036854775807). Le délai est relatif à la soumission de la requête, en unités virtuelles de 100 ns, et non en temps réel. Pour KMDF, zéro applique l’annulation après le routage du framework et avant le callback d’E/S invité ; si le routage a déjà achevé la requête, l’achèvement l’emporte. Pour WDM, zéro s’applique après le retour du dispatch. Pour un délai positif, le temps n’avance vers une échéance de timer, d’attente ou d’annulation que si aucun callback ni contexte n’est prêt. Un IRP WDM en attente appelle sa routine d’annulation enregistrée à `DISPATCH_LEVEL`, verrou d’annulation détenu ; elle doit le libérer avec `Irp->CancelIrql` avant l’achèvement. L’annulation générale des files et du PnP reste exclue. Chaque rapport de requête contient `cancel_requested_at_100ns`, soit l’heure virtuelle absolue de l’annulation effective, soit null si elle n’a pas eu lieu, notamment si l’achèvement a précédé l’annulation. Demander l’annulation ne suffit pas à achever l’IRP ni à imposer son statut final.
`IoSetCancelRoutine`, `IoAcquireCancelSpinLock`, `IoReleaseCancelSpinLock` et `IoCancelIrp` partagent l’état de l’IRP et le verrou d’annulation ; `IoCancelIrp` appelle la routine enregistrée de façon synchrone et indique si elle a été exécutée.

Le booléen facultatif `user_unmap_after_dispatch` retire l’accès aux adresses utilisateur originales d’un transfert WDM neither non vide après le retour du dispatch et avant le travail ou l’annulation planifiés. Les pages verrouillées par MDL et leurs alias système restent utilisables jusqu’au déverrouillage ; les pointeurs utilisateur bruts et les nouveaux verrouillages échouent. Si la sortie est révoquée, `output_hex` est vide. La réutilisation et les instants arbitraires de retrait ne sont pas modélisés.

`requestor_process_id` identifie un processus demandeur synthétique (4096 par défaut, de 5 à `UINT32_MAX`). `IoGetRequestorProcessId` donne son ID pour un IRP actif ; `PsGetCurrentProcessId` donne cet ID pendant la distribution directe ou 4 dans le worker système modélisé. Changer de processus bloque les adresses utilisateur d’origine d’un autre processus, mais conserve les alias système des MDL verrouillés. `requestor_exit_after_dispatch` révoque après la distribution toutes les adresses utilisateur d’origine de ce processus et refuse ses nouvelles E/S ; CLEANUP/CLOSE explicites restent possibles, sans déduire l’annulation ni la fermeture automatique des handles.

Un travailleur système peut obtenir le processus opaque d’un IRP actif avec `IoGetRequestorProcess`, puis utiliser `KeStackAttachProcess` avec un `KAPC_STATE` noyau inscriptible pour accéder temporairement aux adresses utilisateur initiales. Il doit appeler `KeUnstackDetachProcess` avec le même état. `IoGetCurrentProcess` et `PsGetProcessId` indiquent le processus attaché, tandis que `PsGetCurrentProcessId` reste 4, le créateur du travailleur. Un processus quitté, un IRP terminé, un état non apparié, ou une attente ou complétion d’IRP pendant l’attachement échoue explicitement.

Pour les IOCTL directs, `input` initialise le premier tampon système,
tandis que `direct_input` initialise le second tampon distinct décrit par le MDL,
complété par des zéros jusqu’à `output_size`. `METHOD_IN_DIRECT` exige un accès
en lecture ; il n’implique pas un mappage système en lecture seule. Les deux
méthodes utilisent des tampons de scénario accessibles en lecture/écriture.
`MdlMappingNoWrite` retire le droit d’écriture du mappage et `MdlMappingNoExecute`
retire le droit d’exécution. Le démappage révoque l’adresse virtuelle système ;
un nouveau mappage conserve les mêmes données verrouillées. L’achèvement rend
le MDL et le mappage caducs. Les champs publics du MDL utilisés par les macros
WDM sont modélisés ; les champs de processus, les PFN de descripteurs non construits, les MDL construits manuellement,
les mappages utilisateur et l’accès direct via le UserBuffer brut sont rejetés.
Un tampon direct de longueur nulle possède un MDL nul.

`IoAllocateMdl` alloue des métadonnées autonomes pour un tampon non vide, sans débordement et limité à 1 MiB ; il ne sonde ni ne verrouille ce tampon. `Irp` doit être NULL, `SecondaryBuffer` et `ChargeQuota` doivent être FALSE. L’épuisement de l’arène renvoie NULL. `MmBuildMdlForNonPagedPool` exige que toute la plage décrite appartienne à une seule allocation active du pool non paginé. La macro sûre et les macros WDM ordinaires réutilisent l’adresse originale, préservant les alias et les permissions existantes même avec de nouveaux indicateurs interdisant l’écriture ou l’exécution. Les mappages système supplémentaires et le démappage sont rejetés. `IoFreeMdl` invalide uniquement le descripteur ; le tampon du pool possède sa propre durée de vie. Les deux ordres de libération sont possibles si aucun stockage libéré n’est ensuite utilisé. Tous les champs MDL modélisés et les tableaux PFN construits sont en lecture seule ; les champs de processus, les PFN non construits, les chaînes et les modifications manuelles restent non pris en charge. Le déchargement doit libérer chaque descripteur appartenant au pilote.

Pour toute méthode IOCTL avec un `output_size` non nul, `Information` ne doit
pas dépasser `output_size`, même si le tampon d’entrée est plus grand. Sans
tampon de sortie, `Information` peut contenir un résultat propre à l’IOCTL sur
64 bits ; aucun octet de sortie n’est copié. Le rapport conserve cette valeur
exacte dans `information_hex`.

Pour READ/WRITE, `DO_BUFFERED_IO` ou `DO_DIRECT_IO` sélectionne la méthode
de transfert tamponnée ou directe. Si aucun indicateur n’est défini, l’adresse
utilisateur originale n’apparaît que dans `IRP.UserBuffer` : WRITE utilise
l’entrée et READ la sortie ; aucun SystemBuffer ni MDL n’est créé implicitement.
Le pilote doit vérifier et utiliser l’adresse dans le contexte de l’appelant,
ou verrouiller les pages avant de reporter le travail. `user_input_access`
s’applique à neither WRITE et `user_output_access` à neither READ. Des droits
explicites pour READ/WRITE tamponné/direct ou des indicateurs contradictoires
provoquent un arrêt. Information est vérifié par rapport à la longueur ; les écritures
renvoient un nombre d’octets et les lectures renvoient des octets.

`kernel_exports` associe les noms de routines à des booléens de disponibilité
explicites, par exemple `"kernel_exports": {"OptionalRoutine": false}`. Les
exports modélisés et les imports statiques reçoivent des adresses stables partagées
avec `MmGetSystemRoutineAddress`. Un export explicitement absent est résolu en
NULL et ne peut pas satisfaire un import statique. Un export déclaré présent
sans modèle d’API est résolu vers un piège déclenché à l’appel. Un nom dynamique
inconnu provoque un arrêt avec un diagnostic de disponibilité non spécifiée ;
l’absence n’est jamais déduite du manque d’implémentation. Les noms sont en ASCII
imprimable de longueur bornée et la résolution est sensible à la casse.
L’inventaire est une propriété concrète du scénario, sans prétendre correspondre
à toutes les versions de Windows.
`IoMarkIrpPending`, `IoGetCurrentIrpStackLocation` et `MmGetSystemAddressForMdlSafe` sont des fonctions auxiliaires des en-têtes WDM modélisées ; cela ne les déclare pas exportées par défaut, leur disponibilité comme exports nécessitant un import statique ou une déclaration explicite dans `kernel_exports`.

Le texte du scénario est limité à 2 MiB, avec au plus 64 requêtes, 65536 octets
par tampon d’entrée ou de sortie et 512 KiB d’octets demandés au total, contenu
de `direct_input` compris. Les budgets d’instructions, d’observations, de mémoire
invitée et de temps s’appliquent à l’ensemble du scénario. L’arène de 1 MiB
contient aussi les objets et métadonnées : une image peut donc épuiser la mémoire
du modèle avant d’atteindre le maximum prévu pour les tampons du scénario.

## Scénarios de registre

Le tableau facultatif `registry` définit une arborescence concrète du registre,
limitée à la session. Chaque clé comporte un `path` obligatoire et un tableau
`values` facultatif ; chaque valeur comporte `name`, un entier non signé `type`
et des données hexadécimales `data`. Un nom vide désigne la valeur par défaut.
Par exemple, une valeur DWORD s’écrit
`{"name":"Mode","type":4,"data":"01000000"}`. Les octets sont conservés tels quels ;
le modèle ne corrige pas les terminateurs de chaînes et ne développe pas les
variables d’environnement.

Les chemins doivent être absolus, en ASCII, sous `\Registry\Machine` ou
`\Registry\User`. Les clés ancêtres sont créées implicitement. Les identités
des clés et des valeurs sont comparées sans distinction de casse selon les
règles ASCII ; les noms non ASCII et les identités dupliquées sont rejetés.
L’omission du tableau laisse la disponibilité du registre indéterminée et les
appels au registre s’arrêtent. `"registry": []` décrit explicitement un espace
de noms vide. Aucune clé, valeur, donnée du registre hôte ou configuration de
service n’est déduite du pilote.

`ZwOpenKey` et `ZwCreateKey` renvoient des handles opaques indépendants, avec
vérification des droits de chaque handle pour la lecture, l’écriture, la création
de sous-clés et la suppression. L’arborescence configurée accorde les bits pris
en charge de `KEY_ALL_ACCESS`, y compris les masques usuels `KEY_READ` et
`KEY_WRITE`. Il s’agit d’une arborescence de test explicitement accessible, sans
ACL Windows ni évaluation des privilèges. Les droits génériques,
`MAXIMUM_ALLOWED`, les vues alternatives du registre, les descripteurs de
sécurité personnalisés, les classes et les liens symboliques ne sont pas pris
en charge. La création relative exige un handle du parent direct possédant
`KEY_CREATE_SUB_KEY`. Les clés d’entrée sont non volatiles ; les clés créées
peuvent être volatiles, mais une sous-clé non volatile d’une clé volatile est
rejetée. Aucun redémarrage ni stockage persistant sur disque n’est modélisé.

`ZwQueryValueKey` implémente les classes d’information Basic, Full et Partial,
ainsi que leurs variantes Align64 définies, avec des longueurs exactes, des
données alignées, une sortie partielle et des résultats distincts
`STATUS_BUFFER_TOO_SMALL` et `STATUS_BUFFER_OVERFLOW`. `ZwSetValueKey` et
`ZwDeleteValueKey` modifient uniquement l’arborescence de cette session.
`ZwDeleteKey` rejette une clé possédant des sous-clés actives ; les handles d’une
clé supprimée renvoient `STATUS_KEY_DELETED` jusqu’à leur fermeture. `ZwClose`
libère un handle indépendamment de la clé ; le déchargement demandé échoue tant
que des handles de registre restent ouverts.

Les limites sont de 256 clés, ancêtres compris, 1024 valeurs au total,
65536 octets par valeur, 512 KiB de données de valeurs au total, 1024 octets
ASCII par chemin de clé, 256 octets par nom de valeur et 256 handles ouverts
simultanément. Les créations et modifications respectent les mêmes limites que
la validation préalable du scénario. `configuration.registry` conserve l’entrée
originale dans le rapport ; `registry` énumère les chemins et valeurs des clés
actives à la fin, y compris les modifications observées avant un arrêt. Un état
de registre non spécifié est représenté par null. La volatilité et les identités
des handles ne font pas partie de cet instantané des valeurs.

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
des dépendances de l’objet compilé. La vérification exécute des scénarios
bufferisés, in-direct et out-direct distincts via DriverEntry, create, IOCTL,
cleanup, close et unload. Ajoutez `--debug` et choisissez un répertoire de sortie
distinct pour compiler avec `DBG=1` et vérifier les messages de journalisation
invités. L’exemple amont n’enregistre pas de gestionnaire cleanup ; le gestionnaire
par défaut modélisé termine donc cleanup avec `STATUS_INVALID_DEVICE_REQUEST`
(`0xC0000010`). Le pilote effectue tout de même close et unload, et l’IOCTL réussi
renvoie les octets attendus. Pour ce scénario complet, le code de sortie attendu
de la CLI est **2** et `scenario_success` vaut false. Le script ne réussit que
si tous ces résultats correspondent, y compris l’échec visible de cleanup ;
il ne réécrit pas l’exemple pour masquer ce résultat.

## Vérification d’acceptation avec l’exemple Zero

Le [script de validation Zero](../../scripts/validate_zero_driver_sample.py)
supplémentaire compile l’exemple WDM public Zero de Pavel Yosifovich sans
modification, à partir de la révision et des empreintes indiquées dans
[son manifeste](../../unittests/emulation/fixtures/zero-validation.json).
Exécutez `python3 scripts/validate_zero_driver_sample.py` avec les mêmes
prérequis de compilation. Le code source, la licence MIT, les commandes, le
scénario et le rapport sont conservés par défaut sous
`build-release/driver-validation/zero`. Neuf requêtes exercent les lectures
directes traversant des limites de pages, les nombres d’octets écrits, les
statistiques atomiques invitées et un IOCTL bufferisé de statistiques. L’échec
de lecture de longueur nulle de l’exemple et l’absence de gestionnaire CLEANUP
restent visibles ; le code de sortie attendu de la CLI est 2, avec fermeture
et déchargement réussis. Le script de validation ne réussit que si ces résultats
exacts et tous les octets de sortie correspondent.

## Rapports et SDK

Le rapport JSON distingue `stop_reason`, les champs pouvant être null
`nt_status` et `nt_success`, le PC à l’arrêt et le nombre d’instructions. Il
conserve les appels d’API et l’état observable recueillis avant l’arrêt, notamment
les objets périphériques et les adresses des callbacks du pilote. Les adresses
invitées sont des chaînes hexadécimales afin que les consommateurs JSON ne
perdent pas de précision sur 64 bits. L’objet `configuration` enregistre les
limites, le nom du service, les substitutions `kernel_exports` et l’entrée
`registry` de l’exécution. Le profil est
`wdm-x64-scheduled-v42`. `nt_status` reste le résultat de DriverEntry, tandis
que `scenario_success` décrit conjointement l’initialisation et les requêtes
terminées. `phase`, `requests` et `unload_completed` identifient les parties du
cycle demandé qui ont été exécutées. Chaque appel d’API et écriture CPU indique
aussi sa phase (`driver_entry`, `add_device:<ID>`, `request:N`, `callback:N` ou `unload`). Chaque requête rapporte
les statuts de dispatch et d’E/S, l’achèvement, la valeur Information et
les octets renvoyés dans `output_hex`. `preferred_image_base` décrit la base
PE d’origine. `security_cookie` est l’adresse invitée du cookie initialisé,
ou `"0x0"` si aucun n’était nécessaire. Les champs des requêtes sont `kind`, `device`, `device_id`, `pnp`, `file`, `requestor_process_id`, `byte_offset`, `code`, `irp`, `completed`, `cancel_requested_at_100ns`, `dispatch_status`, `io_status`,
`information`, `information_hex` et `output_hex`. `information` conserve sa
forme numérique ; `information_hex` est une chaîne hexadécimale préfixée par
`0x` qui conserve exactement tous les bits du résultat non signé sur 64 bits.
Utilisez `information_hex` si le consommateur JSON ne préserve pas la précision
des entiers sur 64 bits, notamment pour les IOCTL sans tampon de sortie.

Les observations du travail portent la phase `callback:N`. Une requête en attente conserve `STATUS_PENDING` dans `dispatch_status` ; son état final figure séparément dans `io_status` et détermine sa contribution à `scenario_success`.

`ExRaiseStatus` transmet les 32 bits bas du NTSTATUS au handler d’exception invité ; `ExRaiseAccessViolation` et `ExRaiseDatatypeMisalignment` lèvent respectivement `STATUS_ACCESS_VIOLATION` et `STATUS_DATATYPE_MISALIGNMENT`. Le profil suit les pages DDI Microsoft individuelles : ExRaiseStatus autorise `APC_LEVEL`, tandis que les deux routines sans argument exigent `PASSIVE_LEVEL`. Certaines annotations SAL du WDK autorisent APC_LEVEL pour ces wrappers ; ce profil conserve la limite documentée plus stricte. Un appel qui lève conserve `result: null` et inscrit le code dans `detail` ; il ne rapporte jamais de retour API réussi.

La livraison d’exception utilise les tables de déroulement x64 version 1 décodées de l’image et les portées constantes `EXCEPTION_EXECUTE_HANDLER` de `__C_specific_handler`. Elle exécute le véritable corps du handler invité, déroule les frames de fonctions auxiliaires ordinaires, restaure les registres généraux non volatils sauvegardés et préserve la limite de pile de l’exécution courante. `GetExceptionCode()` observe le code levé. Un handler peut lever une autre exception vers une portée englobante prise en charge. Les fonctions filtre, `__finally`, personnalités GS/C++, métadonnées chaînées ou incomplètes, déroulement de prologue et restaurations XMM rencontrés échouent explicitement. Une exception API non capturée arrête en `model_error` ; les défauts CPU de mémoire, d’interruption ou d’instruction invalide restent terminaux.

Le code original `driver_wdm_seh.c` utilise les véritables en-têtes WDK et `/GS-`. Configurez `NEVERD_WDM_SEH_FIXTURE` et `NEVERD_WDM_SEH_CFG_FIXTURE` pour les images normales et CFG actif. L’exemple [driver-seh-scenario.json](../examples/driver-seh-scenario.json) rebase l’image, capture une exception API dans DriverEntry et décharge le pilote. Le chemin WDM distinct de METHOD_NEITHER prend en charge les sondes, les MDL verrouillés et les fautes mémoire utilisateur récupérables.

L’objet nullable `fault` conserve le premier défaut du backend. Ses champs
`kind`, `pc`, `address` nullable, `size`, `access` et `interrupt` distinguent la
mémoire non mappée ou protégée, les plages invalides, les instructions invalides
et les exceptions CPU. Les adresses utilisent des chaînes hexadécimales ; les
tailles et vecteurs d’interruption utilisent des entiers. Les lectures
d’observation ne peuvent pas remplacer le défaut d’origine. Un backend en
défaut ne peut pas reprendre, et cet enregistrement ne permet pas de traiter ces défauts backend par
la SEH invitée.

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

Pour WDM `METHOD_NEITHER`, `Type3InputBuffer` et `IRP.UserBuffer` désignent deux allocations utilisateur distinctes. `ProbeForRead` vérifie la plage et l’alignement sans toucher aux pages ; `ProbeForWrite` touche chaque page. `ExGetPreviousMode` indique le mode de la requête. `MmProbeAndLockPages` verrouille une allocation utilisateur, `MmGetSystemAddressForMdlSafe` fournit un alias partagé et `MmUnlockPages` retire cet alias et déverrouille les pages. Les processus arbitraires ne sont pas modélisés.

Une requête WDM `METHOD_NEITHER` IOCTL avec tampon non vide peut définir indépendamment `user_input_access` et `user_output_access` à `read_write` (défaut), `read_only` ou `no_access`. Ces champs sont refusés pour les méthodes tamponnées ou directes et les tampons vides ; `no_access` conserve le pointeur mais interdit l’accès aux pages.
Le rapport `configuration.user_page_access` conserve uniquement les protections explicites, avec un `source_request_index` commençant à zéro ; une direction omise reste `read_write`.

## Requêtes WDM concurrentes limitées

Une requête WDM READ/WRITE/IOCTL ou une requête d’une file KMDF parallèle illimitée peut définir `defer_callback_drain: true`. Le répartiteur doit renvoyer `STATUS_PENDING` et laisser l’IRP en attente pour que la requête suivante soit soumise avant les rappels. Après la prochaine requête sans ce champ, les rappels sont exécutés et le lot est finalisé ; si la dernière requête porte ce champ, le traitement se fait en fin de scénario. Les requêtes qui se chevauchent peuvent utiliser des objets fichier distincts ou un même objet explicitement ouvert en mode asynchrone. Le chevauchement sur un fichier synchrone, la préemption arbitraire et les arrivées externes ne sont pas pris en charge.

## Objet fichier asynchrone

Seule une requête CREATE peut définir le booléen `asynchronous_file: true` ; l’omission ou false conserve le mode synchrone. L’ouverture asynchrone efface `FO_SYNCHRONOUS_IO` du `FILE_OBJECT` invité et ne définit pas `IRP_SYNCHRONOUS_API` sur les IRP suivants. Les READ/WRITE/IOCTL d’un même fichier asynchrone ne se chevauchent que si le traitement des rappels est explicitement différé. CLEANUP/CLOSE attendent la fin et la finalisation de tous les transferts antérieurs. Aucune position de fichier implicite n’est maintenue ; `byte_offset` est propre à chaque requête et vaut zéro par défaut. Les autres types de requêtes rejettent ce champ, même avec false.
