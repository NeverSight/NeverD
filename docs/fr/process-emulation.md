**Langues** : [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Index de la documentation](README.md)

# Émulation de processus invités

`neverd emulate` exécute une image sous un profil explicite d’OS invité. Le transport CPU, l’analyse d’image, l’entrée du processus et les services OS ont des propriétaires distincts. Activez `NEVERD_ENABLE_CPU_EMULATION=ON` ; l’émulation des pilotes l’inclut aussi.

Le premier profil `linux-elf64-v1` exécute des ELF `ET_EXEC` x64/AArch64 et des PIE statiques `ET_DYN` auto-relocatifs à CPL3 ou EL0. Il charge de vrais segments ELF, construit la pile initiale, reprend par quanta et traite les requêtes explicites d’appels système Linux. C’est un modèle de processus autonome, pas une distribution Linux complète ni une promesse d’exécuter n’importe quel binaire libc. Liaison dynamique, signaux, threads, systèmes de fichiers et services non pris en charge échouent explicitement.

<!-- i18n-section: cli-sdk -->

## CLI et SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Les hôtes Linux compatibles choisissent KVM et les hôtes Windows compatibles WHP ; les autres combinaisons d’ISA hôte/invité utilisent Unicorn. Un backend choisi mais indisponible entraîne une erreur, sans bascule silencieuse. L’ELF conserve le modèle de processus Linux même sous Windows. Voir [exécution CPU](cpu-execution.md) pour l’inventaire d’instructions et ses limites.

Le CLI émet un seul rapport JSON. Code de sortie 0 pour un état invité nul, 2 pour un autre état, 3 pour une exécution incomplète (défauts et limites inclus), 1 pour une configuration/API invalide. L’état réel figure dans `exit_status`. L’entrée C additive [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) reçoit une session, un chemin non vide, un profil explicite et un JSON d’options facultatif. Libérez le résultat avec `neverd_free_string` ; NULL indique un échec de configuration détaillé par `neverd_last_error`. Un défaut invité ou un arrêt sur ressource renvoie un rapport. L’image d’analyse de la session n’est ni requise ni modifiée.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## Options et résultats

Les options sont un objet JSON de 64 KiB maximum. Champs inconnus/null, types incorrects, NUL intégrés aux chaînes et limites non positives sont rejetés.

| Option | Défaut | Contrat |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` ou `whp` |
| `arguments` | Nom du fichier d’entrée | argv complet, argv[0] inclus ; vide = défaut |
| `environment` | `[]` | Chaînes invité explicites ; l’environnement hôte n’est jamais hérité |
| `instruction_limit` | 100000 | Tentatives d’instruction admises et partagées |
| `event_limit` | 10000 | Événements syscall, facturés avant le service OS |
| `timeout_microseconds` | 5000000 | Deadline monotone démarrant après le setup du processus |
| `memory_limit` | 67108864 | Budget mémoire physique/mappé |
| `stack_size` | 1048576 | Pile alignée sur page dans le budget |
| `output_limit` | 1048576 | Total des octets stdout/stderr capturés |
| `instruction_quantum` | 1024 | Intervalle d’admission avant cession à la runtime |

`schema_version` vaut 1. Le rapport inclut profil, architecture, backend sélectionné et motif, `stop_reason`, `exit_status` nullable, diagnostic, PC d’entrée/courant, compteurs, enregistrements de services et dernière sortie CPU typée. Adresses, numéros syscall, registres d’arguments et bits de retour sont des chaînes hexadécimales **sans** `0x` ; `stdout_hex`/`stderr_hex` préservent NUL et UTF-8 invalide. Un résultat syscall null signifie aucun retour modélisé (exit ou requête non prise en charge, par exemple), et non un zéro réussi.

<!-- i18n-section: linux-semantics -->

## Sémantique du profil Linux

La politique OS réutilise les en-têtes de programme déjà décodés par le chargeur ELF. Elle vérifie tags ABI, alignement des segments, tables d’en-têtes mappées et limites d’adresses utilisateur. Un plan générique vérifie étendues, permissions, chevauchements et budget avant allocation, et ne publie qu’un espace privé entièrement préparé. Il conserve préfixes/restes des pages de fichier, met BSS à zéro, respecte les droits de segment et réserve des pages de garde de pile. Les dispositions à pages chevauchantes et en-têtes contradictoires sont rejetés sans supposition.

Le PIE statique utilise un load bias déterministe d’au moins `0x40000000`, augmenté pour respecter les alignements `PT_LOAD` supérieurs. Segments, PC d’entrée et `AT_PHDR`/`AT_ENTRY` partagent ce biais ; les valeurs originales des en-têtes restent inchangées et `AT_BASE` vaut zéro sans interpréteur. Le mappage utilise les octets du fichier original, jamais les fixups d’analyse ; le guest réalise ses propres relocations et initialisation. Le loader décode `PT_DYNAMIC` depuis des enregistrements bornés du fichier original, indépendamment des sections. Présente, la table doit être lisible, terminée et contenir au plus 4096 entrées. `PT_INTERP` et les tags externes de dépendance/filter/audit sont rejetés ; aucun linker dynamique, résolveur de symboles ou constructeur n’est fourni.

La pile initiale contient argc/argv/envp/auxv alignés, PHDR/PHENT/PHNUM, entry, taille de page et identités. PID/TID/UID/GID modélisés valent 1000. `AT_RANDOM` contient les 16 premiers octets du SHA-256 d’entrée pour la reproductibilité ; c’est une politique déterministe du modèle, pas une entropie cryptographique. HWCAP/HWCAP2 sont nuls ; aucun vDSO n’est fourni.

Les appels implémentés sont `write`, `exit`, `exit_group`, `getpid` et `gettid`, `mmap`, `mprotect`, `munmap`, `brk`, avec des numéros distincts pour [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) et [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). Le retour d’un SYSCALL x64 applique les clobbers RCX/R11, RAX et le PC suivant. ARM64 utilise x8 pour le numéro et x0 pour le résultat. Les appels inconnus arrêtent l’exécution avec `unsupported_service` ; aucun syscall hôte n’est exécuté.

Les modèles TLS statiques `PT_TLS` sont validés comme faits du loader : un seul modèle, étendues fichier/mémoire bornées, alignement congruent et octets initialisés lisibles. Le démarrage guest alloue/initialise les blocs TLS et installe le pointeur de thread ; le modèle Linux n’invente ni TCB ni DTV propre à libc. Cela permet le TLS local-exec généré par compilateur dans les programmes freestanding. TLS dynamique et threads OS restent hors périmètre.

Sur x64, `arch_prctl` prend en charge `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` et `ARCH_GET_GS`. Set accepte une base d’espace utilisateur même non mappée ; les déréférencements ultérieurs vérifient toujours les droits. Une base noyau renvoie `EPERM` invité ; une destination Get invalide renvoie `EFAULT` sans défaut CPU. Les autres opérations échouent explicitement. ARM64 installe `TPIDR_EL0` par `MSR` ; `MRS`, accès mémoire FS/GS et restauration de contexte préservent le pointeur entre quanta et entrées backend. Cela n’implémente pas d’ordonnanceur.

Les descripteurs 1 et 2 sont des puits d’octets virtuels. `write` valide les pages utilisateur lisibles, renvoie le préfixe lisible si une page suivante est inaccessible et `EFAULT` si aucun octet ne l’est. Un descripteur invalide donne `EBADF` ; une écriture de longueur nulle avec descripteur valide ne lit pas le pointeur. L’atomicité des pipes Linux et les fichiers ne sont pas modélisés. La limite de sortie arrête avant publication d’une écriture trop grande.

Les services de mémoire anonyme partagent l'espace d'adressage et le budget physique de l'image et de la pile. `mmap` accepte exactement `MAP_PRIVATE | MAP_ANONYMOUS`, avec `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`, `PROT_READ | PROT_EXEC` ou RWX lisible. Les adresses suggérées libres et alignées sont respectées ; sinon, la recherche part de `0x100000000`, puis de l'adresse utilisateur minimale, en réservant les gardes de pile. Ce placement déterministe ne simule pas l'ASLR Linux. Les nouvelles pages sont indépendantes et remplies de zéros ; une suppression partielle peut libérer les pages non épinglées. Une projection CPU ou une vue du stockage conservée peut prolonger la vie d'une allocation retirée.

Les longueurs sont arrondies aux pages. `munmap` tolère les trous et suppressions répétées ; `mprotect` modifie le préfixe mappé avant de retourner `ENOMEM` au premier trou. `PROT_NONE` conserve l'allocation et les octets sans autoriser l'accès invité. L'appel brut `brk` retourne la limite demandée en cas de succès et l'ancienne en cas d'échec, contrairement au zéro/moins un du wrapper libc. La limite initiale est la fin d'image alignée sur une page. L'extension respecte les autres mappings et le budget ; la réduction conserve les octets de la page partielle restante. Les règles et priorités d'erreur suivent les services Linux de [mapping](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) et de [protection](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c).

Les mappings de fichiers, partagés ou fixes, la croissance descendante, les grandes pages, le verrouillage, les clés de protection, les politiques exécution seule/écriture seule et les autres drapeaux restent explicitement non pris en charge : arrêt avant tout effet publié ou retour inventé. Les erreurs ordinaires de plage, longueur et alignement du sous-ensemble admis retournent une erreur invitée et permettent de poursuivre. Aucun pointeur ni demande de mapping invité n'est transmis à l'OS hôte.

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## Profil Windows PE64

`windows-pe64-v1` ajoute des processus console Windows x64/ARM64 bornés : chargement PE, PEB/TEB, TLS statique et dynamique, callbacks de démarrage/arrêt et modèles Win32 nommés. Il utilise la couche CPU indépendamment des pilotes ; chargement DLL/CRT, GUI, SEH utilisateur, threads et compatibilité Windows générale restent inachevés.

La mémoire virtuelle Windows ajoute `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` et `FlushInstructionCache` pour le processus courant. La couche OS possède les réservations ; `AddressSpace` reste la référence pour les pages validées, les permissions et leur stockage. Les tests couvrent la réécriture de code, les défauts d’accès et la réutilisation du budget mémoire.

Les allocations privées prennent en charge `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE` et `MEM_TOP_DOWN`, avec un alignement de réservation de 64 KiB et des pages de 4 KiB. Une réservation seule ne consomme pas de RAM invitée. Une nouvelle validation conserve les octets et actualise les permissions ; la dévalidation restitue chaque page. La vérification de toute la plage et la préparation des allocations évitent les modifications partielles en cas d’échec ordinaire. La requête renvoie la structure x64/ARM64 de 48 octets et regroupe les pages suivantes d’une même allocation. L’image, l’environnement, le tas, les entrées API et les marges de pile participent au placement ; l’identité de la pile correspond au TEB. Si un `VirtualProtect` réussi rend son emplacement de sortie des anciennes permissions accessible en lecture seule, les nouvelles permissions restent appliquées, les octets de sortie restent inchangés et l’appel réussit. Une modification de protection couvrant des pages non validées renvoie `ERROR_INVALID_ADDRESS`, écrit `PAGE_NOACCESS` dans la sortie des anciennes permissions et ne modifie pas les permissions des pages.

Les protections admises sont `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ` et `PAGE_EXECUTE_READWRITE`. Pages de garde, exécution seule, copie à l’écriture, modificateurs de cache, grandes pages, reset/write-watch/emplacements réservés et modification des mappages internes du modèle restent explicitement non pris en charge. Seules les allocations virtuelles privées peuvent être dévalidées ou libérées. Cela n’ajoute ni distribution d’exceptions utilisateur ni preuve matérielle ARM64 native.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

Ce modèle versionné à un seul thread charge les EXE console PE32+ AMD64/ARM64 à leur base préférée. Les octets originaux et les faits du chargeur sont contrôlés ; les droits utilisateur et les espaces de garde de pile sont conservés. Imports inconnus, ordinaux, liés ou différés, configuration de chargement/CFG, images gérées, GUI et entrées DLL sont refusés. Les relocalisations sont validées sans rebasage. Les modèles API ne sont pas des DLL installées : la liste du chargeur ne contient que l’EXE et `GetModuleHandleW` accepte seulement NULL.

GS sur x64 et x18 sur ARM64 pointent vers TEB : limites de pile, pointeur propre, PID/TID, PEB, paramètres, LastError et TLS. L’entrée UTF-8 stricte devient UTF-16, argv suit les règles de citation Microsoft CRT. Les noms d’environnement sont ASCII ; les doublons sans distinction de casse sont refusés, les valeurs Unicode sont permises et le bloc trié finit par deux NUL. Aucun environnement ni système de fichiers hôte n’est hérité. Le TLS statique copie son modèle, initialise BSS à zéro et écrit un index 32 bits ; le TLS dynamique utilise d’autres cases TEB. Les callbacks d’attachement/détachement sont relus en mémoire, dans l’ordre, avec un budget et une échéance partagés. Retour d’entrée et sortie normale déclenchent le détachement ; une sortie récursive pendant celui-ci est arrêtée explicitement.

`WindowsProcessServices.def` définit exactement `ExitProcess`, `RtlExitUserProcess`, les handles de sortie et `WriteFile` synchrone, LastError, les identifiants et pseudo-handles processus/thread, `GetCommandLineW`, allocation/libération/taille du tas, TLS dynamique et NULL `GetModuleHandleW`. La résolution exige les noms exacts dans `kernel32.dll`, `kernelbase.dll` ou `ntdll.dll`. Les syscalls directs et faux points de retour ne choisissent pas de modèle. Le tas appartient au processus et est récupéré ; la sortie garde les octets binaires. Les erreurs Win32 se distinguent des E/S asynchrones et exceptions utilisateur non prises en charge. Les alias observent la remise à zéro initiale du compteur et le véritable emplacement de retour.

`windows.native_calls` conserve module/fonction, arguments scalaires déclarés et résultat nullable, sans inventer de numéro NT. `NeverDWindowsProcessTests` couvre PE réel, TLS du compilateur, callbacks modifiés, tas, alias, métadonnées invalides, privilèges et budgets ; `NeverDProcessPublicTests` vérifie CLI/C ABI. La CI Windows exécute directement le même EXE comme référence indépendante et exige les tests WHP. Une preuve d’exécution ARM64 native nécessite encore une machine adaptée.

Pour un tampon d’entrée non vide et inaccessible en lecture, `WriteFile` renvoie `ERROR_INVALID_USER_BUFFER` (1784), remet à zéro le nombre d’octets écrits et ne produit aucun octet.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c).

<!-- i18n-section: verification -->

## Vérification

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# Dans un build avec bibliothèque partagée/CLI :
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Les tests compilent des entrées ELF autonomes en assembleur et C pour les deux ISA : data/BSS, démarrage réel, erreurs système, sortie binaire, permissions, écritures partielles, services non pris en charge et budgets entre quanta. Les fixtures TLS initialisent des blocs alignés indépendants, leur BSS et les pointeurs de thread, puis vérifient leur conservation ; x64 vérifie aussi les erreurs `arch_prctl` sans perte de la base antérieure. Les backends indisponibles sont explicitement ignorés. La suite publique traverse l'ABI C partagée et la CLI et compare rapports et codes de sortie. Les PIE vérifient auxv et les slots RELA initialement nuls avant leurs propres relocations de données et de fonctions. Les tests de mapping conservent les fixups quand la source d'analyse est choisie ; ceux des tables dynamiques couvrent l'absence de sections et les entrées malformées ou dépendantes. Les deux ISA exercent allocation, protection, trous, remapping, extension/réduction du tas et erreurs traitées. De vraies écritures invitées vérifient les fautes après protection totale ou partielle. x64 réécrit du code à la même adresse entre RW et RX et appelle les deux versions ; le même ELF exécuté nativement sous Linux sert d'oracle indépendant. Les tests mémoire couvrent épuisement, récupération et instantanés de mapping faisant autorité sans retenir la RAM. La compilation croisée et Unicorn ARM64 ne prouvent pas le fonctionnement natif ARM64 KVM/WHP.
