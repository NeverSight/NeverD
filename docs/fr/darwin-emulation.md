**Langues**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 1939e117643388dff149085b992e1ad646feefca63b630abec9b09122e15c2f1 -->

[← Index de la documentation](README.md)

# Environnements de processus invités macOS et iOS

`lib/emulation/os/darwin/` fournit des processus Mach-O autonomes et bornés, distincts du transport CPU hôte. Activez `NEVERD_ENABLE_CPU_EMULATION` ; l’émulation des pilotes Windows n’est pas nécessaire. `macos/` et `ios/` définissent des profils explicites.

| Profil | Plateforme Mach-O | ISA invitées | Pages OS |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, ARM64 de base | 4 KiB x64 ; 16 KiB ARM64 |
| `ios-macho64-v1` | appareil iOS | ARM64 de base | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, ARM64 de base | 4 KiB x64 ; 16 KiB ARM64 |

Un binaire appareil n’est pas une image simulateur. La plateforme invitée ne se déduit pas de l’hôte. [HVF](macos-hvf.md) exécute les ISA correspondantes sous macOS ; `auto` utilise Unicorn pour les autres ISA. La granularité CPU reste de 4 KiB. Les [API C, Python et CLI](process-emulation.md) partagent options, limites et rapports.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## Image et démarrage

Le chargeur `MachOExecutionImage` préserve les octets originaux, sans correctifs de relocation issus de l’analyse. Il accepte uniquement un Mach-O thin little-endian `MH_EXECUTE` avec plateforme et entrée non ambiguës. Une image universelle nécessite l’extraction explicite d’une tranche.

Le fichier complet, métadonnées et octets de fin inclus, doit respecter `memory_limit` avant analyse ou copie. Un instantané privé borné est lu depuis un fichier ordinaire ; chemins avec NUL, lectures courtes et changements de taille sont refusés. Aucun mapping de fichier vivant n’est conservé. Le budget de fichier et le budget de mémoire invitée sont deux plafonds séparés de même valeur ; les E/S hôte n’ont pas de délai temps réel garanti.

Les segments conservent permissions courantes/maximales et zones à zéro. `__PAGEZERO` réserve des adresses sans allouer son étendue. Les plages fichier/VM, alignements OS, chevauchements arrondis, propriétaire de l’en-tête, entrée exécutable et budget sont vérifiés. Le segment d’en-tête doit être lisible et exécutable ; pages de garde et porte de retour privée restent réservées. La dernière page de fichier garde les octets jusqu’à la limite de page ou EOF ; les pages VM complètes suivantes sont mises à zéro, selon le [chargeur XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

`LC_MAIN` reçoit `argc`, `argv`, `envp` et le vecteur apple comme quatre arguments entiers ; le retour fournit les huit bits bas du statut de sortie. `/usr/lib/dyld` est accepté seulement pour ce transfert d’entrée sans imports : le dyld hôte n’est pas exécuté. Un `stacksize` non nul est refusé ; `stack_size` fourni par l’appelant fixe le budget.

`LC_UNIXTHREAD` exige un seul enregistrement complet de registres généraux 64 bits natifs, avec uniquement PC renseigné. La pile contient argc, argv/envp terminés et un vecteur apple terminé avec `executable_path=<input filename>`. SP/flags personnalisés, autres registres, saveurs supplémentaires ou entrées conflictuelles sont refusés. Aucun environnement hôte ni vecteur auxiliaire Linux n’est hérité. Voir l’[architecture dyld](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

Dylibs externes, imports, rebases/chained fixups, constructeurs/destructeurs, sections TLS, arm64e/PAC, sous-types non pris en charge, chiffrement et commandes non modélisées sont refusés avant exécution. Les PIE sans fixups utilisent leurs adresses préférées, sans ASLR. Les blobs de signature sont des métadonnées, sans modèle AMFI ni politique d’entitlements.

## Services Darwin

Les appels BSD sur ARM64 utilisent X16, X0–X5 et `svc #0x80` ; x64 utilise la classe BSD `0x02000000`, RAX et RDI/RSI/RDX/R10/R8/R9. Une réussite efface carry ; une erreur le positionne et renvoie un errno positif. ARM64 efface X1 ; x64 efface RDX en cas de réussite et le conserve en cas d’erreur. Les registres modifiés par SYSCALL sont explicites. Le rapport utilise `result` et `error=true` pour une erreur BSD ; les requêtes sans retour ou non prises en charge n’ont aucun de ces champs. Les règles suivent les entrées XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) et [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), sans incorporation de code Apple.

Services : `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `getgroups`, `mmap`, `mprotect`, `munmap`. PID vaut1000 et PPID1 ; UID/GID valent1000 par défaut, ou les ID réels/effectifs distincts déclarés ci-dessous. Les descripteurs 1 et 2 capturent des octets, y compris NUL/non-UTF8 ; les descripteurs fermés ou en lecture seule donnent EBADF. Une copie partielle conserve les octets déjà lus mais renvoie EFAULT. Une longueur supérieure à `INT_MAX` donne EINVAL avant vérification du descripteur, du pointeur ou du budget, suivant [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

La mémoire accepte les mappings privés anonymes de données : `flags=0x1002`, descripteur -1 et offset zéro. Longueurs et adresses indicatives non fixes sont arrondies vers le haut à la page OS ; une indication occupée cherche plus haut, puis revient au placement par défaut. Le mmap brut historique de longueur zéro renvoie zéro sans allocation ; `MAP_UNIX03` est accepté et refuse la longueur zéro avec EINVAL. Unmap/protect exigent une adresse alignée. NONE/READ/WRITE sont pris en charge, WRITE implique READ. Les pages physiques appartiennent à chaque page OS : un unmap partiel libère son budget, puis les nouvelles pages sont à zéro. Un protect traversant un trou ou dépassant les droits maximaux échoue sans changement partiel. Référence : [services VM XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Les mappings partagés/fixes/JIT ou anonymes exécutables, autres traps Mach, appels indirects, threads, signaux, fichiers hôte/réseau, dyld, runtimes Objective-C/Swift et Foundation/UIKit sont hors contrat et arrêtent explicitement l’exécution. Ce profil ne constitue pas un OS Apple complet ni l’application iOS Simulator.

## Validation

Les fixtures C originales sont produites par Clang et `ld64.lld`, sans SDK Apple ni binaire propriétaire. Elles couvrent les cinq combinaisons plateforme/ISA, les enregistrements Mach-O malformés, les pages 4/16 KiB et la libération partielle sous budget plein. `NeverDProcessPublicTests` compare C API/CLI ; les variables `NEVERD_TEST_LIBNEVERD` et `NEVERD_TEST_DARWIN_FIXTURES` activent les mêmes cinq combinaisons dans le SDK Python.

## Fichiers et descripteurs explicites

`darwin_files` fournit aux trois profils un catalogue fermé de fichiers initialement en lecture seule. Le champ obligatoire `files` contient un `path` invité absolu canonique et des `bytes_hex` hexadécimaux. `stdin_hex` est un flux fini facultatif : absent signifie inconnu et arrête une lecture non vide, une chaîne vide signifie EOF. Sans catalogue open s’arrête ; un catalogue explicitement vide renvoie ENOENT. Aucun fichier ni flux hôte n’est consulté.

Les services ajoutés sont `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl`, avec les entrées nocancel de read/write/open/close/fcntl/pread. O_RDONLY/O_CLOEXEC et F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL sont pris en charge. Chaque open a sa position ; les duplications partagent la position mais gardent leurs propres indicateurs close-on-exec. pread ne déplace pas la position. Fermer ou remplacer 0/1/2 affecte les I/O suivantes ; une sortie dupliquée conserve sa destination et son budget.

Limites : 256 fichiers, 16 MiB cumulés pour chemins/NUL/fichiers/entrée, chemins de moins de 1024 octets et composants de 255 octets maximum. `descriptor_limit` est un plafond exclusif de 3–4096, défaut 256 ; JSON reste limité à 64 KiB. Les options invalides échouent avant chargement. read au-delà de INT_MAX donne EINVAL avant recherche du FD ; EOF ne touche pas la destination, une adresse invalide donne EFAULT. Un tampon partiellement inscriptible arrête avant copie ou changement de position. Les erreurs SET/CUR/END conservent la position. Ancien stat et autres fcntl restent exclus. Un fichier utilisé comme ancêtre donne ENOTDIR. Le même objet est comparé au noyau macOS ; C/CLI/Python couvrent cinq combinaisons invitées, sans preuve sur appareil iOS.

Vérification Release du 2026-10-05 : 381 inscriptions, 177 réussites, 204 ignorées, aucun échec, et 51/51 cas ARM64 HVF obligatoires exécutés. Sept programmes macOS natifs, 35 tests publics C/CLI/rapports, cinq combinaisons Python et 66 tests du vérificateur ont également réussi. Les comptes se recoupent. Les nouveaux services n’ont pas de preuve native Intel HVF/KVM/WHP ; Intel HVF reste non validé et ses Actions suspendues. Le SDK iOS et la comparaison sur appareil manquent.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Modification des fichiers existants

Le booléen strict `"writable":true` ou `DarwinFileOptions::WritableFiles` autorise les modifications locales au processus. Absent/false conserve la lecture seule ; une autorisation inconnue arrête le service. Aucun fichier hôte ni octet initial fourni n’est modifié. write(4/397), pwrite(154/415), truncate(200), ftruncate(201) et O_TRUNC partagent le contenu ; open garde une position indépendante, dup partage position et état, et le contenu survit au dernier close. L’extension remplit de zéros ; la troncature conserve les positions, même avec O_RDONLY|O_TRUNC.

F_SETFL ne modifie que O_APPEND|O_NONBLOCK après conversion native et conserve accès, close-on-exec et FWASWRITTEN. F_GETFL expose 0x10000 après un transfert non vide, y compris pwrite et la sortie capturée. pwrite ignore append et conserve la position. INT_MAX est vérifié avant FD ; pwrite à -1 donne EINVAL encore plus tôt. INT64_MAX donne EFBIG avant le cas vide ; la longueur est réduite avant de choisir EOF.

ftruncate réussi, même sans changement de taille, marque FWASWRITTEN sur la description appelée et ses dup. O_TRUNC marque la nouvelle description, même O_RDONLY ; truncate par chemin ne marque aucune description existante.

Une entrée partiellement lisible arrête avant tout effet. EFAULT intégral conserve les octets, mais append non vide avance à EOF. Une panne du transport ne valide aucun contenu ni position. Sans `mutation_policy`, écriture non vide, troncature et EFAULT intégral non vide invalident l’observation stat complète ; les requêtes suivantes s’arrêtent avant copie. Une écriture vide la conserve. Les 16 MiB comptent chemins/NUL, entrée, enregistrements, CWD, contenu actuel et références de chemins inscriptibles. Réduire remplace le stockage et récupère la capacité ; entrée initiale et tampon de remplacement borné sont supplémentaires. Alias inode connus et flags immutable/append-only sont refusés.

DarwinMemory garde des baux jusqu’au dernier unmap, même pour PROT_NONE ou après close ; les mutations sont refusées jusque-là. Les échecs et anciens mmap de longueur zéro ne gardent aucun bail. Les nouveaux mappings voient les octets actuels. O_WRONLY avec READ/WRITE donne EACCES ; PROT_NONE peut ensuite gagner lecture/écriture par mprotect.

Les programmes originaux normal/nocancel comparent le noyau natif ; tests 4K/16K et C/CLI/Python couvrent cinq combinaisons. Contrôle des droits, suppression de répertoires, renommage entre domaines de répertoires initiaux distincts, liens physiques, métadonnées du système de fichiers natif, cohérence des mappings et SIGBUS EOF restent incomplets. Ni environnement complet, ni appareil iOS, ni Intel HVF ne sont validés ; les Actions Intel restent suspendues.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Métadonnées modifiables explicites

Un fichier peut ajouter mutation_policy à `writable: true` et aux metadata complètes ; C++ utilise `DarwinFileOptions::MutationPolicies`. Ce contrat virtuel d’allocation creuse est explicite : aucune allocation APFS ni heure hôte n’est déduite. Sans politique, les métadonnées après mutation restent inconnues.

allocation_unit, mutation_time et seconds/nanoseconds sont obligatoires, avec les règles entières sans perte existantes. L’unité est une puissance de deux entre512 octets et16 MiB, indépendante de block_size et des pages VM. Il faut des permissions ordinaires sans set-id/sticky, flags=0, link_count=1 et une allocation initiale dense : blocks=ceil(size/allocation_unit)*(allocation_unit/512). Les octets nuls n’impliquent aucun trou. La référence du chemin compte dans les16 MiB logiques ; le registre d’allocation ne sert pas à inventer ENOSPC.

Toute unité touchée par une écriture est allouée, même pour écrire des zéros dans un trou. Agrandir par truncate ajoute des zéros sans allocation ; réduire supprime les unités au-delà d’EOF arrondi vers le haut, conservant l’unité finale partielle. Agrandir ensuite ne restaure pas les unités supprimées. Une écriture non vide réussie ou tout truncate réussi, même de même taille ou O_TRUNC vide, met à jour size/blocks et fixe mtime/ctime au temps fourni. Les autres champs et entrées restent inchangés ; read n’avance pas atime. Stat par chemin, open indépendants, dup et réouverture partagent le nœud.

Écriture vide, refus de budget/mapping, entrée partielle refusée et panne du transport préservent l’état connu. EFAULT intégral non vide le rend inconnu ; un succès ultérieur ne le reconstruit pas. Un échec de copie stat ne change pas le nœud. virtual-file-metadata vérifie144 octets sur cinq profils et C/CLI/Python : c’est un test de politique, pas une équivalence APFS. Les programmes natifs vérifient séparément flags, positions et erreurs. Espace de noms, cohérence native, Mach et chargement dynamique restent incomplets.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## Positionnement dans les fichiers creux

Avec mutation_policy et une allocation encore connue, lseek accepte SEEK_HOLE=3 et SEEK_DATA=4 sur les fichiers ordinaires et lit le même registre que stat. L’entrée initiale est dense, même avec des zéros. Dans une unité du type demandé, il renvoie la position fournie ; sinon le début de l’unité suivante correspondante. Le trou terminal est EOF. Une position négative donne EINVAL ; à/après EOF, même fichier vide, ou sans données suivantes, ENXIO=6. L’erreur conserve la position ; le succès ne change que la description et ses dup. Les open indépendants gardent leur position, la réouverture voit l’allocation actuelle. Métadonnées, flags et octets restent inchangés ; les bits hauts de whence sont ignorés.

Sans politique, pour un répertoire ou après EFAULT intégral rendant l’allocation inconnue, le service reste exclu. Zéros et modifications refusées ne créent aucune allocation supposée. Le programme original sparse-file-seek compare erreurs, octets écrits, EOF et durée des descriptions natifs/invités sans présumer les limites antérieures propres au FS. virtual-file-metadata vérifie séparément la géométrie exacte de la politique ; C/CLI/Python couvrent cinq profils.

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Suppression des noms de fichiers ordinaires

`mutable:true` par répertoire (C++ `MutableDirectories`) autorise explicitement les changements de noms immédiats, indépendamment de `writable`. Sans autorisation, arrêt explicite. L’admission refuse les flags connus non nuls, les permissions spéciales du parent, link_count≠1 pour un enfant et les alias connus du parent/enfant, en combinant stat et inodes des snapshots. Des périphériques explicitement distincts restent distincts ; les chemins consomment le budget existant.

`unlink(10)` / `unlinkat(472)` retirent les noms ordinaires existants. la suppression de fichiers accepte les 32 bits bas 0 ou `0x800` ; bits inconnus : EINVAL avant chemin/FD ; AT_REMOVEDIR suit le contrat borné ci-dessous ; DATALESS et SYSTEM_DISCARDED restent exclus. Résolution commune : ENOENT, ENOTDIR après un fichier suivi de `/`, EPERM pour répertoire ordinaire, EISDIR pour une racine composée de barres seules, EBUSY pour une racine terminée par `.`/`..`. Les suffixes `.`/`..` ont aussi été vérifiés nativement.

Les anciens FD/dup/ouvertures indépendantes gardent données, positions et flags ; F_GETPATH conserve l’ancien chemin capturé. Les nouvelles ouvertures échouent, parents implicites et CWD subsistent. L’autorisation d’écriture appartient à l’objet ; son budget de données actuelles est récupéré après le dernier descripteur et mapping, via close/dup2 ou la prochaine mutation. Les coûts initiaux des chemins restent comptés. Contrôle des droits, renommage entre domaines de répertoires initiaux distincts et liens physiques restent à faire ; les répertoires initiaux utilisent l’autorisation explicite décrite plus bas.

Les observations stat/readdir/SEEK_END du parent deviennent inconnues pour tous les FD et chemins, avant copie/déplacement. read/pread restent EISDIR ; SET/CUR/F_GETPATH/fchdir/résolution relative continuent. Une politique connue fixe nlink=0 et ctime, sans restaurer nlink=1 aux écritures suivantes. Sans politique/après EFAULT, métadonnées inconnues. Les échecs préservent l’état. `unlinked-file` compare les règles natives de noms/FD ; temps et invalidation sont des règles explicites du modèle.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Création de fichiers ordinaires

O_CREAT=0x200 crée un fichier vide dans un parent direct explicitement mutable, via open/openat normal ou nocancel. Le nouvel objet est inscriptible ; les objets existants conservent leur autorisation WritableFiles. Un FD en lecture seule peut créer, mais pas écrire. Sans politique de création explicite, stat64 et recherche de trous restent inconnus. Les metadata/mutation_policy d’un ancien objet homonyme ne sont jamais hérités.

Avec O_CREAT, O_EXCL=0x800 renvoie EEXIST sur fichier/répertoire existant avant troncature ; seul, il est sans effet. O_CREAT en lecture seule ouvre un répertoire existant. Après le contrôle du premier octet et du répertoire dans openat décrit ci-dessous, l’ordre est : mode d’accès invalide, disponibilité FD, EINVAL pour O_CREAT|O_DIRECTORY, puis chemin. Seul le dernier composant original absent peut être créé ; ancêtre absent et terminaisons `/`, `//`, `/.`, `/..` donnent ENOENT. Une création O_TRUNC ne marque pas FWASWRITTEN, contrairement à la troncature d’un objet existant.

Seule l’insertion invalide les observations du parent. Objets homonymes ancien/nouveau gardent données, FD, métadonnées et baux de mapping distincts. La limite de 256 compte les entrées initiales non-fichiers et objets vivants ; chemins canoniques/NUL dynamiques et octets courants comptent dans 16 MiB. Après unlink, le dernier FD/mapping libère les coûts dynamiques ; les coûts initiaux restent réservés. Budget épuisé ou chemin canonique de 1024 octets arrête explicitement sans inventer ENOSPC ou erreur native de chemin ; aucun nom/FD n’est publié. created-file compare le natif aux cinq profils, avec limites 4K/16K. Contrôle des droits, renommage entre domaines de répertoires initiaux distincts, liens et mutation des répertoires restent à compléter.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Métadonnées de création explicites et umask du processus

Le `darwin_files.umask` facultatif (C++ `InitialUmask`) définit le masque initial entre 0 et 07777 octal, indépendamment du droit de création. `umask(60)` renvoie l’ancien masque et conserve les bits bas 07777, sans mémoire invitée ni FD libre. L’omission signifie inconnu, sans valeur hôte ou valeur par défaut supposée. L’initialisation est unique ; les changements concernent uniquement les créations futures, sans modifier l’entrée. L’exemple utilise 18 décimal, soit 0022 octal.

Sans `namespace_policy` : Le `darwin_files.creation_policy` facultatif (C++ `CreationPolicy`) fournit les métadonnées complètes des nouveaux objets. L’objet strict contient exactement `first_inode`, `block_size`, `generation`, `creation_time`, `mutation_policy` ; temps et politique de mutation utilisent les formats existants. Il faut un umask explicite, au moins un parent mutable et des metadata complètes pour chaque parent autorisé. block_size est dans 1..INT32_MAX, generation est uint32 ; l’unité d’allocation est une puissance de deux de 512 à 16 MiB, indépendante du bloc/de la page VM, et les nanosecondes sont dans [0,1000000000). first_inode est un uint64 positif supérieur à tous les inode stat/instantanés, y compris d’autres périphériques. Les chaînes décimales préservent les entiers hors du domaine exact de JSON.

Seule l’insertion réussie d’un nouvel objet consomme la suite globale d’inode. UINT64_MAX l’épuise définitivement ; close/unlink/réutilisation de nom/umask/recherches ne la réinitialisent pas. Refus d’exclusivité, FD, chemin, nombre d’entrées ou budget d’octets ne publient ni nom/FD ni incrément ; O_CREAT existant ne consomme rien. Le nouveau stat64 utilise device/GID du parent direct, UID effectif invité sélectionné (1000 par défaut), mode `S_IFREG | (mode & 0777 & ~umask)`, nlink=1 et size/blocks/flags=0. Bloc, generation et quatre temps initiaux fixes viennent de la politique. Après invalidation du stat/énumération complet du parent, device/GID restent utilisables sans restaurer tout le relevé.

Chaque nœud possède ses métadonnées/allocations, sans héritage de l’ancien homonyme. write/truncate/unlink partagent la politique et préservent inode/mode/birthtime et nlink=0 après unlink ; un EFAULT intégral laisse l’état définitivement inconnu. Les nœuds existants ne changent pas rétroactivement. `created-file-metadata` compare droits, ancien masque, UID effectif, périphérique/groupe parent et durée de vie natifs dans cinq profils ; `virtual-created-metadata` compare séparément les 144 octets. Les quatre temps natifs peuvent différer. Temps fixes/allocation creuse sont des règles virtuelles ; contrôle des droits, changement d’identité, ACL et comportement APFS natif restent à faire.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Échange atomique de fichiers et de répertoires créés

RENAME_SWAP=0x2 échange via renameatx_np deux fichiers ordinaires existants, deux répertoires vivants créés par le processus, ou un fichier et un tel répertoire, avec RENAME_NOFOLLOW_ANY en option. Le répertoire initial explicite déclare mutable:true et swap_rename:true ; C++ utilise DarwinFileOptions::SwapRenameDirectories. Les descendants héritent de la capacité de l’objet initial ; réutiliser un nom supprimé ne transfère aucune déclaration. false ou omission signifie inconnu. Devices identiques et autorisations seules ne prouvent pas le support ; les domaines initiaux distincts restent exclus.

`openat_nocancel`, `fstatat64` et `F_GETPATH=50` utilisent le même composant fichiers. Après échange, chemins et observations restent propres aux objets ; la disponibilité du stat complet suit toujours le contrat de métadonnées.

Une cible absente, même avec slash final, donne ENOENT avant point/double point source, domaine, autorisation ou capacité. Les opérandes répertoires initiaux ou supprimés restent exclus. Dans le domaine admis, les deux ordres ancêtre/descendant et répertoire/fichier enfant donnent EINVAL ; une cible fichier avec slash final donne ENOTDIR. Le même objet avec composant ordinaire est sans effet après autorisation, même sans capacité déclarée. Le même objet source-point dépend encore d’une sensibilité à la casse inconnue. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 et flags inconnus donnent EINVAL avant les chemins ; SECLUDE reste indisponible.

Les deux sous-arbres non vides suivent les objets parents, y compris répertoires supprimés et fichiers orphelins retenus par FD/mapping. L’échange mixte déplace seulement la racine fichier exacte ; un ancien orphelin homonyme reste avec son parent. FD, dup, CWD et double point suivent objets et nouveaux parents. Octets, identité, métadonnées, droits, curseurs, flags et baux des fichiers descendants restent intacts. Racines déplacées et parents immédiats appliquent les règles de namespace existantes ; une politique configurée actualise le ctime propre du fichier racine, sinon ses métadonnées complètes restent inconnues.

Chaque référence réserve chemin+NUL dans le budget initial fixe de 16 MiB. Tous les chemins liés/retenus des deux directions sont vérifiés sous 1024 octets et le budget partagé avant retrait commun des anciens noms et publication. Aucun retrait de racine ni crédit de remplacement, même pour les octets d’une cible non ouverte. Le premier déplacement d’un fichier initial acquiert un coût dynamique ; revenir ne l’efface pas et les allers-retours ne l’accumulent pas. Aucun nouveau FD, entrée ou inode. Un refus préserve les deux namespaces, parents, curseurs, observations et mappings. L’original SDK-free swapped-directory compare les répertoires et les deux ordres mixtes sur macOS natif et cinq profils C++/C/CLI/Python.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Instantanés explicites de répertoire

`getdirentries64` (344) parcourt le `contents` immuable facultatif d’un élément `directories` existant ; C++ utilise `DarwinFileOptions::DirectoryContents`. `entries` décrit dans l’ordre explicite tous les enfants directs, `.` et `..` compris. Sans instantané, même un répertoire vide reste inconnu. Aucun chemin, stat ou accès hôte n’est déduit.

Chaque entrée exige `name`, un `inode` non nul, `type` (0 inconnu, 4 répertoire, 8 fichier), `next_offset` et `seek_offset`. Type et chemin concordent ; les inodes d’un même chemin résolu concordent entre instantanés et métadonnées. `next_offset` est non nul, unique dans ce répertoire, <=INT64_MAX, sans ordre croissant requis ; zéro rembobine. `seek_offset` est l’observation d_seekoff distincte sur 64 bits non signés ; les zéros répétés sont permis. Les entiers suivent les chaînes décimales sans perte de stat.

`contents.minimum_buffer_size` est obligatoire : minimum de charge utile de 1–128 MiB, EOF inclus. Le `minimum_buffer_size` facultatif d’une entrée (défaut 0) s’applique au démarrage à cette position. L’exemple observe APFS : 64 octets pour les deux points initiaux, 1 à EOF ; ailleurs un enregistrement complet doit tenir. Le format LP64 est aligné sur huit octets, taille `roundUp(25 + nameBytes, 8)`. Maximum total : 4096 entrées ; leurs octets comptent dans les 16 MiB. Les chemins ancêtres déclarés uniquement par métadonnées/instantané comptent une fois dans les 256 chemins. JSON reste limité à 64 KiB.

Les open indépendants ont leurs curseurs, dup les partage. Seuls zéro et les valeurs fournies permettent la reprise ; une position inconnue arrête explicitement. Chaque appel retourne le plus grand préfixe d’enregistrements entiers. Une longueur >=1024 réserve les quatre derniers octets demandés à EOF (1 à la fin, sinon 0) ; seule la charge est plafonnée à 128 MiB. L’adresse des indicateurs conserve le calcul non signé original, débordement compris. Ordre : données, avance du curseur, copie de la position initiale, indicateurs. Un EFAULT tardif conserve les effets précédents ; EOF omet la copie vide. Une copie partiellement accessible s’arrête avant cette copie, sans annuler les effets antérieurs.

`directory-entries` compare les champs, dup/rembobinage, petites lectures, EOF et ordre des copies au noyau macOS. Un test séparé compare tous les octets natifs capturés, noms longs compris, au SDK. Les cookies fixes ne reproduisent pas les générations dynamiques APFS. L’ancien `getdirentries` (196), l’énumération après mutation, les autres transports natifs et iOS physique restent hors de cette validation.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Validation de l’énumération (2026-10-05, Release) : 498 cas Darwin, 246 réussites, 252 omissions pour backend indisponible, zéro échec ; 63/63 cas ARM64 HVF obligatoires exécutés. Les 11 programmes macOS natifs, 40 contrôles C/CLI/rapport sans omission, cinq combinaisons Python avec huit scénarios de fichiers chacune et 66 tests des outils ont réussi. Comptages superposés. Preuves : `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel HVF Actions reste suspendu ; autres transports natifs et iOS physique non validés.

## Mappings privés de fichiers

`mmap` accepte les fichiers réguliers du catalogue avec `MAP_PRIVATE` : `flags=0x2` ou `0x40002` avec `MAP_UNIX03`, à un offset aligné sur une page OS. Même une longueur courte conserve tous les octets du fichier dans la page ; la fin de la dernière page EOF est nulle. Une écriture privée ne change ni le fichier, ni les autres mappings, ni les métadonnées fixes, ni le curseur partagé. Le mapping survit à close et à la réutilisation du FD. Les mappings en lecture seule et PROT_NONE sont initialisés ; `mprotect` peut ensuite autoriser l’écriture.

Un débordement de fin de fichier, une longueur UNIX03 nulle ou un offset UNIX03 non aligné donnent EINVAL avant recherche du FD ; un FD invalide donne EBADF avant contrôle du budget. Le mode historique de longueur nulle vérifie néanmoins le FD. Offsets historiques non alignés, flux, pages de fichiers vides et pages entièrement au-delà d’EOF arrêtent avant allocation. macOS permet ces pages EOF mais leur accès produit SIGBUS : le modèle n’invente ni pages nulles lisibles ni livraison de signal. Les mappings partagés, fixes, exécutables et JIT restent exclus.

`DarwinFiles` résout les FD et les octets, `DarwinMemory` gère placement, droits, budget et annulation. Seul `darwin_files` fournit les données. Le programme commun `file-mapping` vérifie copies privées, close, curseurs, erreurs et réutilisation anonyme ; une comparaison native séparée couvre un offset non nul, la page entière et SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### Vérification des mappings privés, 2026-10-05

Release Darwin : 438 identités uniques, 210 succès, 228 omissions, aucun échec ; 57/57 cas ARM64 HVF obligatoires exécutés et cinq combinaisons Unicorn. Neuf programmes macOS natifs, la comparaison de toute une page à offset non nul et SIGBUS dans un enfant isolé ont réussi. Les 36 contrôles API/rapport n’ont aucune omission ; Python couvre cinq combinaisons avec `file-mapping`, et 66 tests d’outils et 38 de provenance passent. Les comptes se recoupent. Preuves : `build-hvf-arm64/darwin-mmap-verified-evidence/`. Aucun nouveau résultat Intel HVF/KVM/WHP ou iOS physique ; Intel HVF Actions reste suspendu.

## Métadonnées explicites des fichiers

Une entrée peut ajouter `metadata` ; tous les champs ci-dessous sont alors obligatoires. Les chaînes décimales préservent toute la largeur ; les nombres JSON restent des entiers exacts dans ±(2^53−1). device est signé sur 32 bits, mode/link_count non signés sur 16, inode non signé sur 64, uid/gid/flags/generation non signés sur 32. size doit égaler le nombre d’octets ; blocks tient sur 64 bits signés, block_size sur 32 bits signés non négatifs. Les temps utilisent des secondes signées sur 64 bits et 0–999999999 nanosecondes.

`stat64` (338), `fstat64` (339) et `lstat64` (340) produisent le même enregistrement LP64 de 144 octets sur ARM64/x64. Ils partagent la résolution de open, suivent dup/close, sans allouer de FD ni modifier le curseur. rdev, remplissage et réserves sont nuls. Les entrées donnent les métadonnées initiales, puis la politique facultative régit les modifications ; read ne change pas les temps et mode ne modifie pas l’accès au catalogue. Métadonnées absentes, flux, ancien stat, et sécurité étendue restent exclus. Les erreurs de chemin/FD précèdent le pointeur de sortie ; une sortie partiellement accessible est refusée avant écriture. Le test natif compare tous les octets d’un fichier réel et les offsets du SDK ; le même programme original vérifie les trois appels.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### Vérification des métadonnées et suite (2026-10-05)

Après stat64 : 409 identités uniques, 193 réussites, 216 omissions, aucun échec ; les 54/54 cas ARM64 HVF obligatoires ont tourné, avec cinq combinaisons Unicorn. Comparaison SDK/enregistrement réel, huit programmes natifs, 36 cas API/rapport sans omission, cinq combinaisons Python et 66 tests des outils réussissent ; les comptes se recouvrent. Chaque cas natif utilise désormais son propre fichier de sortie, supprimant les octets résiduels après une sortie plus courte. Ces ajouts n’ont pas de preuve native Intel HVF/KVM/WHP ou iOS physique.

Suite : mappings partagés et défauts de page EOF, écritures bornées (pages EOF, durée après close, ordre des erreurs), observations explicites temps/système, services Mach/threads requis, puis dépendances Mach-O, rebases/binds, initialiseurs et TLS. Objective-C/Swift et Foundation/UIKit nécessitent des programmes natifs de référence. iOS physique requiert SDK et appareil ; Intel HVF reste non validé et ses Actions suspendues.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

La validation indépendante exige chacun des 111 cas natifs ARM64 ou 74 cas x64, dont `LC_MAIN` et `LC_UNIXTHREAD` sur chaque plateforme. Les cas obligatoires absents/ignorés ou un `ld64.lld` manquant font échouer la validation.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Sur Linux utilisez `kvm`, sur Windows `whp`. Le [workflow Darwin](../../.github/workflows/darwin-native.yml) exécute les deux transports x64 sans Unicorn et autorise une sélection isolée. La [référence noyau](../../.github/workflows/darwin-kernel-reference.yml) compare directement les programmes sur les deux ISA macOS, sans NeverD/LLVM ; `DarwinNativeCases.def` fixe modes, statuts et octets attendus. Seul l’exécutable de référence lie libSystem pour le vrai transfert dyld. Une ISA incorrecte, Rosetta, un dépassement de délai ou une divergence échoue. Cela ne valide pas le noyau d’un appareil iOS.

## Preuves et périmètre restant

Les lignes se recouvrent ; ne les additionnez pas. Résultats au 2026-10-03 :

| Transport | Source | Réussis | Échecs | Ignorés | Charges natives |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

Le [run Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) réconcilie 286 identités CTest et 32 processus avec XML original. Les 234 cas ignorés sont 65 cas Unicorn désactivés, 39 invités ARM64 et 130 autres plateformes hôtes. L’artefact `11267489438` a le SHA-256 vérifié `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. Les résultats [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) ont également été vérifiés indépendamment. La [référence noyau](https://github.com/NeverSight/NeverD/actions/runs/37064795867) a réussi 4/4 programmes sur chacune des deux ISA, avec statut 37, sortie exacte et stderr vide.

C API/CLI avec Unicorn : 138 réussites, 156 cas ignorés, aucun échec. Le SDK Python couvre les cinq combinaisons ; le moteur packagé correspond à 18 rapports CLI ARM64 et ses 186 images Mach-O passent les contrôles de signature. La configuration HVF/Unicorn OFF réussit 38 contrôles, en ignore 231 et ne lie pas Hypervisor.framework. Ce sont des preuves d’intégration, pas des exécutions natives supplémentaires. Le CPU Intel complet reste non vérifié ; voir [HVF](macos-hvf.md) et le [registre détaillé](../darwin-emulation.md#hosted-native-verification-2026-10-03).

## Observations temporelles explicites

`ProcessOptions::DarwinTime` / `darwin_time` fournit des observations fixes à l’appel brut `gettimeofday` (116), y compris sa troisième sortie `mach_absolute_time`, pour tous les profils Darwin. `time_of_day`, `timezone` et `mach_absolute_time` sont facultatifs : l’absence signifie inconnu, zéro explicite est une valeur. Un objet vide ne crée aucune horloge par défaut. Le modèle ne consulte pas l’horloge hôte, ne déduit pas le fuseau, ne fait pas avancer le temps et ne convertit pas les ticks absolus.

Chaque enregistrement fourni exige tous ses membres. `seconds` est non signé sur 32 bits, `microseconds` vaut [0, 999999], `minutes_west` / `dst_time` sont signés sur 32 bits, les ticks sont non signés sur 64 bits. JSON suit les règles entières sans perte ; les valeurs hors plage sûre sont des chaînes décimales. Champs inconnus, dépassements et profils non Darwin sont rejetés avant le chargement.

Le `timeval` LP64 fait 16 octets : secondes étendues par zéro à 0, microsecondes sur 32 bits à 8, quatre octets nuls à 12. Le fuseau contient deux champs signés de 32 bits, les ticks huit octets. Les temps civil et absolu sont échantillonnés ensemble avant toute copie ou vérification de pointeur : chaque observation demandée doit donc exister. Les copies suivent l’ordre timeval, timezone, absolute ticks. Un fuseau manquant ou un EFAULT ultérieur conserve les écritures précédentes ; les alias suivent le même ordre. Une sortie individuellement partiellement accessible provoque un arrêt explicite avant cette copie, sans annuler les précédentes. Tous les pointeurs nuls réussissent sans configuration ; une requête sélective exige uniquement ses valeurs.

Le programme original `time` vérifie le comportement natif ; `time-values` émet les 32 octets configurés via C/CLI/Python sur les cinq combinaisons invitées. Un oracle SDK compare chaque octet aux trois sorties d’un seul appel brut natif. Horloges évolutives, conversion, compteurs commpage, temporisateurs et objets horloge Mach/IPC restent absents, comme les travaux dyld, threads, Objective-C/Swift et Foundation/UIKit. Intel HVF Actions reste suspendu ; aucune validation native Intel ou iOS physique n’est ajoutée.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Validation temporelle (2026-10-06, Release) : 538 cas Darwin, 274 réussites, 264 ignorés pour backend indisponible, aucun échec ; 66/66 cas ARM64 HVF obligatoires exécutés. Les 12 programmes macOS natifs et la comparaison SDK d’un seul échantillon passent. C/CLI/rapports : 43/43, sans omission. Python passe sur les cinq combinaisons, avec les octets temporels exacts et les huit modes de fichiers existants. Les 66 tests des outils, la traduction, les capacités et le format passent. Comptages superposés. Preuves : `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/`, `darwin-time-public.xml`.

## Temps Mach et conventions de retour

`darwin_time.timebase` fournit `numerator` et `denominator`, deux entiers non signés de 32 bits non nuls. Le rapport reste exact, sans réduction ni conversion. `mach_timebase_info_trap`, indice 89, utilise ARM64 X16=-89 ou x64 RAX=0x01000059. Il écrit huit octets little-endian (numérateur, dénominateur) et renvoie zéro, même si toute l’adresse de sortie est invalide. Une sortie partiellement accessible arrête l’exécution avant copie ; les erreurs du transport se propagent. Une configuration absente arrête avant la vérification du pointeur, même nul.

ARM64 X16=-3 et X16=-4 renvoient les 64 bits non signés de `mach_absolute_time` et `mach_continuous_time`. Chaque appel exige uniquement sa propre valeur ; zéro explicite est valide. Les entrées natives x64 correspondantes déclenchent EXC_SYSCALL et restent non prises en charge. Horloges évolutives, commpage, temporisateurs et objets horloge Mach/IPC restent absents.

La résolution utilise les 32 bits bas du numéro ; le rapport conserve les 64 bits originaux. Les nombres négatifs ARM64 sélectionnent Mach ; x64 utilise 0x01000000 pour Mach et 0x02000000 pour BSD. BSD 3/4 restent read/write ; numéros inconnus et classes étrangères arrêtent explicitement. La liaison résolue détermine la convention : Mach conserve les flags et X1/RDX, BSD garde ses règles carry ; x64 met toujours RCX/R11 à jour. Les retours Mach contiennent `result` et omettent `error`, même avec carry initialement actif.

`mach-time` compare flags, résultat secondaire, bits hauts, pointeurs invalides et transitions BSD au noyau ARM64 natif. `mach-timebase-values` vérifie les octets exacts sur cinq invités, `mach-clock-values` sur ARM64 ; le SDK vérifie disposition et rapport capturé. Intel HVF Actions reste suspendu ; les essais logiciels et syntaxiques x64 ne valident ni Intel natif ni iOS physique.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Validation Mach (2026-10-06, Release) : 569 cas Darwin, 293 réussis, 276 ignorés pour backend indisponible, aucun échec ; 69/69 cas ARM64 HVF obligatoires exécutés. Les 13 programmes natifs et les deux oracles temporels SDK ont réussi lors du passage final. C/CLI/report : 100/100 sans omission ; Python couvre cinq invités. Les comparaisons publiques sont isolées par plateforme et scénario, avec un budget invité explicite de 10 secondes ; valeurs produit et régressions de délai restent inchangées. Les comptes se recoupent.

Les premiers démarrages natifs dépassaient la limite existante de 5 secondes : mesure indépendante de 6.056 secondes, puis 0.010 à la réutilisation. Le même binaire a ensuite réussi les 13 cas sous la limite originale ; les échecs sont conservés. La vérification séquentielle séparée a réussi après des délais dépassés sous charge hôte. Preuves : `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`, sur l’arbre avant commit. À cette révision, ARM64 MRS/MSR NZCV étaient absents du contrat checked ; le test observait donc les flags avec des instructions entières. Le changement ci-dessous comble cette lacune CPU.

## Registre des flags de condition ARM64

Le contrat ARM64 checked partagé admet les encodages exacts `MRS Xt, NZCV` et `MSR NZCV, Xt` en EL0/EL1. La lecture ne renvoie que les bits 31–28 ; l’écriture sélectionne ces quatre bits d’entrée et ignore les autres. Lire vers `XZR` abandonne le résultat ; écrire depuis `XZR` efface les flags sans lire SP. Chaque backend exécute les instructions originales. La validation du setter hôte et les limites FPCR/FPSR restent inchangées ; les registres système voisins non déclarés restent non pris en charge.

`NeverDAArch64NZCVTests` compare toutes les combinaisons aux instructions hôtes et vérifie l’état scalaire/vectoriel complet, la mémoire, les registres limites, l’arrêt/l’échec des observateurs, la reprise du contexte et les budgets partagés. ARM64 `mach-time` utilise désormais de vrais MSR/MRS autour de SVC pour vérifier la conservation Mach et la transition vers BSD. Les exigences HVF natives comprennent les six méthodes aux deux privilèges et l’oracle hôte. ARM64 KVM/WHP et iOS physique restent non validés. Fichiers modifiables, informations système, horloges progressives, Mach IPC/threads, dyld/runtimes/frameworks et validation sur appareil restent à réaliser.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


Validation des fichiers modifiables (2026-10-06) : Release Darwin, 610 inscriptions, 322 réussites, 288 ignorées pour backend indisponible, zéro échec ; 72/72 obligations ARM64 HVF exécutées. La vérification finale, avec les nouvelles assertions EFAULT/métadonnées, compte 102 réussites et 12 ignorées sur 114. Les 15 programmes natifs et 111 tests publics C/CLI/rapports passent aussi. Les comptes se recoupent. Le premier essai natif a révélé FWASWRITTEN, corrigé avant réussite ; son échec est conservé. Aucun délai changé. CI GitHub complète et appareil iOS restent séparés ; Actions Intel suspendues.

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python a d’abord dépassé cinq secondes dans trois cas de répertoire ARM64. L’observation sans changer les arguments a réussi les dix nouveaux cas inscriptibles ; un répertoire iOS a expiré à 5,005 s réelles pour 1,263 s CPU. Les trois reprises isolées, même limite, passent en 2,43–3,17 s : 10 941 instructions, sortie65. Charge54–70 pour16 CPU logiques : indice de pression de planification, pas une garantie de latence ; échecs conservés.

La méthode Python finale inchangée a réussi les cinq combinaisons en41,118 s, avec cinq secondes par processus. Les échecs et diagnostics précédents restent séparés.


Validation des métadonnées (2026-10-06) : Release ciblé148=124 réussites/24 ignorés. Darwin complet645=343 réussites/300 ignorés/2 délais dépassés dans l’énumération ARM64 HVF existante. Reprise identique20=8 réussites/12 ignorés,3.818/3.949s pour les cas concernés, limite initiale5s. Les75 identités HVF obligatoires ont des observations réussies ; le premier échec reste conservé. Public C/CLI/rapports117/117 dont73 Darwin, Python cinq profils27.359s, natif15/15, runners66/66 réussis. Allocation virtuelle, pas preuve APFS ; aucun délai modifié. CI complète, Intel, iOS physique et environnement complet restent à valider.

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

Validation du positionnement sparse (2026-10-06) : Release Darwin, 671 cas, 359 réussis, 312 ignorés pour backends indisponibles, aucun échec. Les 78 cas ARM64 HVF obligatoires ont été exécutés ; Unicorn couvre cinq profils. Vérifications ciblées : 123 réussites sur 147, 24 ignorés. Les 16 programmes natifs, 122 contrôles C/CLI/report (78 comparaisons Darwin), cinq profils Python (12.344 s) et 66 tests du runner passent. Comptages recoupés, délais inchangés, échecs historiques conservés. Preuves : `build-hvf-arm64/sparse-seek-validation-summary.json`. La géométrie relève de la politique virtuelle explicite, sans équivalence APFS. CI complète et iOS physique restent à valider ; Intel HVF Actions reste suspendu.

Validation unlink (2026-10-06) : Release Darwin 708 cas, 384 réussis, 324 ignorés pour backends indisponibles, aucun échec ; 81 ARM64 HVF obligatoires exécutés. Ciblés : 137/156 réussis, 19 ignorés. Natifs17/17, C/CLI/report128/128 (Darwin83), Python cinq profils16.268s, runner66/66 réussis. Revue indépendante sans blocage restant. Comptages recoupés, délais inchangés, aucune reprise nécessaire. Preuves : `build-hvf-arm64/unlink-validation-summary.json`. Invalidation et temps fixes sont des règles du modèle ; système de fichiers/runtime complet et iOS physique restent à valider. Intel HVF Actions suspendu, CI complète distincte.

### Validation de la création, 2026-10-06

Release Darwin : 748 cas, 412 réussis, 336 ignorés faute de backend, aucun échec ; 84 obligations ARM64 HVF exécutées. Ciblés162 :150 réussis/12 ignorés. C/CLI/rapports133/133 (Darwin88), Python5 profils9.982s, natif18/18, runners66/66 réussis. Le premier ARM64 refusait correctement les rebases de la table de pointeurs du test ; son remplacement par des octets intégrés corrige le fixture sans assouplir le chargeur. Échecs/binaires initiaux conservés, inventaire attendu27→28. Revue indépendante sans blocage, avec conservation des observations parentales après refus de budget. Comptages chevauchants, délais inchangés. CI complète et iOS physique séparés ; Actions Intel HVF suspendues.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### Validation des métadonnées de création, 2026-10-06

Release Darwin : 787 cas, 439 réussis, 348 ignorés pour backends indisponibles, aucun échec ; les 87 ARM64 HVF obligatoires exécutés. Ciblés : 139/151 réussis, 12 ignorés. C/CLI/rapport : 145/145, dont 98 comparaisons d’entrées Darwin ; méthode Python inchangée avec cinq profils en 12.211 secondes. Natifs 19/19 et scripts 66/66 réussis. Revue indépendante sans blocage ; nouveaux cas : parents device/GID distincts et inode global, unlink avant première écriture, umask sans FD libre/entrée utilisable. Comptages recoupés, délais inchangés, aucune reprise après échec nécessaire. Temps fixes de création/mutation et allocation restent des politiques virtuelles. GitHub CI complète et iOS physique restent distincts ; Intel HVF Actions suspendu.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### Vérification du renommage, 2026-10-06

Release Darwin : 835 inscriptions, 474 succès, 360 indisponibles et un dépassement existant macOS ARM 64 HVF de métadonnées virtuelles (5.087 s). Même méthode, paramètres et limite de 5 s : 8 succès et 12 ignorés, identité concernée 0.113 s. Les 90 identités ARM 64 HVF obligatoires ont une observation réussie sur ces exécutions ; le premier gate final reste enregistré en échec. Ciblés 42/54 réussis,12 ignorés ; C/CLI/rapport 150/150 dont 103 comparaisons Darwin ; Python inchangé, cinq profils en 18.478 s ; natifs 20/20, scripts 66/66. La revue indépendante a trouvé le classement incorrect du point imbriqué, reproduit en 4 K/16 K puis corrigé. Une ancienne attente de test ftruncate en lecture seule a été corrigée vers EINVAL. Échecs et versions des sondes conservés, comptes recouvrants, délais inchangés. GitHub CI complet et iOS physique restent distincts ; Actions Intel HVF suspendues.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## Observations système explicites

`ProcessOptions::DarwinSystem` / `darwin_system` fournit des observations fixes à `sysctl(202)` et au `sysctlbyname(274)` brut pour chaque profil Darwin. Chaque champ est facultatif ; une valeur absente ou une clé non répertoriée reste non prise en charge. Aucune interrogation de l’hôte ni version ou modèle implicite. La validation JSON stricte et C++ rejette les valeurs incorrectes et les profils non Darwin avant le chargement.

`os_revision` est signé sur 32 bits ; `cpu_count` vaut 1..INT32_MAX ; `memory_size` conserve 64 bits non signés ; `max_files_per_process` vaut 0..INT32_MAX et utilise un int de quatre octets. Les autres champs scalaires sont des chaînes de 1023 octets maximum (255 pour `hostname`) sans NUL interne ; une chaîne vide explicite est valide et la sortie comprend le NUL final. Ces observations ne modifient ni ordonnancement ni budgets de mémoire ou de descripteurs.

| Champ JSON | Nom sysctl | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |
| `max_files_per_process` | `kern.maxfilesperproc` | `1,29` |
| `hostname` | `kern.hostname` | `1,10` |

`hw.pagesize` utilise la politique mémoire du guest : huit octets normalement, quatre si la sortie non nulle a une capacité exactement égale à quatre. Le MIB historique `[6,7]` et `hw.pagesize_compat` renvoient toujours quatre octets. L’OID numérique dynamique de `hw.pagesize` reste non pris en charge. À capacité quatre, `hw.memsize` se réduit seulement si son motif 64 bits est l’extension signée d’un entier 32 bits ; sinon ERANGE34 conserve sortie et longueur.

Le nombre MIB utilise les 32 bits bas et doit être 2–12 ; la longueur du nom utilise 64 bits et reste inférieure à 1024. Tous les octets fournis sont vérifiés avant le premier NUL et le retrait d’un point final. Un nom vide donne ENOENT ; une entrée partiellement lisible reste non prise en charge. Un `oldlenp` non nul exige huit octets entièrement lisibles et modifiables avant tout effet. Les sondes natives de pointeurs de longueur invalides n’ont pas terminé dans leur délai : ce cas reste explicitement hors périmètre. `oldlenp` nul signifie capacité zéro ; `oldp` nul demande seulement la taille. Pour les clés autres que `kern.hostname`, un tampon court donne ENOMEM12 sans changer les données et écrit une longueur zéro. EFAULT sur les données conserve la longueur. Entrée et capacité sont capturées avant les données, puis la longueur est copiée en dernier, avec conservation des alias et des copies déjà effectuées en cas d’erreur de transport ultérieure.

`hostname` déclare les octets visibles par cet appelant guest, sans interrogation de l’hôte, valeur mobile `localhost` implicite ni déduction d’entitlements. L’absence reste inconnue ; la chaîne vide explicite renvoie un NUL. Pour `kern.hostname`, une sortie non nulle de capacité positive insuffisante réussit avec exactement cette capacité d’octets, terminés par NUL, et rapporte cette capacité. La capacité zéro conserve ENOMEM12, longueur zéro et données intactes ; une sortie nulle rapporte la longueur complète avec NUL. Seule la plage réellement copiée est vérifiée. Une plage partiellement modifiable reste non prise en charge sans publication de préfixe ; les copies natives partielles sont hors modèle. Seule l’observation raw utilisée par libc uname/gethostname est ajoutée, pas leurs imports dylib ni un runtime complet.

newp/newlen non nuls constituent l’écriture. La lecture nom/MIB et le contrôle complet lecture/écriture oldlenp restent antérieurs. EUID par défaut ou non-root explicite renvoie EPERM1 avant observation/sortie de données. EUID0 arrête kern.osversion / kern.maxfilesperproc / kern.hostname unsupported car son écriture privilégiée n’est pas modélisée ; RUID ne décide pas. Les autres nœuds natifs en lecture seule gardent EPERM1 même root. Longueur nouvelle0 ignore le pointeur ; aucun ENOENT inventé pour clé/arbre/OID dynamique inconnu.

Le programme original `system-info` compare l’ABI native macOS et guest ; `virtual-system` compare les octets configurés via C++, C/CLI et Python. Un oracle SDK capture les neuf observations de l’hôte comme entrées explicites de test et compare sorties nommées et numériques. Cela ne valide ni iOS physique ni Intel HVF.

```json
{"darwin_system":{"hostname":"guest-node","os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man 3/sysctl.3.html).

### Validation des requêtes système, 2026-10-06

Release Darwin : 881 inscriptions, 509 succès, 372 indisponibles ignorés, aucun échec ; les 93 identités ARM64 HVF obligatoires ont été exécutées. Ciblés : 37/49 réussis et 12 ignorés. C/CLI/rapport : 163/163, dont 113 comparaisons Darwin. Méthode Python inchangée : cinq profils en 15.302 s ; programmes natifs 21/21, scripts 66/66. Revue indépendante sans blocage ; combinaisons supplémentaires de priorité et oracle SDK réussis. Une compilation du nouvel oracle a échoué faute de StringExtras, puis réussi après ajout ; source et journal conservés. Les sondes natives de longueur invalide restent conservées et hors périmètre. Après les tests, seuls deux commentaires d’en-tête ont été normalisés, puis la reconstruction a réussi. Comptes recouvrants, délais inchangés, aucune répétition après échec d’exécution nécessaire. GitHub CI complet et iOS physique restent distincts ; Actions Intel HVF suspendues.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## Entrées-sorties vectorielles et capture

`readv`/`writev`, `preadv`/`pwritev` et leurs entrées nocancel partagent les traitements scalaires des fichiers et de la capture, sans nouvelle option ni accès hôte. Chaque iovec LP64 contient une adresse et une longueur de huit octets. Les 32 bits bas signés de iovcnt doivent valoir 1–1024. Le tableau entier est copié avant la recherche du descripteur, donc les alias de sortie ne modifient pas la demande. Un tableau partiellement lisible reste non pris en charge.

Les droits et la possibilité de positionner un flux précèdent les longueurs : chacune et leur somme doivent tenir dans INT64_MAX, avec une limite supplémentaire INT_MAX pour fichiers et répertoires. Le stdin fini est limité aux octets disponibles ; la capture garde son budget. pwritev refuse tout décalage négatif avant le tableau ; preadv vérifie le décalage après le descripteur et les longueurs. Les éléments vides ignorent leur adresse, mais gardent les contrôles de descripteur, type et position. EOF évite les éléments inutilisés. Les appels positionnés préservent le curseur et pwritev ignore l’ajout. L’ajout ordinaire borne toute la demande une seule fois avec le curseur initial, puis choisit EOF.

Un élément ultérieur entièrement invalide renvoie EFAULT et conserve les octets précédents, le curseur ordinaire et FWASWRITTEN après écriture d’au moins un octet. Une écriture non vide admise qui renvoie EFAULT sur un tampon de données invalide les métadonnées complètes ; erreurs d’arguments, refus du modèle et erreurs backend les préservent. Une destination de lecture partiellement accessible provoque UnsupportedService sans copier cet élément, en conservant les copies antérieures. Une source de fichier partiellement lisible reste non prise en charge avant tout effet sur le fichier. Autorisation, baux de mapping et budget total précèdent l’écriture ; les erreurs de précontrôle ou lecture du backend ne publient aucun octet de fichier ou capture.

La capture contrôle d’abord le budget commun stdout/stderr. Un élément franchissant la limite d’adresse utilisateur ne contribue aucun octet ; les précédents restent acquis. Les autres préfixes lisibles sont capturés avec EFAULT. La priorité scalaire de l’erreur de plage sur le budget est conservée. Les descripteurs dupliqués ou redirigés gardent leur destination. Le programme original `vectored-io` vérifie les huit entrées sur macOS natif, les cinq combinaisons invitées et C/CLI/Python. Aucun ajout de cancellation, pipes, threads ou validation iOS physique.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### Vérification des E/S vectorielles, 2026-10-06

Release Darwin :937 inscriptions,553 réussites,384 sauts de backend indisponible, aucun échec ;96 identités ARM64 HVF obligatoires exécutées. Ciblés :45/57 réussis,12 sauts. C/CLI/report :168/168, dont118 comparaisons Darwin ; Python couvre cinq combinaisons en 20.397s. Natif :22/22 ; scripts :66/66. La revue indépendante a ajouté un défaut d’écriture positionnée creuse vérifiant curseur, EOF réel, refus des métadonnées et capacité restante exacte. La première compilation référençait une requête interne supprimée dans un ancien test, désormais remplacée par la vérification de la sortie réelle. Une erreur optional<bool> dans la nouvelle assertion d’événement a fait échouer huit exécutions invitées pourtant réussies ; correction et contrôles suivants réussis. Sources et journaux des deux échecs conservés. Comptages recoupés, délais inchangés. GitHub CI complète et iOS physique séparés ; Intel HVF Actions suspendu.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## Requêtes d’existence de fichiers

`access(33)` et `faccessat(466)` interrogent le catalogue virtuel courant sans allouer de descripteur ni changer contenu, curseurs, drapeaux ou métadonnées. F_OK prouve l’existence du nom selon le contrat de parcours existant. Les métadonnées n’accordent ni ne retirent l’accès au catalogue ; les droits natifs de recherche des ancêtres, ACL et MAC ne sont pas validés. Des observations stat absentes ou invalidées n’empêchent pas la requête. Un nom supprimé donne ENOENT même si des FD ou mappings gardent l’objet ; création, réutilisation et renommage suivent l’espace courant.

Le mode utilise les 32 bits bas. R/W/X occupe les bits 0–2, les droits étendus 9–21. `(mode & 0x003ffe07) == 0` signifie existence ; les autres bits, signe compris, sont ignorés sans EINVAL. Une demande de permissions reste UnsupportedService après une recherche réussie, sans déduction des métadonnées ou autorisations de mutation. Les erreurs connues de chemin/descripteur passent avant.

Faccessat accepte toute combinaison des drapeaux bas AT_EACCESS(0x10), AT_SYMLINK_NOFOLLOW(0x20), AT_SYMLINK_NOFOLLOW_ANY(0x800). Les autres donnent EINVAL avant chemin ou FD, même sans catalogue. Les identités réelle/effective sont fixes; les liens fixes suivent les règles ci-dessous. Un chemin absolu ignore dirfd ; un chemin relatif garde les règles CWD/FD de répertoire. nameiat hors AT_FDCWD lit un octet, vérifie le FD relatif puis importe la chaîne entière; `/` évite le FD. Premier octet inaccessible: EFAULT14; FD inconnu/fichier: EBADF9/ENOTDIR20 avant faute ultérieure. Un chemin relatif vide vérifie encore le FD : EBADF si inconnu, ENOTDIR pour fichier, sinon ENOENT. Catalogue absent ou nature de répertoire d’un flux inconnue restent non pris en charge.

L’original `file-access` compare les deux appels, bits ignorés, drapeaux et ordre sur macOS natif et cinq invités via C++/C/CLI/Python. NOFOLLOW_ANY utilise un FD relatif pour éviter les liens hôtes `/tmp` ou `/var`. Les tests directs couvrent noms vivants, bits mixtes, épuisement des FD, indépendance des métadonnées et erreurs mémoire.

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### Vérification de l’existence de fichiers, 2026-10-06

Release Darwin :971 inscriptions,575 réussites,396 sauts de backend indisponible, aucun échec ;99 identités ARM64 HVF obligatoires exécutées. Ciblés :23/35 réussis,12 sauts, dont14 tests directs. C/CLI/report :173/173, dont123 comparaisons Darwin. Python couvre cinq combinaisons en 16.235s ; natif23/23 et scripts66/66. La revue indépendante du plan et du code ne relève aucun blocage. Le résultat initial NOFOLLOW_ANY lié au lien hôte /tmp et la comparaison avec chemin canonique restent conservés ; le programme partagé utilise un FD de répertoire relatif. Comptages recoupés, délais inchangés, aucun échec d’exécution à retester. GitHub CI complète et iOS physique séparés ; Intel HVF Actions suspendu.

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## Création et suppression de répertoires

`mkdir(136)` et `mkdirat(475)` créent dans un parent direct explicitement modifiable. Les nouveaux répertoires héritent de l’autorisation de modifier les noms ; les répertoires initiaux gardent leurs propres droits déclarés. Seuls device/GID connus du parent sont hérités, jamais stat complet, taille, allocation, temps ou cookies. Un nouveau répertoire masque toutes les observations d’un ancien fichier homonyme. `creation_policy` reste réservée aux fichiers ordinaires : leurs descendants utilisent cette identité et la séquence globale d’inodes ; mkdir n’en consomme aucun. Contrôle des permissions et métadonnées natives de répertoire restent exclus.

Le parcours commun admet un nom final absent suivi uniquement de slashs pour mkdir. Un ancêtre absent avant point/double-point donne ENOENT, un fichier ancêtre ENOTDIR et un nom existant EEXIST. FD/CWD relatifs, indépendance absolue du FD et priorité des fautes de chaîne restent inchangés. Aucun FD libre n’est nécessaire ; refus de recherche, autorisation, budget ou transport ne publient rien.

`rmdir(137)` et `unlinkat(472)` avec AT_REMOVEDIR(0x80), éventuellement AT_SYMLINK_NOFOLLOW_ANY(0x800), suppriment les répertoires vides créés par ce processus. Les bits bas32 inconnus donnent EINVAL avant les entrées ; DATALESS et SYSTEM_DISCARDED restent exclus. Les erreurs connues de chemin/type/racine restent ; supprimer un répertoire initial sans autorisation removable reste UnsupportedService. Sur un répertoire admis, point final donne EINVAL, double-point depuis un répertoire lié ou cible non vide ENOTEMPTY. Les FD de répertoire, dup et CWD gardent l’objet original et ne bloquent plus sa suppression. Les fichiers ordinaires déjà unlink et leurs mappings ne sont pas des noms : les comparaisons natives conservent contenu, inode et dernier F_GETPATH après suppression/réutilisation du parent.

Chaque nouveau chemin canonique+NUL et une entrée rejoignent le budget commun16 MiB/256 entrées. Seul ce coût est remboursé après disparition de toutes les références au répertoire supprimé, sans libérer les fichiers orphelins ou mappings. Seul le succès invalide stat/énumération du parent ; les observations complètes des nouveaux répertoires restent inconnues. L’original `directory-mutations` compare création imbriquée, renommage, unlink, suppression et réutilisation sur macOS natif et cinq invités via C++/C/CLI/Python. Un fichier orphelin retenu par une description ou un bail de mapping retient aussi ses répertoires parents et leurs coûts chemin/NUL et entrée, même après fermeture de tous leurs FD. La récupération du fichier précède celle de la chaîne des répertoires. Déplacer un ancêtre créé vivant actualise F_GETPATH ; la réutilisation du nom ne rattache pas les anciens objets au remplaçant.

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### Vérification des modifications de répertoires, 2026-10-06

Release Darwin :1 017 cas,609 réussites,408 sauts de backend indisponible,aucun échec ;102 ARM64 HVF obligatoires exécutés. Ciblés :58 réussites,12 sauts,dont26 nouveaux cas directs4K/16K. C/CLI/report178/178,dont128 Darwin ;Python cinq combinaisons en 17.255s,natifs24/24,scripts66/66. Revue indépendante des budgets,réutilisations,identités parentes,baux et annulations. Une sonde native après le premier passage réussi a révélé EISDIR pour la racine uniquement en barres,contre EBUSY avec point/deux points finaux. Décision et tests natifs/invités sont partagés ;premiers résultats et instantanés sources/binaires conservés. Comptages recoupés,délais inchangés,Intel HVF Actions suspendu ;GitHub CI complète et iOS physique séparés.

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## Identités de répertoires conservées

FD/CWD gardent l’objet supprimé et sa chaîne de parents malgré la réutilisation des noms. Ouvrir un point crée un curseur indépendant ; dup le partage ; deux points suivent le parent original. Un enfant ordinaire d’un répertoire supprimé donne ENOENT. LOOKUP peut traverser les parents supprimés conservés, tandis que création/suppression/renommage donnent ENOENT. Le point final d’un renommage donne EINVAL avant sa traversée, après les erreurs précédentes. F_GETPATH conserve le dernier chemin ; stat et énumération complets restent inconnus. Le coût chemin+NUL et une entrée persiste jusqu’à libération de tous FD/CWD/enfants retenus. Close/dup2/changement de CWD/admission de mutation récupèrent les chaînes inaccessibles ; coûts initiaux et baux de fichiers restent séparés. `deleted-directories` compare ces durées, réutilisations et intentions avec macOS natif et cinq invités. Un fichier orphelin retenu par une description ou un bail de mapping retient aussi ses répertoires parents et leurs coûts chemin/NUL et entrée, même après fermeture de tous leurs FD. La récupération du fichier précède celle de la chaîne des répertoires. Déplacer un ancêtre créé vivant actualise F_GETPATH ; la réutilisation du nom ne rattache pas les anciens objets au remplaçant.

### Vérification des durées de répertoires, 2026-10-06

Release Darwin1 051 cas,631 réussites,420 sauts indisponibles,zéro échec;105 ARM64 HVF obligatoires exécutés. Ciblés98 réussites/12 sauts,directs initiaux64/64 dont14 nouveaux. C/CLI/report183/183,dont133 Darwin;Python cinq combinaisons 18.691s,natifs25/25,scripts66/66. Une sonde native supplémentaire a corrigé l’ordre du point final de renommage;premiers sources/résultats/instantanés conservés. Audit des preuves par l’agent principal;revue indépendante finale indisponible. Comptages recoupés,délais identiques,CI complète/iOS physique séparés,Intel HVF Actions suspendu.

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## Suppression des répertoires initiaux explicitement admis

Le booléen strict `"removable": true` d’une entrée de répertoire (C++ `DarwinFileOptions::RemovableDirectories`) déclare un répertoire ordinaire, sans montage et avec une seule identité dans l’espace de noms. Il faut une entrée initiale explicite `directories`, différente de la racine, et un parent immédiat explicitement modifiable. Les modes/drapeaux spéciaux connus, alias d’inode (instantanés compris) et numéros de périphérique parent/cible contradictoires sont rejetés. Des numéros égaux ne prouvent pas l’absence de montage. Omission/false restent non pris en charge ; les autres types JSON sont invalides. Cela ne fournit pas un modèle général des droits ou montages.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

La suppression exige un espace de noms courant vide. Un sous-répertoire initial implicite subsiste après unlink de son dernier fichier d’origine. La réussite invalide les observations complètes stat/énumération de l’objet et du parent immédiat ; les anciens FD/dup/CWD conservent l’objet et sa chaîne de parents. Les entrées immuables ne ressuscitent jamais un nom supprimé. Un nouveau fichier ou répertoire au même nom a une identité séparée, sans reprendre les anciennes métadonnées ou l’instantané. Les données de l’appelant restent intactes.

Chaque référence removable ajoute son chemin et NUL au coût initial fixe de 16 MiB. Entrées, chemins, références et instantanés initiaux restent comptés après suppression et fermeture finale, y compris leur place dans la limite de 256 entrées ; les nouveaux objets gardent leur comptabilité dynamique propre. Le programme original `initial-directory-removal` supprime le répertoire vide préexistant du test natif tout en le gardant ouvert, réutilise son nom pour un fichier puis un répertoire, vérifie la rétention par CWD seul et restaure le répertoire vide. Il est partagé par C++/C/CLI/Python sur les cinq combinaisons invitées.

### Validation de la suppression des répertoires initiaux, 2026-10-06

Les sources finales en Release ont réconcilié 1 089 tests Darwin : 657 réussites, 432 omissions pour backend indisponible, aucun échec ; les 108 cas ARM64 HVF obligatoires ont été exécutés. Les contrôles ciblés ont réussi 27/39 cas avec 12 omissions ; le contrôle supplémentaire des alias issus uniquement des instantanés a réussi. C/CLI/rapports publics : 191/191 ; Python : cinq combinaisons en 76,276 s ; charges natives originales : 26/26 ; exécuteurs de preuves : 66/66. Une entrée de test inode/instantané incohérente a été corrigée et ses échecs conservés.

Deux exécutions complètes antérieures ont produit un puis trois dépassements de délai dans les anciens cas de fichiers/renommage. Une exécution instrumentée en a reproduit un à 5,008 s réelles et 0,171 s de CPU du processus. Les comparaisons par méthode identique et avec les anciens programmes ont réussi, mais la cause de cette latence reste inconnue ; la réussite finale ne prouve pas la stabilité des délais. Les diagnostics temporaires ont été retirés, les empreintes des programmes restaurées et la limite invitée de 5 s conservée. Audit principal des sources/preuves effectué ; revue indépendante indisponible. Les comptes se recoupent. CI GitHub complète, iOS physique et Actions Intel HVF suspendues restent hors de cette validation locale.

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## Renommage exclusif des fichiers ordinaires

Après résolution des deux chemins, RENAME_EXCL renvoie EEXIST pour un autre fichier ou répertoire existant, avant les contrôles de montage et de mutation. Les erreurs de chemin antérieures restent prioritaires, notamment EINVAL pour un point/double point terminal. Une cible absente utilise la même transaction bornée, conservant descriptions ouvertes, curseurs, flags, baux de mapping et transitions de métadonnées configurées. Le même objet reste explicitement non pris en charge : le résultat natif dépend de la sensibilité à la casse du système de fichiers, non établie par les clés exactes du catalogue. Casse, sources répertoires initiales et SECLUDE restent hors contrat. Le programme original `renamed-file` compare désormais refus sans changement de métadonnées et réussite EXCL|NOFOLLOW_ANY sur macOS natif et C++/C/CLI/Python.

Validation, 2026-10-06 (Release) : 1 097 tests Darwin, 665 réussites, 432 omissions pour backend indisponible, aucun échec ; les 108 cas ARM64 HVF obligatoires ont été exécutés. Contrôles ciblés : 44 réussites, 12 omissions, dont huit nouveaux cas directs. C/CLI/rapports : 191/191 ; Python : cinq combinaisons en 19,241 s ; sonde indépendante : 26 contrôles réussis. La première exécution native a expiré sur le cas return existant ; les 25 autres, dont renamed-file, ont réussi. Trois contrôles return du même binaire inchangé ont pris 0,014–0,034 s, puis les 26 cas ont réussi avec la limite initiale de 5 s. L’échec initial est conservé et inexpliqué ; ces résultats et la réussite HVF précédente ne prouvent pas la stabilité de latence. Audit principal terminé, revue indépendante indisponible. Comptes chevauchants ; iOS physique, CI GitHub complète et Actions Intel HVF suspendues hors validation locale.

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## Renommage entre répertoires créés

Un répertoire initial et tous ses descendants créés par ce processus avec mkdir/mkdirat partagent un domaine de noms virtuel. `rename`, `renameat` et `renameatx_np` déplacent un fichier ordinaire entre ces parents sans nouveau champ JSON. Après création de `/work/left` et `/work/right` sous le `/work` initial modifiable, `/work/data` peut passer aux enfants et entre eux. Un `/work/left` initial déclaré séparément conserve son propre domaine, même avec le même device. La topologie générale des montages reste inconnue.

Les deux parents immédiats exigent une autorisation ; les répertoires créés l’héritent avec les device/GID connus. Identité, propriétaire/groupe, droit d’écriture et allocation du fichier sont préservés. Les devices contradictoires sont refusés. Un déplacement invalide stat/énumération complets des deux parents. Répertoires initiaux supprimés et chemins réutilisés restent des objets distincts ; anciens FD/CWD ne reçoivent pas le domaine du remplacement.

Transaction bornée, leases de mappings, coûts chemin/NUL et priorité des erreurs restent valables. EXCL sur une cible existante distincte donne EEXIST avant domaine/autorisation. `renamed-file` compare déplacement vers l’enfant créé, remplacement dans le parent initial et retour via C++/C/CLI/Python et macOS natif. Permissions, déplacement des répertoires initiaux, liens physiques, liens symboliques dynamiques et métadonnées APFS natives restent incomplets.

### Vérification entre parents, 2026-10-06

La validation Release finale a réconcilié 1,115 inscriptions Darwin : 683 réussites, 432 exclusions pour backend indisponible, aucun échec ; les 108 cas ARM64 HVF obligatoires ont été exécutés. Les contrôles directs ont réussi 56/56, dont 18 nouveaux cas 4K/16K. C/CLI/report public : 191/191 ; la méthode Python a couvert cinq profils en 22.254s. Programmes natifs originaux : 26/26 ; sonde indépendante d’appels bruts : 34 contrôles ; scripts de documentation/capacités/exécution des preuves : 296/296. Les comptes se recoupent. Les empreintes des dix binaires de validation n’ont pas changé après l’ajustement CMake limité à MSVC.

Les deux validations complètes précédentes conservent trois et deux dépassements de délai dans la méthode HVF de fichiers existante. Les comparaisons de méthode entière, répertoire de travail et session ont réussi sans établir la cause ; la réussite finale ne prouve pas la stabilité de latence. La sonde comparait initialement /tmp au chemin canonique /private/tmp ; lire le chemin du FD racine a corrigé quatre attentes. Le programme natif étendu utilisait mkdir(136) pour le nettoyage ; rmdir(137) a corrigé exit150. Sources et échecs initiaux sont conservés, et la limite invitée reste 5s.

Quatre conflits LP64 de listes d’initialisation détectés par la CI Linux complète utilisent désormais des valeurs uint64_t explicites ; NeverDJumpTableTests reçoit /bigobj sous MSVC. La compilation Linux/Windows réelle attend la CI. La CI complète antérieure signalait aussi des échecs distincts du corpus Windows EH et de l’annulation d’une PR fermée. Une auto-revue des sources/preuves a été réalisée ; aucune revue indépendante ni validation iOS physique ou Intel HVF suspendue n’est revendiquée.

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## Échange atomique de noms de fichiers ordinaires

RENAME_SWAP=0x2 échange les noms de deux fichiers ordinaires existants via renameatx_np, avec RENAME_NOFOLLOW_ANY en option. Un répertoire initial explicite doit déclarer mutable:true et swap_rename:true ; C++ utilise DarwinFileOptions::SwapRenameDirectories. Les descendants créés héritent de la capacité de l'objet répertoire initial. Supprimer puis réutiliser un chemin ne transfère pas l'ancienne déclaration. false ou omission laisse la capacité inconnue ; mêmes devices et autorisations ne prouvent pas le support. Les domaines initiaux distincts restent exclus.

Les deux chemins utilisent le résolveur existant. Une cible absente donne ENOENT avant domaine, autorisation et capacité. Tout opérande répertoire est explicitement refusé : le swap natif peut échanger fichier et répertoire, donc EISDIR du renommage ordinaire ne s'applique pas. Le même objet autorisé est une opération sans effet, même sans déclaration de capacité. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 et flags inconnus donnent EINVAL avant les chemins ; SECLUDE reste indisponible.

Les deux fichiers restent liés. Identité, propriétaire/groupe, octets, autorisation d'écriture, descriptions, curseurs, flags et baux de mapping propres sont conservés. Les politiques virtuelles configurées mettent à jour chaque ctime propre ; une politique absente ou invalidée laisse les métadonnées complètes inconnues. Un échange réel invalide métadonnées et énumération complètes des deux parents. Aucun inode de création, entrée ou FD n'est consommé ; les entrées du demandeur restent intactes.

Chaque référence de capacité réserve chemin+NUL dans le budget initial fixe de 16 MiB. La transaction vérifie les deux coûts dynamiques complets avant publication ; octets et baux encore liés ne financent aucun crédit de remplacement. Les échanges répétés réutilisent ces coûts. Le programme original renamed-file échange vers un enfant créé puis revient, vérifie les deux objets et poursuit le remplacement ordinaire sur macOS natif et tous les profils C++/C/CLI/Python. Permissions, topologie des montages, casse, déplacements des répertoires initiaux restent séparés.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

[Apple volume swap capability](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2), [XNU rename](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

### Vérification du swap, 2026-10-06

Release Darwin : 1 133 inscriptions, 701 réussites, 432 exclusions pour backend indisponible, aucun échec ; les 108 cas ARM64 HVF obligatoires ont exécuté. Contrôles directs rename 68/68, dont 18 nouveaux cas d'options et 4K/16K. C/CLI/report public 192/192 ; la méthode Python a couvert cinq profils en 16,153s. Programmes natifs originaux 26/26, sonde indépendante d'appels bruts 45 contrôles. Comptes chevauchants ; délais invités inchangés.

Deux premiers tests directs attendaient des diagnostics incorrects pour autorisation d'écriture inconnue et métadonnées inconnues après mutation ; seules les attentes ont été corrigées. Un premier filtre JSON a sélectionné zéro test et ne compte pas ; le propriétaire réel et la vérification publique complète ont ensuite réussi. Sources et résultats initiaux sont conservés. La latence HVF/native antérieure reste inexpliquée ; ce passage ne prouve pas la stabilité. Auto-revue des sources/preuves effectuée ; aucune revue indépendante, acceptation iOS physique, CI GitHub complète ou Intel HVF suspendu revendiquée.

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## Renommage des répertoires créés par le processus

rename, renameat et renameatx_np ordinaires déplacent un répertoire créé vivant et son sous-arbre dans un même domaine initial, avec autorisation des deux parents immédiats. Cible absente avec slash final : admise ; fichier ordinaire : ENOTDIR ; répertoire non vide : ENOTEMPTY ; déplacement vers un descendant : EINVAL. Le même nom autorisé ne change rien. EXCL donne EEXIST pour une autre cible existante avant type, cycle, domaine ou autorisation. Les erreurs de recherche cible précèdent les points source. Un point/double point source visant le même objet reste UnsupportedService, la sensibilité à la casse étant inconnue. Sources initiales/supprimées, remplacement d’un répertoire initial, domaines distincts, liens, permissions et SECLUDE restent exclus.

Les chaînes de parents définissent l’appartenance : enfants nommés, répertoires supprimés retenus et fichiers orphelins retenus par FD/mapping suivent le nouveau chemin. FD, dup, CWD et double point source suivent le même objet et son nouveau parent. L’ancien répertoire cible vide conserve chemin et parent ; ses enfants ordinaires donnent ENOENT, ses points/CWD restent attachés. Ses orphelins ne suivent pas un nouveau déplacement du remplaçant, même à chemins identiques.

Octets, identité, métadonnées, droits d’écriture, curseurs, flags FD et mappings des fichiers enfants restent inchangés. Source et parents perdent leurs observations complètes ; l’autorité héritée reste disponible pour les créations et renommages admis. Aucun nouvel inode, entrée ou FD. Tous les chemins vivants/retenus et leurs clés sont préparés avant publication, sous les limites chemin/NUL de 1024 octets et 16 MiB. Seule une cible sans FD/CWD/descendant retenu finance le crédit, récupéré une fois. Refus : aucun changement aux noms, parents, observations ou mappings. Un mapping seul retient aussi les coûts des parents supprimés.

Le programme original SDK-free `renamed-directory` compare macOS natif et cinq invités via C++/C/CLI/Python ; 4K/16K couvre identité réutilisée, rollback, capacité exacte, longs descendants, crédits, récupération des parents et épuisement entrée/FD/inode.


### 2026-10-06

Release final : 1,175 inscriptions Darwin, 731 réussies, 444 skips de backend indisponible, zéro échec ; 111 cas ARM64 HVF obligatoires exécutés. Ciblé32/44 (12 skips,22 nouveaux4K/16K), frontières finales4/4. C/CLI165/165 et rapports32/32 sans skip ; Python cinq profils21.759s, natif27/27, sonde indépendante79. La revue indépendante a confirmé les corrections du séparateur racine du programme et de la propriété FS pour le point source du même objet. Les exit124 initiaux et deux attentes anciennes en échec, leurs sources/binaires, restent conservés ; la qualification finale complète passe. Comptages chevauchants et délais inchangés. Les anciens timeouts HVF/natifs restent inexpliqués, sans preuve de stabilité. Intel HVF Actions suspendu ; iOS physique et GitHub CI complet restent distincts.

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### Vérification des échanges de répertoires et mixtes, 2026-10-07

Le contrôle Release Darwin rapproche 1 219 inscriptions : 763 réussites, 456 tests ignorés faute de backend et aucun échec. Les 114 cas ARM64 HVF obligatoires ont été exécutés. Le contrôle ciblé réussit 32/44 cas avec 12 indisponibles, dont les 24 nouveaux cas directs 4K/16K ; le composant fichiers réussit 317/317. Les programmes natifs originaux réussissent 28/28 et une sonde indépendante d’appels bruts consigne 32 observations réussies sur ce système macOS insensible à la casse. Les comptes se recoupent ; les délais invités restent inchangés.

Une revue indépendante du plan et du code a vérifié la transaction bidirectionnelle, les charges des fichiers initiaux, les deux sous-arbres conservés, les orphelins retenus seulement par mapping et les remboursements exacts. Le premier test rouge omettait la déclaration explicite d’échange de la racine et est exclu de l’acceptation ; le contrôle corrigé échouait dans les six cas sur l’ancien refus des répertoires. Deux assertions initiales de fichiers mixtes éliminaient à tort les métadonnées configurées ; elles comparent désormais le record entier avec seul ctime modifié. Une table locale de pointeurs constants introduisait des rebases ARM64 et six refus de chargement. Quatre assertions scalaires l’ont remplacée : le programme corrigé n’a pas de rebases classiques et le chargeur conserve sa frontière de refus des fixups non pris en charge.

La première exécution ciblée corrigée conserve trois expirations HVF de cinq secondes. Les contrôles individuels et sur trois profils, puis le contrôle complet, réussissent ; la cause reste inconnue et cela ne prouve pas la stabilité des délais. Sources, binaires, échecs et contrôles initiaux restent conservés. Déplacements de répertoires initiaux, domaines initiaux distincts, déclarations de permissions/montages/casse, dépendances dynamiques et runtime des frameworks restent inachevés. iOS physique, Intel HVF suspendu et l’ensemble de GitHub CI constituent des frontières d’acceptation séparées.

`build-hvf-arm64/directory-swap-research/`: `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/`, `hvf-controls.json`.

Les binaires liés finaux ont repassé le contrôle Darwin complet. L’intégration figée de dev 0a9a1d28d consigne 4 515 cas sur 20 composants partagés : 4 489 réussites, six cas Z3 facultatifs et 20 cas du corpus Windows EH indisponibles ignorés, aucun échec. C/CLI 170/170 et rapports 32/32 sont inclus. Python couvre les cinq profils en 30,780s. Les 12 variantes natives mobiles architecture/fixup correspondent à 4 532 observations des originaux ; session unique et métadonnées Swift complètes concordent 12/12. Le générateur de témoins Swift reproduit aussi son catalogue avec le SDK/compilateur consigné après une correction d’indentation de commentaire. Cette acceptation locale Release LLVM 23/Apple Clang 17 ne valide ni dev ultérieur ni Linux Clang 18.


## Déplacement ordinaire des sous-arbres initiaux déclarés

Le booléen strict `"movable": true` sur un répertoire initial explicite autre que la racine autorise son rename ordinaire et déclare tout son sous-arbre initial comme des répertoires ordinaires sans montage ni alias de nom. `DarwinFileOptions::MovableDirectories` est ajouté après les membres C++ existants. Le parent immédiat doit être mutable. Absence/false laisse le comportement inconnu ; les autres types JSON sont refusés. Les descendants gardent leurs propres droits mutable/removable/movable et d’écriture. Cette déclaration ne permet ni SWAP de répertoire initial, ni permissions générales, ni montages. Les flags connus, modes spéciaux de répertoire, fichiers ordinaires à liens multiples et alias inode des stat/instantanés sont refusés. Tout domaine connecté par ces déclarations doit avoir au plus un numéro de périphérique connu, y compris les sous-arbres frères et fichiers sous un ancêtre sans stat. L’égalité des numéros ne connecte pas un autre domaine.

Les objets gardent noms, parents, stat/instantanés et droits d’origine. Les anciens chemins d’entrée ne recréent pas les noms déplacés ou supprimés. Descendants non ouverts, FD/dup/CWD, mappings et descendants supprimés suivent leurs objets. Les descendants inchangés gardent stat, instantanés, cookies et SEEK_END ; la racine déplacée et les parents modifiés perdent leurs observations complètes. Réutiliser un nom ne transmet aucune observation ni autorisation. L’entrée initiale décrit un appel synchrone, sans API de remplacement à chaud. La capacité SWAP reste distincte : déclaration swap_rename directe des objets initiaux, copie du parent à mkdir, aucune modification au déplacement. Les deux parents SWAP doivent avoir cette capacité.

Remplacer une cible initiale vide exige removable séparément. Avant publication, toutes les longueurs sont limitées à 1023 octets et le budget partagé à 16 MiB. Une référence movable réserve définitivement son chemin original plus NUL, sans entrée supplémentaire. Chemins, références, instantanés et entrées des répertoires initiaux restent réservés après suppression. PathCharge dynamique commence à zéro puis facture une fois le chemin actuel de chaque membre lié ou conservé. Seule la charge dynamique existante d’une cible immédiatement libérable fournit un crédit ; FD/CWD/enfants/mappings orphelins conservés n’en fournissent pas. La récupération rembourse une fois. Les ancêtres initiaux implicites ne s’ajoutent pas aux 256 entrées ; la récupération des fichiers ordinaires initiaux reste inchangée.

Exemple :

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

initial-directory-move, sans SDK, contrôle déplacement, curseurs partagés/indépendants, flags FD, CWD, mapping privé, remplacement et restauration des noms. Les profils guest locaux et l’iOS physique ont des validations distinctes.

La limite concernant le SWAP initial vise les objets racines source et cible de l’appel. L’échange d’ancêtres créés peut transporter des descendants initiaux déjà déplacés, tout en conservant leur état et leurs coûts dynamiques. Les restrictions initiales précédentes s’appliquent en l’absence des déclarations requises.

Après la revue indépendante finale sur macOS ARM64, le contrôle Darwin compte 1 264 tests : 796 réussites, 468 ignorés pour backend indisponible, aucun échec. Les 117 charges ARM64 HVF obligatoires ont été exécutées. Le sous-ensemble fichiers passe 342/342, dont 22 nouveaux cas 4K/16K et trois contrôles d’admission. CreationPolicy conserve le Device/GID du parent déplacé, sans confondre réutilisation du nom, suite d’inodes et umask courant. À la limite exacte de 16 MiB, 16 échanges aller-retour ne cumulent pas les frais ; le retour libère seulement les six octets réellement économisés. C/CLI public : 175/175 ; rapports : 33/33 ; noyau natif : 29/29 ; sonde originale indépendante : 19 observations réussies. Les cinq configurations Python passent en 27.865 secondes ; API pure 71, dérive SDK et runners 49 passent aussi. Les comptes se recoupent. Sources, binaires, tentatives et résultats sont conservés sous build-hvf-arm64/initial-directory-move/ et liés au commit. iOS physique, Intel HVF suspendu, SWAP de racines initiales, permissions/montages/casse, EOF partagé, Mach/threads/dyld et frameworks restent des travaux distincts.

## Échange atomique des racines initiales déclarées

Le Boolean strict exchangeable:true (C++ DarwinFileOptions::ExchangeableDirectories, ajouté en fin d’agrégat) autorise seulement une racine initiale explicite non racine comme opérande RENAME_SWAP. Son parent initial direct doit être mutable. Absence/false restent exclus ; les autres types sont invalides. Il partage avec movable la déclaration ordinaire sans montage et à noms uniques ainsi que les contrôles flags/modes spéciaux/alias/liens physiques/appareils du composant entier. Leur union ne détermine que la topologie. Chaque référence réserve chemin original+NUL, même si les deux visent la même racine, sans nouvelle entrée. Des appareils égaux ne joignent pas les domaines.

Une source initiale ordinaire/EXCL exige toujours movable ; une cible initiale de remplacement ordinaire exige removable. exchangeable n’accorde ni ces droits, ni mutable aux descendants, ni écriture, ni permissions/montages généraux. Pour deux objets distincts, les deux parents réels doivent être mutables et chacun soutenir swap_rename. Un SWAP homonyme autorisé avec composant ordinaire vérifie parent/appareil puis ne change rien, sans première charge ni capacité d’échange distinct. Dot/casse du même objet restent inconnus ; l’ordre cible absente/dot demeure.

Deux racines initiales non vides, initiale/créée et répertoire/fichier s’échangent dans les deux sens. Les deux arbres liés et conservés sont intégralement prévalidés, puis tous les noms retirés avant publication. Les racines restent liées : aucun crédit de remplacement/contenu ni nouveau FD/inode/entrée. FD/dup/CWD/curseurs, parents objets, baux et droits suivent leurs objets ; descendants inchangés gardent stat/instantanés. Anciens objets supprimés et nouveaux homonymes restent distincts. Les chemins dynamiques commencent à zéro, sont chargés une fois puis remplacent l’ancienne charge ; les coûts fixes restent réservés. Un échec de chemin/budget préserve les deux états.

La charge originale sans SDK initial-directory-swap échange empty et data préexistants puis les rétablit, vérifiant descendants, mapping, CWD, curseurs, flags et création après déplacement. L’acceptation f98068c07 précédente est un résultat ordinaire figé distinct ; seul ce contrat étend le SWAP des racines initiales.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```


Cette validation Release sur macOS ARM64 compte 1,303 inscriptions Darwin : 823 réussites, 480 exclusions pour moteur indisponible et aucun échec ; les 120 cas ARM64 HVF obligatoires sont exécutés. Les contrôles fichiers passent 361/361 (16 nouveaux cas 4K/16K et 3 admissions), C/CLI 180/180, analyse des rapports 34/34, programmes noyau originaux 30/30 et sonde indépendante 35 observations. Python couvre cinq configurations en 33.894 secondes ; 71 tests API purs, 49 unités d’inventaire/référence et les contrôles SDK, format, capacités, provenance et documentation passent. Les comptes se recoupent.

À capacité exacte, la même cible et 16 allers-retours conservent objets et charges. Si six octets sont nécessaires mais cinq disponibles, les deux refus conservent les arbres, curseurs et budget de création. Les revues indépendantes du plan et des sources finales sont acceptées. Essais, sources, binaires et résultats sont figés dans `build-hvf-arm64/initial-directory-swap/` et liés au commit. Un rapport exécuté avec chevauchement est exclu puis répété séquentiellement ; les marqueurs de traduction sont synchronisés. Aucun délai ni contrôle négatif n’est assoupli. Permissions, montages/casse non déclarés, mappings partagés/EOF, horloges évolutives, Mach/threads/dyld/frameworks restent incomplets ; iOS physique, Intel HVF suspendu et CI de fusion distante sont des validations distinctes.

## Observations explicites des limites, en lecture seule

`getrlimit(194)` lit `DarwinSystemOptions::ResourceLimits` (`darwin_system.resource_limits`) dans les cinq profils Darwin. Chaque `DarwinResourceLimit`, clé 0..8, contient deux uint64 little-endian aux offsets 0/8, soit 16 octets. `0 <= current <= maximum <= 9223372036854775807`; zéro est explicite, INT64_MAX représente l’infini. Aucune lecture du host ni modification des budgets FD, VM, stockage ou exécution. `setrlimit`, application des limites, signaux et ordonnancement restent à faire.

JSON strict autorise au plus neuf clés uniques et exactement `resource`, `current`, `maximum`, sous forme d’entiers exacts ou de chaînes décimales non signées. Champs/types incorrects, doublons, clés non canoniques et bornes inversées échouent avant chargement. Un tableau vide/absent reste inconnu; les clés de configuration ne sont pas normalisées.

Seul le syscall retient les 32 bits bas du sélecteur et efface `_RLIMIT_POSIX_FLAG=0x1000`. Une ressource invalide renvoie EINVAL avant tout accès; une observation absente reste unsupported avant accès à la sortie. Une sortie entièrement non inscriptible renvoie EFAULT; une paire partiellement inscriptible reste unsupported sans écriture. Une copie complète, non alignée ou entre pages, ne change que 16 octets; les erreurs backend restent des erreurs de transport. La sonde ARM64 native a passé 23 contrôles de valeurs/sélecteurs et quatre contrôles de faute distincts; son préfixe partiel intact ne prouve pas une garantie portable.

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

macOS ARM64 Release : inscrits/réussis/ignorés non exécutés/échecs 1337 / 845 / 492 / 0; les 123 cas HVF requis ont été exécutés. Douze nouveaux cas 4K/16K et oracle SDK; C/CLI 190/190, parser 37/37, charges natives 31/31; Python sur cinq profils en 71.978 secondes, 71 contrôles API et 49 du runner réussis. Dérive SDK, format, capacités, provenance et documentation passent; totaux chevauchants. Sources, binaires et tentatives gelés dans `build-hvf-arm64/resource-limit-observations/` et liés au commit. Le premier build comportait une faute d’enum dans un test; les tentatives de raccordement/filtrage sont conservées, validation corrigée en série sans relâcher délais ou contrôles. Limites effectives, permissions, répertoires après mutation, maps partagées/EOF, horloges, Mach/threads/dyld et frameworks restent incomplets. iOS physique, Intel HVF suspendu et CI de merge distante exigent une validation séparée.

## Observations explicites de consommation en lecture seule

Dans les cinq configurations Darwin, getrusage(117) lit indépendamment DarwinSystemOptions::ResourceUsageSelf / ResourceUsageChildren, optionnels. JSON strict : darwin_system.resource_usage.self / .children. Chaque DarwinResourceUsage exige user_seconds/system_seconds int64, user_microseconds/system_microseconds uint32 inférieurs à 1000000, et exactement quatorze counters int64. Les entiers exacts ou chaînes décimales signées préservent toute la plage ; types/champs/microsecondes/longueurs invalides échouent avant chargement. Le pair absent reste inconnu sans bloquer le pair fourni ; un instantané nul explicite est valide.

Une seule copie complète little-endian de144 octets : timeval à0/16 (secondes8, microsecondes4, padding nul4), counters dès32 par8. Les valeurs/unités Darwin sont conservées, sans conversion Linux KiB de ru_maxrss. Les valeurs fixes ne mesurent pas le host et ne réalisent ni comptabilité, fork/wait, ordonnancement ou application des limites.

Seuls les32 bits bas sélectionnent 0=SELF, -1=CHILDREN ; 0x1000 invalide, aucun retrait de drapeau POSIX. Sélecteur invalide : EINVAL avant mémoire ; observation absente : unsupported avant sortie. Copie entière non alignée/interpage avec gardes intactes ; sortie totalement non inscriptible EFAULT, partielle unsupported avant tout octet ; erreurs backend restent de transport. L’oracle SDK capture chaque sélecteur une fois, sans comparer des SELF successifs variables. Probe natif13 contrôles et4 processus fautifs indépendants, limite inchangée5s. Sur ce host, partial SELF écrit64 octets avant EFAULT ; observation conservée sans garantie générale de préfixe.

0..13: `ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

macOS ARM64 Release inscrit/réussi/skip sans exécution/échec : 1371/867/504/0 ; 126 HVF requis exécutés. File361/361 et C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 réussis. Douze nouveaux cas4K/16K, admission/SDK ; Python cinq configurations 42.865s, API71 et runner/reference49 ; audits SDK drift, format, capacités, provenance et documentation réussis. Nombres chevauchants ; revue indépendante réussie. Preuves gelées dans `build-hvf-arm64/resource-usage-observations/`, liées au commit. L’assertion initiale x64 EINVAL a été corrigée pour préserver RDX, ARM64 efface X1 ; échec et filtre vide conservés. Runtime/délais/contrôles négatifs inchangés. Application des limites, permissions, répertoires après mutation, shared maps/EOF, horloges, Mach/thread/dyld, frameworks restent incomplets ; iOS physique, Intel HVF suspendu et CI de fusion séparés.

Premier gate :866 réussis, un timeout5s du rename iOS ARM64 HVF existant,504 skips. Le même binaire passe ce cas en386ms, puis le gate complet séquentiel passe. Les deux runs sont conservés ; cause inconnue, aucune garantie de latence.


## Identités explicites, groupes et propriétaire cohérent

Credentials optionnel contient RealUID/EffectiveUID/RealGID/EffectiveGID et GroupAccessList indépendamment optionnel. Omission garde quatre getters1000 ; zéro/root explicite valide, IDs0..INT32_MAX. Groupes1..16, premier=EffectiveGID, ordre/doublons conservés ; absence inconnue, jamais host/EGID inféré. darwin_system.credentials exige exactement real_uid/effective_uid/real_gid/effective_gid, groups optionnel. Entiers sans perte et validation centrale rejettent forme/champs/plage/nombre/premier incohérent avant chargement ; profils nonDarwin rejetés.

getuid24/geteuid25/getgid47/getegid43/getgroups79 partagent un propriétaire system. Nouveau fichier régulier utilise UID effectif, device/GID du parent direct ; rename/FD retenus/nom réutilisé gardent l’objet, stat d’entrée inchangé. Root ne donne pas droits écriture/mutation/ACL ; setuid/setgid/setgroups et processus/session absents.

Capacité getgroups=low32 int signé : négatif EINVAL avant tout, inconnu unsupported, zéro connu retourne count sans pointeur, positif court EINVAL avant mémoire ; suffisant copie une fois4*count octets little-endian. 0x1000 est positif, pas de suppression POSIXflag. Guards complets nonalignés/interpages ; entièrement inaccessible EFAULT, partiel unsupported avant tout octet, backend reste transport. Erreur BSD garde x64 RDX/efface ARM64 X1 ; succès efface secondary des deux, rapport conserve arguments bruts.


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

enregistrés/réussis/skips indisponibles/échecs: 1413/897/516/0; ARM64 HVF 129/129. System/SDK72/72, File365/365, Report43/43, C/CLI210/210; guest8/12skip; native33/33; Python5 82.918s; API71, runner/reference49. SDK drift / clang-format22.1.2 / capabilities23 / provenance / docs11+negative3: OK. Independent source review: OK.

21 value +5 native fault,5s; host16groups, positive short capacity: OK. Native partial32byte thenEFAULT: observation only. Counts overlap. `build-hvf-arm64/credential-observations/`.

Premier run8 échecs (5 budgets instructions,3 deadlines5s),12 skips. Scan deux pages du fixture réduit à la fenêtre complète132octets au même franchissement (64 avant, jusqu’à64 données, au moins4 après) ; owner direct vérifie toujours deux pages entières. Budgets/arguments/contrôles négatifs inchangés, sources/binaires/deux runs préservés. Géométrie transport corrigée avant exécution sur revue ; sélection public compare contenus. Droits/ACL, liens, observations directory après mutation, shared maps/EOF, horloges, Mach/thread/dyld/framework restent incomplets ; iOS physique, Intel HVF suspendu, merge CI séparés.

## Table de descripteurs issue des limites déclarées

`DarwinSystemOptions::MaxFilesPerProcess` / `darwin_system.max_files_per_process` déclare un int non négatif optionnel. L’absence reste inconnue ; zéro explicite est valide. `kern.maxfilesperproc` et le MIB `[1,29]` lisent les mêmes quatre octets, indépendamment des limites de ressources. Le parseur entier sans perte et la validation centrale rejettent formes, signes et bornes incorrects avant chargement ; les profils non Darwin refusent cette configuration.

BSD `getdtablesize(89)` exige ce plafond et `ResourceLimits[8].Current`, puis retourne leur minimum. Current reste complet sur 64 bits avant conversion int : `0x100000001` avec cap=64 retourne64 ; l’infini est aussi borné. Maximum, hôte, FD actifs et `DescriptorLimit` ne remplacent pas les observations. Un pair absent reste unsupported même si l’autre vaut zéro. Les six arguments sont ignorés, sans accès mémoire utilisateur, avec la convention BSD carry/registre secondaire existante. Le trap Mach timebase89 reste indépendant.

Les phases sysctl sont conservées. Une écriture réelle avec EUID0 s’arrête unsupported après lecture nom/MIB et contrôle oldlenp, avant observation/sortie ; non-root reçoit EPERM. Un nouveau pointeur avec longueur zéro reste une lecture. Aucune exécution de limites ni autorisation d’écriture n’est déduite. Le workload obligatoire vérifie les deux cap et les numéros syscall bas/hauts en gardant `l` /144 octets ; un refus ultérieur conserve les octets déjà émis. Le fixture scalaire vérifie absence, zéro, Current large et indépendance de DescriptorLimit=3.

```json
{"darwin_system":{"max_files_per_process":64,"resource_limits":[{"resource":8,"current":"4294967297","maximum":"9223372036854775807"}]}}
```

[XNU getdtablesize](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU proc_limitgetcur_nofile](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [XNU MIB constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/sysctl.h).

Vérification: Darwin (enregistrés/réussis/indisponibles/échoués) 1441/913/528/0; ARM64 HVF 132/132; System/SDK 80/80; File 365/365; C/CLI 211/211; ProcessReport 45/45; native 33/33; Python 5 (42.254s); API 71; runner 44 + reference 5. clang-format 22.1.2; capabilities/provenance; docs 286 / locales 10 / negative controls 3.

Historique des essais : le premier contrôle runner a échoué pour les inventaires ARM64 et x86-64, faute d’enregistrement obligatoire de la nouvelle méthode scalaire. L’enregistrement a été ajouté sans affaiblir la règle d’égalité des méthodes. Le premier contrôle de 129 cas obligatoires et les échecs sont conservés ; le contrôle final ARM64 en exige 132.

Les comptes se recoupent. `build-hvf-arm64/descriptor-table-observations/` conserve le baseline réel et les hachages exacts des sources/binaires/journaux ; les preuves antérieures restent immuables. Sonde ARM64 macOS : cinq processus enfants jetables,40 contrôles, délai de cinq secondes chacun ; Current=1048575/cap=245760 retourne245760, les enfants Current=0/1/32/245777 retournent0/1/32/245760. Limites du parent et du système inchangées. iOS physique, Intel HVF suspendu et remote merge CI restent distincts. Droits, observations de répertoire après mutation, shared maps/EOF, horloges progressives, Mach/thread/dyld et frameworks restent incomplets.

## Observations explicites du processus

`DarwinSystemOptions::ProcessGroupID`, `SessionID` et `ProcessTainted` sont des entrées facultatives indépendantes, nommées `process_group_id`, `session_id` et `process_tainted` en JSON. Les ID sont positifs et au plus INT32_MAX ; l’état contaminé accepte uniquement les Boolean JSON `true`/`false`. Une omission reste inconnue ; `false` explicite est un zéro connu. Aucune valeur ne vient de l’hôte, du PID1000, des identifiants de sécurité ou d’une autre observation.

`getpgrp(81)` brut lit le groupe ; `getpgid(151)` et `getsid(310)` utilisent les32 bits bas signés de `pid_t`, avec zéro ou le PID1000 courant fixe pour soi-même. `0xffffffff000003e8` désigne encore soi-même. Un PID bas négatif renvoie ESRCH3 avant la recherche d’observation, confirmé par des sondes natives en lecture seule et l’allocation/recherche des processus XNU. Une cible positive inconnue, dont `0x1000`, s’arrête avec UnsupportedService sans inventer ESRCH ni appliquer un masque de drapeaux. Une observation sélectionnée absente arrête aussi l’exécution comme non prise en charge. `getpgrp` et `issetugid(327)` ignorent tous les arguments ; les quatre requêtes scalaires n’accèdent pas à la mémoire invitée. Les règles BSD carry et du deuxième registre de retour sont conservées.

`process_tainted` fournit l’observation fixe `P_SUGID` indépendamment de l’égalité des ID réels/effectifs. Il ne change ni EUID, ni propriétaire de fichier, ni autorité d’écriture sysctl, habilitations, rôle de chef ou état du terminal. `setpgid`, `setsid` et la mutation des identifiants restent non pris en charge. La charge originale en lecture seule `process-observations` capture son propre PID et vérifie les bits hauts, erreurs négatives et retours ; `virtual-process-observations` émet les octets configurés groupe/session/contamination. Les cas du modèle pour les autres processus, valeurs absentes et écritures sont exclus de l’inventaire natif. Les références macOS ne valident pas un appareil iOS physique.

```json
{"darwin_system":{"process_group_id":7,"session_id":16909060,"process_tainted":false}}
```

[XNU process queries](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c), [XNU service numbers](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/syscalls.master), [XNU PID allocation](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_fork.c), [XNU PID lookup](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_proc.c).


## Tampon de connexion de session explicite

`DarwinSystemOptions::LoginNameBytes` déclare indépendamment les 255 octets bruts de la session (`MAXLOGNAME`). JSON `login_name_hex` exige exactement 510 chiffres hexadécimaux ASCII, sans distinction de casse. Les NUL internes et les octets non nuls après un terminateur restent valides. Une omission est inconnue ; un tampon entièrement nul est explicite. Aucun remplissage de nom court, aucune déduction depuis hôte, identifiants, groupe, session ou état de contamination. Cette observation ne confère aucun droit de connexion ou de fichier.

`getlogin(49)` utilise les 32 bits bas non signés de longueur (`u_int`) et copie exactement min(length,255) octets, sans décodage, ajout de NUL ou taille requise. Une longueur nulle réussit sans observation ni accès mémoire même avec un pointeur invalide ; `0xffffffff00000000` sélectionne zéro. Une demande non nulle exige le tampon complet avant de vérifier la destination. Une destination entièrement non inscriptible renvoie EFAULT14 ; une plage partielle est refusée avant copie. Les erreurs de précontrôle ne publient rien. La couche de copie transmet les erreurs du backend sans garantie générale de rollback. Les règles BSD carry et second registre, dont RDX x64 original en erreur, sont conservées.

`setlogin(50)` reste non pris en charge même avec root explicite ou un tampon nul. Le programme original en lecture seule `login-buffer` vérifie longueur complète, préfixes, octets voisins, pointeurs de longueur nulle et EFAULT ; `virtual-login-buffer` émet les 255 octets déclarés. Les tests de valeurs manquantes et de setters sont uniquement modélisés. La référence macOS ARM64 ne valide ni iOS physique ni Intel natif. Cet exemple déclare explicitement 255 zéros, sans déduire un nom vide.

Le vérificateur invité initialise les 265 octets de sortie avant chaque copie et contrôle trois plages croissantes disjointes : [0,3), [3,3+n), [3+n,265), où n reste la même longueur de copie plafonnée. Les première et dernière plages contrôlent chaque octet de garde ; celle du milieu compare chaque octet copié à la capture initiale. Les 12 longueurs complètes, 21 appels de longueur nulle, 42 appels EFAULT, contrôles carry/registre secondaire et chemins bruts/virtuels restent couverts avec les limites existantes. Le travail du vérificateur diminue en conservant la couverture et l’ordre du premier échec ; les performances de production demandent des mesures distinctes.

```json
{"darwin_system":{"login_name_hex":"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}}
```

[XNU getlogin / MAXLOGNAME](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c#L1561).


## Priorité explicite du processus courant

`DarwinSystemOptions::ProcessNice` / JSON `darwin_system.nice` déclare une valeur nice signée fixe et indépendante de -20 à 20. Absente signifie inconnue; zéro et -1 explicites sont connus. Le parseur entier sans perte accepte les nombres entiers et chaînes décimales entières; types incorrects, fractions, chaînes exponentielles, espaces et dépassements sont rejetés avant le chargement. Le JSON numérique exactement entier reste valide. Les profils non Darwin rejettent ce champ. Ni hôte, identifiants, groupe/session, contamination, connexion, ressources ni CPU ne le fournissent; ordonnancement, permissions et budgets restent inchangés.

`getpriority(100)` brut utilise les32 bits bas du sélecteur int et de la cible `id_t` non signée. Une cible supérieure à INT32_MAX donne EINVAL22 en premier. Sélecteurs inconnus (dont GPU5 et0x1000) et thread3 avec cible basse non nulle donnent EINVAL avant les observations. `PRIO_PROCESS`0 accepte zéro ou PID1000 courant et exige ensuite nice. Les autres PID positifs restent non pris en charge sans ESRCH inventé. Groupe1, utilisateur2, thread3/cible0 et extensions4,6,7,8 restent inconnus même avec observations connexes; une cible thread dont seuls les bits hauts sont non nuls ne donne pas EINVAL. Le résultat étend le signe à64 bits: -1 est UINT64_MAX avec succès et carry effacé. Aucune mémoire invitée, arguments inutilisés ignorés; BSD conserve RDX x64 en erreur et le remet à zéro en succès, X1 ARM64 est nul dans les deux cas.

`setpriority(96)` reste non pris en charge même avec root/nice explicites. Le programme original en lecture seule `process-priority` vérifie les arguments propres, erreurs certaines et transitions succès/erreur/succès; `virtual-process-priority` émet les huit octets signés déclarés. Les cibles tierces, agrégats, absences et setters restent propres au modèle. La nouvelle sonde ARM64 macOS a passé191 contrôles avec nice0; cet échantillon non négatif ne prouve pas l’extension négative matérielle, couverte par les déclarations signées de la version épinglée et des bornes indépendantes du modèle. iOS physique et Intel natif restent séparés.

```json
{"darwin_system":{"nice":-1}}
```

[XNU getpriority](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_resource.c), [XNU signed INT entry](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/dev/arm/systemcalls.c).


## Ouverture sans suivi de liens et contrôle du répertoire

Les entrées ordinaires et nocancel `open` / `openat` acceptent O_NOFOLLOW=0x100 ou O_NOFOLLOW_ANY=0x20000000 dans les32 bits bas non signés. Ces options de recherche sont absentes de F_GETFL et ne changent ni accès, ajout, troncature, création ni CLOEXEC propre au FD. Leur combinaison donne EINVAL22 après admission de la capacité FD, avant lecture du chemin complet ; une table pleine donne d’abord EMFILE24. Les options inconnues restent non prises en charge.

Hors AT_FDCWD, `openat` lit exactement le premier octet avant le mode d’accès et la capacité FD. Un octet inaccessible donne EFAULT14. Un préfixe relatif, même NUL, vérifie d’abord l’objet répertoire détenu : FD inconnu EBADF9, fichier ordinaire ENOTDIR20, type vnode de flux inconnu non pris en charge. `/` ignore dirfd ; la séquence open existante lit ensuite le chemin complet. `open` ordinaire et AT_FDCWD omettent ce contrôle, les autres services nameiat vérifient, après les options et tailles, le premier octet et le dirfd relatif avant la chaîne complète. Un refus ne consomme ni FD ni nouvel inode ; les erreurs de transport se propagent sans mutation de l’espace de noms.

Le programme original `file-access` utilise un FD de répertoire relatif pour NOFOLLOW_ANY afin d’éviter les alias natifs `/var` et `/tmp`. Les tests directs distinguent premier octet et suite, barre/NUL, limites utilisateur/page, table pleine et répertoires supprimés conservés. La sonde brute ARM64 macOS a réussi30 cas sous le délai inchangé de cinq secondes ; sa dernière ligne de chemin absolu avec table pleine utilise réellement un FD de répertoire valide malgré son ancien libellé. iOS physique et Intel natif restent à valider séparément.

[XNU open1at / open1](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU vn_open_auth](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_vnops.c), [XNU open flags / FMASK](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/fcntl.h).

## Liens symboliques initiaux fixes

Les noms initiaux ci-dessous sont protégés par défaut; le dernier paragraphe décrit les droits explicites de mutation. `DarwinFileOptions::SymbolicLinks` / `darwin_files.symbolic_links` déclarent un `path` absolu canonique, un `target_hex` brut et les `metadata` facultatives du lien. La cible contient1..1023 octets nonNUL, conserve nonUTF-8, barres répétées et points, et peut manquer. `files` reste requis, même vide. Collisions et descendants déclarés sous un lien sont refusés. Chemin/NUL/cible partagent256 entrées/16 MiB. S_IFLNK, size=longueur, DT_LNK=10 et inode cohérent sont requis; CWD configuré doit être un vrai répertoire.

Les liens sont développés avant les points: cible relative depuis le parent réel, cible absolue depuis la racine invitée. Chaque reprise réanalyse les barres finales sans réutiliser celles consommées.32 développements sont permis, le33e donneELOOP62; cible+suffixe+NUL dépassant1024 octets donneENAMETOOLONG63. FD/CWD/F_GETPATH/mmap gardent l’objet résolu.

stat64/open/access/truncate/chdir suivent le lien final; lstat64/readlink le gardent. O_NOFOLLOW donneELOOP, avecO_DIRECTORY ENOTDIR20 passe avant. O_NOFOLLOW_ANY refuse tout développement nécessaire. O_CREAT|O_EXCL donneEEXIST17 pour un lien final existant, même cassé/cyclique. AT0x20/0x800 gardent le lien final;0x800 refuse aussi les développements intermédiaires/barres finales. Combinaison permise. AT_FDONLY ignore le chemin après validation des options.

readlink(58) utilise count signé bas32, readlinkat(473) le size_t entier; retourint. Au-delà deINT32_MAX: EINVAL22 avant chemin/FD. Copie min(count,longueur), sansNUL; seul ce préfixe est vérifié. Taille0 vérifie chemin/type puis ignore la sortie. Non-lienEINVAL22, aucun octet accessibleEFAULT14; préfixe partiellement accessible: arrêt avant copie. Les erreurs de transport/budget mémoire se propagent.

Les noms des liens et leurs octets cibles restent fixes. MutableDirectories ne peut être la racine ni un ancêtre par composante d’un lien fixe ; /work ne contient pas /workspace/link. Des domaines mutables séparés peuvent contenir les cibles créées, déplacées, supprimées ou remplacées pendant l’exécution. Les contrôles de parent, montage, alias, flags, support SWAP et création restent applicables ; les nouveaux inodes doivent dépasser tous ceux des métadonnées/instantanés, liens protégés inclus. WritableFiles/MutationPolicies fixes peuvent modifier le fichier cible. Unlink/rename d’un lien retenu s’arrêtent avant effet. La création pendant l’exécution est décrite ci-dessous ; liens durs, ACL et catalogues initiaux mutables restent exclus. La sonde ARM64 macOS a passé189 observations/115 tampons complets sous la limite initiale5s; pas une preuve d’iOS physique/Intel HVF/OS complet.

Les 60 contrôles ARM64 macOS DELETE/RENAME supplémentaires conservent les tampons stat complets, l’espace de noms avant/après et les identités FD/CWD sous la limite initiale de5s. Les barres finales peuvent développer un lien fixe et modifier sa cible ; NOFOLLOW_ANY refuse le développement requis avec ELOOP. symbolic-link-mutations, sans SDK, vérifie création, cible absente, déplacement/suppression/remplacement, parents CWD conservés et les10 octets originaux du fichier avant fermeture des FD, puis les10 octets du mapping après fermeture. Ces observations ne prouvent pas iOS physique ou Intel natif.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"symbolic_links":[{"path":"/link","target_hex":"64617461"}],"working_directory":"/"}}
```

[XNU namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c), [XNU readlink / AT](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU open authorization](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_subr.c).

## Création de liens symboliques pendant l’exécution

Les appels bruts `symlink(57)` et `symlinkat(474)` créent des liens locaux au processus dans les répertoires mutables autorisés, avec un retour int. Le dirfd utilise ses32 bits faibles ; une destination absolue ignore le FD. La cible est importée avant la destination, jusqu’au premier NUL :0..1023 octets opaques, y compris cible vide, non-UTF8, points et séparateurs répétés.1024 octets sans NUL donnent ENAMETOOLONG63 ; une faute antérieure donne EFAULT14. Les cibles JSON initiales restent limitées à1..1023 octets.

La table courante détient le nom réel, le parent et les octets. Un terminal existant donne EEXIST17. Les séparateurs finaux consommés peuvent suivre un lien pendant et créer à son nom cible, sans changer le lien initial. Développer une cible vide donne ENOENT2. Readlink d’une cible vide renvoie0 sans toucher la sortie, même avec une capacité positive, après validation du count/chemin/type.

Nom/NUL et cible sont comptés une seule fois dans les256 entrées/16 MiB communs. Un refus ne change ni nœud, ni parent, ni FD, ni inode de création de fichier. Les métadonnées complètes du nouveau lien restent inconnues : pas d’héritage de CreationPolicy de fichier ni d’une ancienne observation au nom réutilisé. Le stat/instantané parent devient inconnu après création. FD/CWD/mappings conservent leurs objets après suppression/remplacement de cible. Rmdir et remplacement de répertoire détectent les enfants liens. Un déplacement/SWAP contenant un lien initial protégé sur l’un des côtés s’arrête avant effets. remplacements lien/répertoire, liens durs, ACL et catalogues initiaux mutables restent exclus ; un alias ne transfère aucune autorisation du parent réel.

Les150 observations ARM64 macOS conservent quatre erreurs d’observateur ; dix contrôles séparés vérifient la vraie cible et les limites du lien vide. Le programme sans SDK `symbolic-link-creation` vérifie les deux entrées, octets/tampons, parents, remplacement et dix octets complets des anciens FD/mappings. Cela ne prouve pas iOS physique, Intel natif ou un OS complet.

[XNU symlink / symlinkat](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU empty-link expansion](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c).

## Suppression des liens symboliques créés à l’exécution

`unlink(10)` et `unlinkat(472)` suppriment un lien créé à l’exécution si son parent réel autorise la modification ; les liens initiaux fixes restent protégés. `AT_SYMLINK_NOFOLLOW_ANY` seul conserve le lien final et permet sa suppression, même avec une cible absente, cyclique ou vide. Une expansion intermédiaire ou due au séparateur final donne ELOOP62. Sans ce drapeau, supprimer `a/` dans `a → b → target` retire seulement `b`, en conservant `a` et la cible finale. L’ordre existant des erreurs de drapeaux, chemin et dirfd reste applicable.

Le succès rembourse une seule entrée dynamique et son chemin/NUL/cible courants ; seules les observations stat/énumération complètes du parent réel sont invalidées. Aucun FD libre ni inode de création n’est requis. Données, politique de modification, descriptions ouvertes, curseurs partagés, CWD et mappings gardent leurs objets. Réutiliser le nom ne restaure pas l’ancien lien ; un refus ne modifie ni état ni budget. Les métadonnées complètes des liens créés à l’exécution restent inconnues. Les remplacements lien/répertoire et le déplacement/SWAP de sous-arbres contenant des liens initiaux protégés restent exclus.

Les 40 observations ARM64 macOS indépendantes enregistrent, avec le délai inchangé de cinq secondes, 28 suppressions, 12 erreurs, 17 ENOENT après une seconde suppression et la conservation des FD/CWD/mappings privés. Elles ne valident pas les invités, iOS physique ou Intel natif. `symbolic-link-unlink` et les cas API publics vérifient séparément ces limites.

## Renommer les liens symboliques créés à l’exécution

`rename(128)`, `renameat(465)` et `renameatx_np(488)` prennent en charge les déplacements ordinaires et `RENAME_EXCL=4` : lien vers nom libre, lien/lien, lien/fichier et fichier/lien. Les deux parents réels doivent autoriser la modification dans un domaine de montage établi ; les liens initiaux restent immuables. Le résolveur partagé choisit les nœuds réels. Les octets de cible vide, absente, cyclique ou non UTF-8 restent inchangés ; une cible relative est résolue depuis le nouveau parent. `RENAME_NOFOLLOW_ANY=16` seul conserve le lien terminal, tandis qu’une expansion intermédiaire nécessaire renvoie ELOOP62. Une destination EXCL existante distincte renvoie EEXIST17 ; EXCL sur le même objet reste exclu sans contrat de sensibilité à la casse.

La nouvelle charge chemin/NUL, limitée à1024 octets NUL inclus, est réservée avant publication. Remplacer un lien d’exécution rembourse une seule fois sa charge actuelle complète chemin/NUL/cible, indépendamment des FD ou mappings du référent. Le contenu/chemin dynamique d’un fichier remplacé reste facturé jusqu’à libération de toutes les descriptions et leases de mapping ; seul un fichier immédiatement récupérable fournit un crédit de réservation. Aucun nouvel élément, FD ou inode de création de fichier n’est nécessaire. Un refus conserve les deux nœuds ; le succès invalide les observations stat/énumération complètes des parents réels. Les métadonnées complètes du lien restent inconnues. remplacements lien/répertoire, liens durs et déplacement/SWAP de sous-arbres avec liens initiaux protégés restent exclus.

Les19 contrôles ARM64 macOS indépendants enregistrent14 succès et5 erreurs EEXIST/ELOOP sous les5 secondes d’origine, avec inode/octets du lien, nouvelle liaison relative et FD/dup/curseurs/CWD/mappings privés conservés. `symbolic-link-rename` sans SDK et les vérifications publiques SDK/CLI sont séparés. Une référence native seule ne prouve pas iOS physique, Intel natif ni une compatibilité OS complète.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Échanger les liens symboliques créés à l’exécution

`renameatx_np(488)` accepte lien/lien, lien/fichier et fichier/lien avec `RENAME_SWAP=2` lorsque les deux parents réels autorisent modification et SWAP dans un domaine de montage établi. Un nom identique réussit sans effet ni autorisation SWAP supplémentaire. Une cible absente renvoie ENOENT2 ; `SWAP|NOFOLLOW_ANY=18` conserve les liens terminaux et refuse l’expansion intermédiaire avec ELOOP62 ; `SWAP|EXCL=6` renvoie EINVAL22 avant lecture des chemins. Les octets restent inchangés et les cibles relatives utilisent leurs nouveaux parents.

Les deux charges chemin/NUL sont réservées avant publication. Les deux objets restent liés, sans crédit de remplacement pour les données ou leases de mapping. Un fichier initial acquiert une charge dynamique au premier échange et la réutilise au retour. Aucun élément, FD ou inode de création n’est consommé. Identité, nlink, descriptions, curseurs, CWD et mappings restent conservés ; les observations parentales complètes deviennent inconnues. Métadonnées complètes des liens, paires répertoire/lien et déplacement/SWAP de sous-arbres contenant des liens initiaux protégés restent exclus. Les 22 contrôles ARM64 macOS enregistrent 14 échanges, deux succès du même objet et six erreurs sous les cinq secondes d’origine. `symbolic-link-rename` et SDK/CLI/Python vérifient échanges et erreurs, sans prouver iOS physique, Intel natif ni OS complet.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Déplacer des arbres contenant des liens d’exécution

Les déplacements ordinaires/EXCL et SWAP de répertoires incluent les liens créés à l’exécution, y compris l’échange répertoire/fichier. La transaction vérifie tous les noms de répertoires, fichiers et liens et la limite1024 octets NUL inclus avant d’extraire les trois tables puis publier les noms. Les octets cibles restent facturés et inchangés ; seule la charge chemin/NUL actuelle change. SWAP ne fournit aucun crédit de remplacement et ne consomme ni nouvel élément, FD ni inode.

Les liens conservent leurs parents réels déplacés ; les cibles relatives utilisent les nouveaux chemins. Descriptions, curseurs, CWD, nœuds retirés et leases de mapping gardent leurs objets. Liens initiaux protégés et leurs arbres, paires racines répertoire/lien, métadonnées complètes des liens, liens durs et ACL restent exclus.25 contrôles ARM64 macOS enregistrent cinq déplacements, dix échanges, deux succès sans effet et huit refus sans changement de noms sous les cinq secondes d’origine. Les contrôles entre parents vérifient octets intacts et nouvelle résolution relative ; `symbolic-link-rename` couvre C++/SDK/CLI/Python sans prouver iOS physique, Intel natif ni OS complet.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Métadonnées des liens et répertoires créés

L’option `creation_policy.namespace_policy` (C++ `DarwinFileCreationPolicy::Namespace`) ajoute aux cinq champs obligatoires un objet strict contenant `symbolic_link_allocation_unit`, `directory_entry_size`, `directory_blocks`. L’unité du lien est une puissance de deux entre512 et16 MiB, la taille d’entrée positive jusqu’à16 MiB, les blocs uint64 jusqu’àINT64_MAX; les chaînes décimales restent exactes. Métadonnées parentales, umask explicite et nouveaux inodes restent requis. Sans extension, les anciennes limites de métadonnées inconnues et la consommation d’inode réservée aux fichiers ordinaires restent le défaut.

Les insertions réussies de fichiers,symlink,mkdir partagent alors une séquence d’inodes, définitivement épuisée àUINT64_MAX; erreurs et noms existants ne consomment rien. Device/GID viennent du parent réel, UID de l’identité effective invitée. Mode lien:S_IFLNK avec `0777 & ~umask`, nlink1, taille des octets bruts incluant cible vide/nonUTF-8, blocs512octets arrondis à l’unité déclarée. Mode répertoire:S_IFDIR avec `mode & 0777 & ~umask`, nlink2 plus tous les noms directs liés, size=nlink fois la taille d’entrée et blocks fixes, même pour un répertoire vide retiré mais retenu. Ce contrat virtuel explicite ne déduit aucune règle APFS.

Les temps initiaux utilisent creation_time. Les changements de noms enfants mettent à jour mtime/ctime du parent créé; les déplacements directs seulement ctime de l’objet avec mutation_time. Déplacer un ancêtre préserve les descendants. Les enregistrements complets suivent l’objet à travers dup/CWD/remplacement/SWAP/retrait/réutilisation. L’extension de création seule ne conserve pas le stat complet des parents initiaux ni les snapshots fixes; les politiques indépendantes ci-dessous fournissent stat et énumération courante. ACL, mutations de liens initiaux sans droit individuel et transactions générales racines répertoire/lien restent absents. `created-namespace-metadata` vérifie les observations natives communes; `virtual-created-namespace-metadata` compare144octets constants complets viaC++/SDK/CLI/Python. Validation ciblée:11 modèles/admissions,1JSON strict,43 workloads natifs avec5secondes inchangées,8 invités exécutables（12 ignorés faute de backend,3HVF requis exécutés）et10 cas publics réussis. Échec de table de pointeurs et correction statiqueARM64 conservés; iOS physique et Intel natif non validés.

```json
{"namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}}
```

## Énumération virtuelle après modification des noms

Le champ facultatif `directories[].enumeration_policy` active une vue actuelle via `getdirentries 64`; C++ utilise `DarwinFileOptions::DirectoryEnumerationPolicies` et `DarwinDirectoryEnumerationPolicy`. L'objet strict exige exactement `minimum_buffer_size`, `initial_minimum_buffer_size` et `seek_offset`: le premier est positif, les deux minima ne dépassent pas 128 MiB et seek_offset est un uint 64 sans perte. Le répertoire initial doit posséder des métadonnées avec inode non nul; `contents` immuable est incompatible. Chaque référence compte une fois le chemin+NUL. mkdir hérite de la politique du parent réel; les descendants existants conservent leurs déclarations. Cette politique est indépendante du droit de modification.

L'ordre virtuel est `.`, `..`, puis les noms directement liés triés par octets non signés. Les inodes proviennent des objets observés ou créés et `..` suit le parent réel conservé. Une identité enfant/parent inconnue ou nulle arrête avant toute sortie. Seul l'inode peut subsister après invalidation du stat initial complet. Les cookies sont des ordinaux locaux depuis 1, d_seekoff une constante déclarée. dup partage le curseur, open reste indépendant. Une modification validée ou un déplacement direct impose un retour à zéro; refus et opérations identiques sans effet le préservent. Déplacer un ancêtre préserve les curseurs descendants. L'épuisement des versions arrête explicitement. Un répertoire vide supprimé et retenu émet zéro enregistrement, même après réutilisation du nom.

Ce paragraphe décrit la préparation avec la seule politique d’énumération, sans la politique de stat initial ci-dessous. L'encodeur, les enregistrements entiers, minima, plafond de charge, suffixe EOF et effets ordonnés données/curseur/position/drapeaux restent communs. Stat complet du parent initial et instantanés fixes restent invalidés; sans politique, les refus antérieurs restent. Les générations APFS ne sont pas reproduites. La préparation ARM 64 indépendante a enregistré 25 événements/16 vues sous le délai inchangé de 5 secondes. `directory-enumeration-mutations` vérifie les identités natives et objets retenus; `virtual-directory-enumeration` compare 160 octets littéraux par invité, C/CLI et Python. Intel natif, iOS physique, ACL, liens physiques et OS/frameworks complets restent non validés ou non pris en charge.

## Mutation explicite du stat des répertoires initiaux

La déclaration facultative `directories[].mutation_policy` conserve le stat complet d’un répertoire initial admis après une modification de l’espace de noms. C++ utilise `DarwinDirectoryMutationPolicy` et `DarwinFileOptions::DirectoryMutationPolicies`. L’objet strict contient exactement `directory_entry_size` et `mutation_time`. La taille est positive, au plus 16 MiB; le temps utilise des secondes signées sur 64 bits sans perte et des nanosecondes dans [0,1000000000). Le répertoire doit avoir ses propres metadata complètes avec un inode non nul. Chaque référence compte une fois le chemin et NUL; la taille projetée n’alloue aucun octet de fichier. Cette politique n’accorde aucune autorité de modification ou de permission et ne nécessite ni politique de création ni d’énumération.

Le relevé complet reste intact jusqu’au premier changement réellement validé, qui copie ses champs scalaires dans l’objet sans allocation. Changer les noms enfants fixe nlink à deux plus tous les noms directement liés, quel que soit leur type, size à nlink multiplié par directory_entry_size et mtime/ctime à mutation_time. Déplacement direct, SWAP ou suppression ne modifient que ctime; déplacer un ancêtre préserve les descendants. Refus et opérations sans effet sur le même objet ne changent rien. Device, inode, mode, propriétaire, blocks, taille de bloc, flags, generation, atime et birthtime gardent leurs valeurs observées. Ce sont des règles virtuelles déclarées, sans déduction des allocations, liens ou horloges APFS.

Le relevé et la politique suivent l’objet d’origine malgré dup, FD conservés, CWD, remplacement, suppression et réutilisation du nom. Un nouvel objet mkdir utilise la politique de création séparée, si fournie, sans hériter du stat initial du parent ou de l’ancien objet homonyme. Les snapshots immuables deviennent encore inconnus après modification; une enumeration_policy indépendante peut fournir des vues courantes. Sans cette politique de stat, les metadata complètes des répertoires initiaux modifiés restent inconnues.

Une préparation native ARM64 indépendante a conservé 27 vues stat brutes protégées et 15 opérations dans les cinq secondes inchangées, vérifiant l’ABI SDK de 144 octets et l’identité conservée sans généraliser temps ou allocation. Le test original `initial-directory-metadata` vérifie les observations natives communes; `virtual-initial-directory-metadata` compare un relevé littéral complet de 144 octets via guest, C/CLI et Python. Les modèles couvrent aussi première suppression, absence de droit de modification, omission de création et indépendance snapshot/énumération. Intel natif, iOS physique, ACL, liens physiques, mutations de liens initiaux sans droit individuel et OS/frameworks complets restent non validés ou non pris en charge.

```json
{"mutation_policy":{"directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}}}
```

## Mutation explicite des liens symboliques initiaux

`symbolic_links[].mutable:true` et C++ `DarwinFileOptions::MutableSymbolicLinks` autorisent la mutation du nom de l’objet initial, avec un parent réel mutable séparément. Flags connus, bits mode spéciaux, link_count≠1, alias d’identité et appareils parents contradictoires sont refusés. L’omission protège le nom et exclut ces liens des domaines ancêtres mutables. Chaque droit réserve une référence path/NUL fixe, sans entrée ni inode créé. Les cibles initiales restent immuables.

unlink, rename ordinaire/EXCL, SWAP feuille lien/fichier/link et transactions de sous-arbres admis gardent les conditions parent réel, mount et SWAP. Les cibles relatives se résolvent depuis le nouveau parent; FD/dup, CWD et leases de mapping conservent leurs référents. Les coûts initiaux restent réservés après suppression/remplacement; le premier renommage réserve un nom dynamique séparé. Seuls les coûts dynamiques possédés sont remboursés et seuls les liens créés ajoutent des entrées dynamiques. Refus et opérations sans effet ne publient rien.

`symbolic_links[].mutation_policy` utilise `DarwinSymbolicLinkMutationPolicy` et `DarwinFileOptions::SymbolicLinkMutationPolicies`. Son unique champ strict `mutation_time` contient des secondes signed 64-bit sans perte et des nanosecondes [0, 1000000000), avec droit explicite et métadonnées complètes à inode non nul. Le premier déplacement direct/SWAP copie les scalaires sans allocation et modifie seulement ctime. Blocks et tous les autres champs restent observés; déplacements d’ancêtres/refus/opérations sans effet conservent le record. Sans politique, stat complet devient inconnu après déplacement direct, mais inode d’énumération et contradictions connues de périphérique restent disponibles. La référence path/NUL est fixe. Noms réutilisés et nouveaux symlink n’héritent pas de la politique initiale; la création emploie sa namespace policy distincte.

La préparation ARM64 privée conserve 14 vues guarded raw-stat, 11 opérations, l’ABI SDK indépendante de 144 octets, les bornes compile120s/native5s/drain1s/reap1s et le nettoyage. `mutable-initial-links` vérifie identité native, résolution et référents; `virtual-mutable-initial-links` vérifie stat complet dans cinq profils guest, C/CLI et Python. Les modèles couvrent deux tailles de page, SWAP de sous-arbres et budgets/exhaustion entry/inode. Intel et iOS physique ne sont pas validés; hard links, ACL, OS/runtime/framework complet restent absents.

```json
{"mutable":true,"mutation_policy":{"mutation_time":{"seconds":-13,"nanoseconds":456}}}
```

## Transactions racines de répertoire et de lien symbolique

`renameatx_np(RENAME_SWAP)` échange un répertoire réel et un lien dans les deux ordres, y compris des sous-arbres non vides. Le répertoire initial exige `exchangeable`, le lien initial `mutable` et les deux parents réels leurs droits de modification/SWAP dans un mount établi. Les descendants initiaux protégés restent refusés. Avec les droits ordinaires existants, répertoire→lien renvoie ENOTDIR20, l’inverse EISDIR21 et EXCL vers un nom distinct existant EEXIST17. Les deux cycles répertoire/lien descendant donnent EINVAL22 avant effet. L’échange porte sur le lien lui-même : une autoréférence peut résulter d’un succès, puis son suivi renvoie ELOOP62.

La transaction des trois tables vérifie racines, descendants liés ou retenus, chemins complets et mémoire dynamique avant publication. Noms/cibles/références initiaux, contenu et leases de mapping ne fournissent aucun crédit SWAP. La première nouvelle clé réserve un chemin/NUL dynamique indépendant; les répétitions remplacent une fois l’ancien coût. Aucun nouvel entry, FD ou inode de création. L’appartenance suit les parents réels et non l’orthographe réutilisée d’un ancien objet orphelin. Les cibles brutes restent inchangées; résolution relative, FD/dup, curseurs, CWD, référents et mappings conservent leurs contrats. Les racines directes appliquent leurs propres politiques stat à ctime uniquement; les déplacements d’ancêtres conservent les descendants. Sans politique, stat complet reste inconnu mais inode reste disponible pour l’énumération vive; les versions concernées suivent le contrat de remise à zéro. Aucun champ JSON ou droit nouveau.

La préparation ARM64 originale conserve 36 vues stat protégées de 144 octets, 16 raw rename, compile120s/native5s/drain1s/reap1s et récupération/nettoyage confirmés. `directory-link-roots` et `virtual-directory-link-roots` vérifient identité et stat complet constant via cinq guest, C/CLI et Python. Les modèles de deux tailles de page couvrent budgets exacts, dépassements non ouverts, orphelins et épuisement FD/entry/inode. Cette section étend les exclusions précédentes dans ces droits. Intel natif, iOS physique, hard links, ACL et OS/runtime/framework complet restent des travaux distincts.

## Requêtes pathconf fixes du noyau

pathconf(191) / fpathconf(192) couvrent les constantes vnode XNU :15/16/17→1,19/25→0,20/22/23→4096,21→65536,24→255. Ces observations de liens, allocation, I/O et transfert ne permettent ni exécution asynchrone ni autorisation; pages et budgets du catalogue ne déterminent pas ces valeurs.

La résolution complète du chemin avec suivi des liens ou la recherche du FD précède le sélecteur low32. CWD et EFAULT/ENOENT/ENOTDIR/ELOOP sont conservés; FD absent/fermé donne EBADF. Les objets fichier/répertoire retenus restent interrogeables après dup, déplacement, suppression ou réutilisation du nom sans stat complet. Le type natif des flux capturés reste inconnu. Le contrat BSD int/carry/registre secondaire est conservé sans copie de sortie, modification du curseur, métadonnées/énumération ou réservation d’entrée/FD/inode. NAME_MAX, casse et sélecteurs inconnus restent non pris en charge après recherche, sans valeur hôte ni EINVAL supposé.

kernel-pathconf, kernel-pathconf-values et kernel-pathconf-unsupported vérifient le comportement commun,80 octets littéraux indépendants et l’arrêt conservant la sortie précédente via invité/C/CLI/Python. Préparation ARM64 privée :250 requêtes,249 comparaisons SDK,0.262s avec limite5s inchangée. Intel natif/iOS physique/ACL/liens physiques/runtime et frameworks complets restent non validés ou non implémentés.

[XNU vn_pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU fpathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_descrip.c).

`pathconf`: `file/` → ENOTDIR; `alias/`, `alias//` → 1 (selector15); `alias/.`, `alias/child` → ENOTDIR.

## Listes d’attributs communs fixes

getattrlist(220), fgetattrlist(228) et getattrlistat(476) interrogent onze champs communs du catalogue explicite : appareil, type, quatre dates, propriétaire/groupe, mode complet, indicateurs et ID. La validité du stat complet est partagée avec stat64; une observation absente ou invalidée reste inconnue. Type et sélection vide ne demandent pas de stat. NAME de racine/montage, volume, masques propres aux répertoires/fichiers/forks, ACL et options inconnues restent explicitement non pris en charge, même avec un masque retourné; aucune donnée hôte ou étiquette de montage n’est déduite.

Chemin/at importent les24 octets avant la résolution; FD valide d’abord low32 FD et type natif. reserved est ignoré. CWD, FD relatif et liens partagent les erreurs natives avant taille/bitmap. Les champs sont little-endian, alignés sur4, avec st_mode complet et secondes signées. Le record avec masque mesure120 octets, sans masque100. Un petit tampon reçoit le préfixe demandé mais indique toujours la longueur complète. L’accès partiel s’arrête avant cette copie; la taille hors signed-uio donne EINVAL après une requête valide et supportée. Aucun curseur, métadonnée, énumération ou budget entrée/FD/inode ne change; la durée de vie dup/suppression/réemploi des noms est conservée.

Trois modes vérifient comportement commun, octets configurés indépendants et ATTR_CMN_EXTENDED_SECURITY non supporté conservant la sortie via guest/C/CLI/Python. Préparation ARM64 privée :187 requêtes raw et176 comparaisons SDK chemin/FD, limite native5s inchangée. SDK15.5 ne déclare pas getattrlistat; raw476 est séparé. Nouveaux guest/Python :5,000,000us/quantum1024; tests publics existants :10s. Intel natif, iOS physique, faits FS, liens physiques, droits/ACL et runtimes/frameworks complets restent incomplets ou non validés.

```text
ATTR_CMN_RETURNED_ATTRS=0x80000000
FSOPT_NOFOLLOW=1, FSOPT_REPORT_FULLSIZE=4
FSOPT_PACK_INVAL_ATTRS=8 (requires ATTR_CMN_RETURNED_ATTRS)
FSOPT_NOFOLLOW_ANY=0x800
common-attributes
common-attributes-values
common-attributes-unsupported
```

[XNU attrlist](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_attrlist.c), [XNU attr.h](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h).


## Lecture explicite des attributs étendus

getxattr(234), fgetxattr(235), listxattr(240) et flistxattr(241) lisent les observations ordinaires complètes et ordonnées de darwin_files. Chaque fichier, répertoire ou lien peut fournir extended_attributes, tableau strict de {name,bytes_hex}. Une omission reste inconnue ; [] déclare une liste vide connue. Les noms UTF-8 de 1..127 octets acceptent la barre oblique, les valeurs sont opaques, les doublons sont refusés et l’ordre est conservé. Limite : 4096 attributs. Noms, NUL et valeurs entrent dans le budget existant de 16 MiB sans compter deux fois le chemin existant.

Les observations appartiennent aux objets après dup, déplacement, suppression, CWD et réutilisation du nom, indépendamment du stat complet et de l’énumération. Les nouveaux objets restent inconnus. Écriture, troncature réussie et échec ambigu de copie non vide invalident les attributs ; un refus préalable du tampon partiel les conserve. Les lectures préservent curseurs, entrées, métadonnées et budgets des objets/FD/inode.

ABI : FD/options/position low32, size full64, BSD user_ssize_t/carry/secondary. NULL ignore position. Pour une valeur non vide, le chemin nonNULL size0 donne ERANGE, FD size0 interroge la longueur. Seuls UINT32_MAX/UINT64_MAX du get par chemin sont des requêtes historiques ; FD limite à INT32_MAX. Une liste positive courte publie des noms complets avant ERANGE ; une taille négative full64 nonNULL donne ERANGE pour une liste non vide. Aucune liste vide native n’a été vérifiée : ce cas négatif sur une liste vide déclarée reste UnsupportedService. Une sortie inaccessible s’arrête avant copie. NOFOLLOW1 et NOFOLLOW_ANY64 sont indépendants ;8/16 sont refusés avant recherche, FD1/64 avant FD/nom. CREATE2/REPLACE4 sont ignorés ; SHOWCOMPRESSION32 et bits inconnus restent non pris en charge. com.apple.system.*, ResourceFork, FinderInfo, decmpfs, modification/suppression, autorisation/ACL et déduction du système de fichiers sont exclus.

extended-attributes / extended-attributes-values / extended-attributes-unsupported vérifient le programme commun natif, les octets virtuels et l’arrêt inconnu conservant la sortie. Budgets inchangés : guest/Python5,000,000us/quantum1024, API publique10s, natif5s. Préparation privée ARM64 :320 comparaisons raw/SDK avec tous288 octets de garde et carry/secondary identiques. com.apple.provenance automatique est une observation, pas une liste vide par défaut. Intel natif/iOS physique restent non vérifiés ; dyld, Mach IPC, Objective-C/Swift et frameworks complets restent inachevés.

Sources: [XNU syscall ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU xattr calls](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ordinary attributes](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_xattr.c), [XNU xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code/probes; Apple implementation is not copied.

## Noms d’objets bornés

ATTR_CMN_NAME=1 est admis par getattrlist220/fgetattrlist228/getattrlistat476 pour un objet non racine nommé de façon unique dans le catalogue explicite. Le nom terminal est un UTF-8 valide de1..255 octets. Le chemin réel partagé avec F_GETPATH conserve la dernière graphie liée à travers dup, CWD, déplacement, SWAP, suppression et réutilisation; les alias de l’appelant ne la remplacent pas. Nom et type ne demandent pas de stat; les champs stat sélectionnés exigent des observations complètes valides. Étiquettes racine/montage, noms invalides, liens physiques, alias de casse, normalisation et attributs de chemin complet restent inconnus.

attrreference_t occupe8 octets avant les autres champs communs; attr_dataoffset est relatif à la référence, attr_length inclut NUL et le nom terminal est complété à4 octets. Les sorties courtes gardent la longueur totale et les préfixes exacts, même un UTF-8 partiel. attribute-names / attribute-names-values / attribute-names-unsupported vérifient comportement natif, octets indépendants et arrêt sur nom racine avec sortie conservée via guest/C/CLI/Python. ARM64:601 requêtes raw,453 comparaisons SDK de tous les octets protégés,384 préfixes. SDK15.5 ne déclare pas raw476. Native5s, guest/Python5,000,000us/quantum1024 et public10s inchangés. Intel natif, iOS physique et runtimes/frameworks complets restent incomplets ou non validés.

## Attributs de répertoire groupés bornés

getattrlistbulk(461) exige enumeration_policy.bulk_attributes=true explicite ; omission et false ne donnent aucun droit. Ce contrat TYPE virtuel renvoie les enfants directs actuels par ordre d’octets non signés, sans entrées point, avec des ordinaux locaux plutôt que les cookies natifs. Le droit reste attaché au répertoire initial après dup, déplacement, SWAP, suppression et réutilisation du nom ; les nouveaux répertoires ne l’héritent pas. minimum_buffer_size, initial_minimum_buffer_size et seek_offset concernent seulement getdirentries64.

NAME|OBJTYPE|RETURNED_ATTRS (0x80000009) est obligatoire ; les onze champs communs existants restent disponibles si les observations choisies sont valides. Options0/8 sont admises. bulk ignore les deux mots bitmap/reserved de16 bits indépendamment du contrôle attrlist ordinaire. Un seul encodeur partage attrreference_t et la validité stat64. Seuls les enregistrements complets sont renvoyés : bourrage sur8 octets si possible, taille sur4 octets admise pour le dernier groupe sinon. Un premier groupe trop grand produit ERANGE sans changer sortie ni curseur ; une sortie requise partiellement accessible arrête avant copie. Seuls les octets réellement renvoyés doivent être inscriptibles.

dup partage la progression, les open distincts restent indépendants. Un parcours achevé non nul conserve EOF après modification de l’espace de noms et ignore taille/sortie après validation de la demande ; un répertoire initialement vide à offset0 est réexaminé. lseek à zéro réinitialise l’itération. Changement de membres avant EOF, seek non nul arbitraire et mélange getdirentries64/bulk arrêtent explicitement. Repli NAME-only, entrées ERROR, instantanés, ACL/permissions, ordre hôte et autres mask/options restent non pris en charge. bulk-attributes / bulk-attributes-values / bulk-attributes-unsupported vérifient comportement natif commun, octets virtuels littéraux et sélection inconnue conservant la sortie antérieure. La préparation ARM64 privée a réussi728 comparaisons raw/SDK avec gardes. native5s, guest/Python5,000,000us/quantum1024 et public10s restent inchangés. Intel natif, iOS physique et environnements complets restent non vérifiés ou incomplets.

Sources: [XNU bulk ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/man/man2/getattrlistbulk.2), [XNU attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h). Original implementation and probes; no Apple implementation copied.

## Modifications explicitement autorisées des attributs étendus ordinaires

setxattr(236), fsetxattr(237), removexattr(238) et fremovexattr(239) exigent une autorisation indépendante sur l’objet initial : C++ `MutableExtendedAttributes`, booléen JSON strict `mutable_extended_attributes=true`. Chaque fichier, répertoire ou lien autorisé doit déclarer une liste complète d’attributs ordinaires `extended_attributes`, éventuellement connue vide. Les autorisations d’écriture du contenu ou de modification de l’espace de noms ne suffisent pas. Autorisations et valeurs suivent l’objet conservé à travers dup, déplacement, suppression et maintien d’une projection mémoire. Les objets créés et les noms réutilisés commencent avec des attributs inconnus. Les alias connus, les indicateurs de métadonnées incompatibles, les attributs système protégés, ResourceFork, FinderInfo et la compression restent exclus.

Le remplacement garde la position dans la liste virtuelle, la suppression retire l’entrée et la création l’ajoute à la fin. Cet ordre est déclaré dans le processus ; il ne déduit pas l’ordre APFS. Les octets, nombres d’attributs initiaux et références de chemins d’autorisation restent réservés. L’excédent à l’exécution partage la limite existante de 16 MiB/4096 ; suppression, invalidation du contenu et libération finale ne récupèrent que cet excédent. Un échec de capacité, de transfert ou de délai ne publie aucun état temporaire. Une entrée requise totalement illisible donne EFAULT ; une entrée partiellement lisible arrête explicitement avant publication. Une modification réussie invalide le stat complet sans inventer de dates, tout en conservant identité, membres du répertoire, version/instantané d’énumération et curseurs.

L’ABI utilise low32 FD/options/position et full64 size. Les vérifications précoces des options privilégiées/de liens FD précèdent l’import du nom, qui précède la recherche de l’objet. set rejette un NULL de longueur non nulle avant une entrée VFS trop grande (E2BIG7) ; la recherche précède la validation du nom ordinaire, de position et des conflits. set importe la valeur complète avant EEXIST17 pour CREATE existant ou ENOATTR93 pour REPLACE absent. CREATE et REPLACE ensemble donnent EINVAL ; les suppressions ignorent ces deux bits. Une taille nulle ne lit pas le pointeur de valeur. Les autres indicateurs, permissions inconnues et comportements non observés du fournisseur arrêtent explicitement.

xattr-mutations / xattr-mutations-values / xattr-mutations-unsupported vérifient des contrôles natifs/invités originaux, des octets virtuels littéraux indépendants et un arrêt faute d’autorisation conservant la sortie précédente. La préparation privée ARM64 a vérifié726 appels raw/SDK, des observations protégées complètes de544 octets et les pages lisibles complètes. native5s/compile120s/drain1s/reap1s, guest/Python5,000,000us/quantum1024 et public10s sont inchangés. Intel natif, iOS physique, dyld, Mach IPC, threads/signaux, environnements Objective-C/Swift et frameworks complets restent non vérifiés ou incomplets.

Références ABI primaires : [déclarations des appels système XNU](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [définitions xattr](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Code et sondes originaux ; aucune copie de l’implémentation Apple.

## Liens physiques Darwin bornés

link suit la cible symbolique finale ; linkat flags=0 choisit le lien lui-même, AT_SYMLINK_FOLLOW sa cible. Seuls low32 0/0x40 sont admis ; les autres bits bas donnent EINVAL avant import. Recherche source et EPERM des répertoires précèdent la destination ; une destination existante donne EEXIST. La destination exige le droit de mutation et le même domaine de montage explicitement établi. Alias initiaux et conflits connus de périphérique, mode ou flags restent exclus.

Un alias ne consomme que l’entrée et le chemin/NUL, sans nouvel inode. Octets, droits d’attributs, validité des métadonnées et baux de mapping appartiennent à l’objet partagé. Les politiques explicites actualisent compte de liens et ctime ; sans elles, stat complet reste inconnu. Les attributs invalident stat, le contenu invalide les observations d’attributs. Descriptions et derniers mappings conservent les coûts retirés ; seul un coût immédiatement libérable est crédité. Les sous-arbres utilisent identité et parent exacts ; les alias externes restent en place et les cibles relatives utilisent le parent sélectionné.

Après plusieurs noms, F_GETPATH/ATTR_CMN_NAME restent non pris en charge même avec un ou zéro nom ; aucun modèle général de cache APFS. bulk NAME utilise l’entrée réelle, rename/SWAP du même objet conserve les deux noms. Casse EXCL, Intel HVF, iOS physique, ACL, mappings cohérents/signaux EOF, dyld, Mach IPC, threads et frameworks complets restent des lacunes. Seul ce contrat étend les exclusions précédentes.

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## Descripteurs O_SYMLINK bornés

O_SYMLINK=0x00200000 conserve le dernier lien symbolique, même cassé ou cyclique, en lecture, écriture ou lecture/écriture. Il ne donne aucun droit d'écriture sur la cible. O_CREAT suit encore la cible; NOFOLLOW garde la priorité ELOOP, la création exclusive EEXIST, et O_DIRECTORY renvoie ENOTDIR pour le lien conservé. Les composants intermédiaires et les barres finales utilisent le résolveur et NOFOLLOW_ANY existants. F_GETFL omet le bit de sélection.

LinkNode et NameIdentity sont les objets réels retenus. Dup partage les indicateurs et le curseur; les ouvertures indépendantes ont leur propre description. Renommage, suppression, réutilisation du nom ou suppression du parent ne remplacent pas l'objet détenu. F_GETPATH/ATTR_CMN_NAME utilisent le nom sélectionné unique; un historique de noms multiples interdit définitivement l'inférence vnode, même après leur suppression. Les réservations initiales restent fixes; noms, cibles, entrées et croissance d'attributs dynamiques attendent le dernier propriétaire réel. Le remplacement ne peut déduire un dernier alias encore détenu.

Les E/S n'exposent jamais la chaîne cible comme contenu. Après les contrôles existants d'import scalaire/vectoriel, d'accès et de nombre, un décalage négatif donne EINVAL. À INT64_MAX, la lecture renvoie zéro et l'écriture EFBIG; les autres décalages admis donnent EPERM, y compris pour une longueur nulle, avant APPEND ou accès aux données. Les contrôles négatifs précoces pwrite/pwritev restent inchangés. DATA/HOLE donne ENXIO aux positions non négatives, EINVAL aux négatives, sans déplacer le curseur.

ftruncate non négatif sur une description inscriptible et open TRUNC admis ne positionnent que WasWritten: cible, stat complet, xattrs, curseur, budget et inode ne changent pas. Lecture seule ou longueur négative donne EINVAL. F_SETFL admis modifie APPEND|NONBLOCK puis renvoie ENOTTY25; dup observe l'effet, une ouverture indépendante non. Les arguments inconnus arrêtent avant effets.

fpathconf fixe, fgetattrlist et les droits ordinaires FD-xattr déclarés séparément concernent le lien. FD de répertoire relatif et fchdir donnent ENOTDIR. truncate ne restaure pas le stat invalidé par une mutation d'attributs. Les mmap historiques private/shared alignés et non exécutables atteignent EINVAL par le type symbolique, sans mapping ni bail. Shared ordinaire, indicateurs inconnus, exécution et autres frontières non prises en charge sont préservés. Les18 contrôles natifs couvrent seulement length16384, offset0, protection1/2/3.

La préparation ARM64 originale compare les144 octets de stat protégés pour15 troncatures, les décalages/nombres extrêmes, seek creux et effets F_SETFL échoués. Le programme commun sans SDK s'exécute à O0/O1/O2 et compare le chemin réel du parent détenu. Les routes virtuelles comparent un littéral stat/type indépendant ou refusent explicitement les noms multiples en conservant la sortie. Intel HVF natif, iOS physique, ACL/permissions, EOF/signaux mappés, dyld, Mach IPC, threads et environnements/frameworks complets restent non vérifiés ou incomplets.

Source d'interprétation: [frontière mmap XNU correspondante](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_mman.c). Code et sondes originaux; aucune implémentation Apple copiée.

```text
O_SYMLINK=0x00200000; ENOTTY=25
open O_RDONLY|O_SYMLINK|O_TRUNC: WasWritten only
LinkNode, NameIdentity, HadMultipleNames, DetachedNames
INT64_MAX read=0 / write=EFBIG; other admitted offsets=EPERM
SEEK_DATA/SEEK_HOLE nonnegative=ENXIO / negative=EINVAL
F_SETFL: APPEND|NONBLOCK effect before ENOTTY; dup shares / independent open separate
symbolic-descriptors
symbolic-descriptors-values
symbolic-descriptors-name-unsupported
SymbolicDescriptor*, SymbolicDescriptorsRetainObjectsAndNativeErrorOrder
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 transport parameters / 15 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## État non bloquant borné des descripteurs

O_NONBLOCK=4 est admis pour les ouvertures de fichiers ordinaires, répertoires et O_SYMLINK, puis conservé par F_GETFL. Dup partage état et curseur; les ouvertures indépendantes gardent leurs descriptions. Les règles existantes d'accès, close-on-exec, WasWritten, métadonnées, octets et curseur restent applicables. Le stdin fini déclaré conserve EOF et ordre des erreurs de pointeur; son omission reste inconnue. La capture conserve erreurs de copie et budget partagé.

F_SETFL valide les arguments low32 avant effet, ajoute un selon la conversion native des flags open, puis modifie seulement APPEND|NONBLOCK. High32 est ignoré; accès et WasWritten d'entrée ne créent ni autorisation ni écriture. Les contrôles natifs littéraux3/7/11/15 sélectionnent4/8/12/0. Les descriptions symboliques changent d'état avant ENOTTY25. Les flags inconnus, dont ASYNC0x40, arrêtent avant effet.

La préparation ARM64 macOS originale contient122 observations:16 requêtes par combinaison valide objet/accès, dup conservé, open indépendant, effacement, écritures réelles et CLOEXEC par FD. Le programme commun sans SDK compare à O0/O1/O2 octets, état, curseur et ABI BSD brut carry/errno. Invité, C/CLI et Python vérifient aussi le refus des flags inconnus; les trois profils ARM64 HVF doivent réellement s'exécuter. Pas d'attente de disponibilité, pipes, réseau, kqueue, signaux asynchrones ou I/O hôte. La politique O_EVTONLY reste non prise en charge; Intel HVF natif, iOS physique et environnement complet restent non vérifiés ou incomplets.

```text
O_NONBLOCK=4; F_SETFL raw low32 mask=0x1000f
requests3/7/11/15 -> APPEND|NONBLOCK status4/8/12/0
nonblocking-descriptors / nonblocking-flags-unsupported
Nonblocking*, NonblockingDescriptorsKeepNativeControlState
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
8 model cases / 20 transport parameters / 10 public cases / 5 Python profiles
67 mandatory workloads per platform / ARM64 201 / Intel 134
```

## Observations getentropy explicites et finies

Le BSD brut getentropy500 utilise la file ordonnée `DarwinSystemOptions::EntropyReads`, avec le champ JSON `darwin_system.entropy_reads`. Chaque chaîne hexadécimale est non vide et de longueur paire : au plus256 enregistrements de1..256 octets. Ce sont des limites du modèle ; le transport JSON conserve65536 octets. Une omission reste inconnue ; `[]` est explicitement épuisé. La validation native/JSON précède toute mutation de l’image ou du backend ; les autres profils OS refusent ces options.

La longueur64 bits est examinée en premier : au-delà de256, EINVAL22 sans accès ni consommation ; zéro réussit pour tout pointeur sans entrée. Une demande non nulle admet ensuite le prochain enregistrement de longueur exacte. Absence, épuisement ou désaccord arrêtent UnsupportedService avant les effets, même pour une mauvaise adresse : cet ordre est celui de l’admission du rejeu. Succès ou EFAULT14 entièrement inaccessible consomme un enregistrement. Une destination partiellement accessible est refusée avant copie ou progression ; les erreurs de transport ne progressent pas. Une erreur ultérieure des registres de retour conserve les effets terminés. Chaque exécution recommence au premier enregistrement, même avec les mêmes options.

DarwinEntropy possède le curseur de chaque exécution ; les octets restent immuables. Le BSD et returnService existants possèdent les deux ISA, la retenue et les registres secondaires. Le programme sans SDK couvre5 profils invités et3 ARM64 HVF. Ses octets fixes ne rejoignent pas l’inventaire RNG natif déterministe. Les sondes ARM64 O0/O1/O2 couvrent594 appels ; le nombre d’octets modifiés n’est pas une longueur de copie exacte. Aucun RNG hôte, qualité cryptographique, /dev/random, import libc ou framework. Intel HVF, iOS physique et compatibilité OS complète restent non vérifiés ou incomplets.

```text
BSD getentropy500 / DarwinEntropy / EntropyReads / darwin_system.entropy_reads
full64 size>256 -> EINVAL22; zero ->0; whole EFAULT14 consumes one record
1..256 bytes per record / at most256 records / JSON transport65536 bytes
entropy-replay / entropy-missing / entropy-exhausted / entropy-mismatch / entropy-partial
15 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
68 mandatory workloads per platform / ARM64 204 / Intel 136
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU getentropy ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU generation/copyout boundary](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/dev/random/randomdev.c).

## Identité explicite du thread courant

BSD thread_selfid372 lit l’observation immuable facultative `DarwinSystemOptions::ThreadID` / JSON `darwin_system.thread_id`. Toute valeur uint64, zéro compris, est connue; l’absence arrête UnsupportedService. Les chaînes décimales conservent64 bits; les nombres JSON exigent un entier exact au plus2^53-1. Aucun ID n’est déduit de l’hôte, du PID ou d’un port Mach. Cet appel sans arguments ignore les six registres d’arguments et n’accède pas à la mémoire. La résolution existante sur32 bits préserve le numéro brut complet dans les événements; le retour BSD conserve64 bits et efface carry et RDX/X1. Les formes Mach restent non prises en charge.

Les exécutions répétées conservent l’observation et les options distinctes restent indépendantes. Cela n’alloue aucun ID, ne garantit aucune unicité et ne crée aucune identité d’événement du planificateur, ni cycle de vie pthread, TLS ou Mach IPC. Les sondes ARM64 originales O0/O1/O2 conservent24 appels comparant le résultat au SDK pthread courant, avec arguments arbitraires et bits hauts du numéro. Le programme natif commun compare uniquement des relations dans un même processus; les octets d’ID fournis sont exclus de l’inventaire natif déterministe. Intel HVF, iOS physique et la compatibilité OS complète restent non vérifiés ou incomplets.

```text
BSD thread_selfid372 / Wide / ThreadID / darwin_system.thread_id
known uint64 including0 / missing -> UnsupportedService / no memory
full64 return / low32 resolution / carry clear / RDX-X1 zero / raw event number
thread-identity / thread-identity-value / thread-identity-missing
4 model cases / 20 transport parameters / 16 public cases / 5 Python profiles
69 mandatory workloads per platform / ARM64 207 / Intel 138 unverified
original ARM64 O0/O1/O2 probes24 / native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU thread_selfid ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [libpthread current-thread owner](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/kern/kern_support.c).
