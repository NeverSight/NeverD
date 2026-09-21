**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# Récupération du code natif et des sources iOS

[← Index de la documentation](README.md) · [Vue mobile](../mobile.md)

`neverd mobile` accepte IPA, `.app` et Mach-O. Il exporte du C natif, les métadonnées du runtime et, à titre expérimental, des sources Objective-C `.m` et Swift `.swift` pour les corps natifs pris en charge. Un résultat publié peut contenir des méthodes non récupérées : examinez la couverture avant utilisation. Les conteneurs mobiles relèvent de la CLI ; le SDK C natif charge séparément le Mach-O sélectionné.

La compilation élimine commentaires, mise en forme, identifiants et constructions du langage source. Ce processus reconstruit une représentation source, sans retrouver le texte original ni certifier une équivalence de comportement pour une application quelconque. Il ne lance pas l’application analysée.

## Démarrage et dépendances

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Compilez la cible `neverd` avec une chaîne compatible C++20. Le traitement mobile fonctionne dans la CLI native sans interpréteur Python. Le démanglage des signatures Swift est fourni par `LLVMSwiftDemangle` dans le fork LLVM de NeverD. Les compilations depuis les sources et les paquets LLVM publiés correspondants incluent ce composant ; NeverD ne récupère pas séparément les sources de Swift. Compiler et exécuter NeverD ne nécessite aucune installation du compilateur ou de la chaîne Swift. Les dépendances natives comme LLVM et Capstone restent nécessaires : distribuez les bibliothèques et mentions de licence requises par votre compilation. La compilation indépendante des sources Apple générées et les tests de comportement Swift sur macOS nécessitent, selon le cas, Apple Clang, le SDK et `swiftc`.

La récupération des signatures Swift exploite directement les nœuds structurés de `LLVMSwiftDemangle` dans le processus C++. Elle ne recherche ni ne lance d’exécutable externe de démanglage ou de commande de découverte de chaîne. L’ancienne option de chemin exécutable est supprimée et l’ancienne variable d’environnement du démangleur n’est plus lue. `--metadata-only` n’exécute ni l’exporteur natif de sources ni le démanglage des signatures.

L’inventaire de signatures dans `metadata/swift-signatures.json` identifie le composant intégré ainsi :

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
```

## Entrées et sélection

Une IPA doit contenir exactement un `Payload/*.app` au premier niveau. Pour IPA et `.app`, `CFBundleExecutable` dans `Info.plist` désigne le programme principal. `--artifact` est relatif à ce bundle dans les deux formats et sélectionne un seul exécutable intégré, sans analyser récursivement tous les frameworks ou extensions. Une entrée Mach-O brute n’accepte pas `--artifact`.

Pour les binaires fat, `--arch=auto` privilégie arm64, arm, x86_64, puis i386. Une tranche absente ou non prise en charge échoue explicitement. La projection source vise actuellement arm64/x86_64 ; choisir une autre famille ne garantit pas des sources Objective-C/Swift. Une tranche sélectionnée avec `cryptid != 0` est refusée : fournissez une entrée déjà déchiffrée et lisible. Archives et répertoires refusent chemins dangereux, liens symboliques, fichiers spéciaux et entrées en conflit.

## Options et limites de ressources

| Option | Défaut | Signification |
|--------|--------|---------------|
| `-o DIRECTORY` | Obligatoire | Nouveau répertoire hors du répertoire d’entrée ; la sortie existante est conservée |
| `--platform=auto\|ios` | `auto` | Détecter la plateforme ou choisir iOS |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | Choisir une tranche Mach-O |
| `--artifact PATH` | Programme principal | Exécutable relatif au bundle |
| `--metadata-only` | Désactivé | Métadonnées seules, sans récupération source ni appel d’outil |
| `--max-func N` | `0` | Limite de fonctions natives ; zéro signifie toutes les fonctions découvertes ; ignorée en mode métadonnées |
| `--timeout N` | `300` | Budget total positif d’analyse en secondes ; les processus enfants utilisent le temps restant |
| `--max-files N` | `20000` | Budget positif d’entrées ; l’inventaire Swift est également borné |
| `--max-bytes N` | `2147483648` | Budget positif d’octets pour entrée, extraction et sortie finale |
| `--json` | Désactivé | Rapport versionné au format JSON |

Le répertoire de travail est surveillé et peut utiliser jusqu’à trois fois les budgets d’entrées/octets pour les copies et résultats intermédiaires. Les journaux sont limités à 16 MiB par processus, et le JSON des signatures Swift à 32 MiB. Ces contrôles de ressources n’isolent pas les processus. Augmenter le délai ne supprime aucune autre limite. Les méthodes exclues par `--max-func` restent non récupérées lorsqu’elles figurent dans les métadonnées.

## Sources Objective-C et structure du runtime

Les deux modes de rapport source incluent `source_projection_graph`. Ses nœuds décrivent les corps natifs typés finaux, les diagnostics locaux, les `dependencies` natives et Block fusionnées, ainsi que le résultat de production `closure_closed`. Les contrôles locaux et les échecs de dépendances propagés ont des raisons distinctes ; un corps typé absent garde des diagnostics incomplets. Résoudre un appel non lié peut révéler d’autres dépendances. Un nœud fermé n’a franchi que l’étape des dépendances : émission et contrôles du texte restent nécessaires avant l’état `recovered`. `native_dependency_graph` demeure un inventaire LowIR séparé.

Pour les analyses répétées de couverture, la commande suivante exécute les mêmes analyses et contrôles de publication que `--format=objc-methods`, mais omet `native_source` et le champ `source` de chaque méthode. Le JSON ajoute `sources_omitted=true` tout en conservant la portée complète des identités, états, diagnostics, signatures, références d’auxiliaires partagés et preuves de dépendances. Les contrôles de rendu restent actifs ; le mode complet produit les sources compilables. Le point d’entrée C correspondant est `neverd_objc_methods_summary_json(session, max_functions)` ; son résultat se libère avec `neverd_free_string`.

```sh
neverd export WMF --format=objc-methods-summary -o summary.json
```

Le chargeur natif associe enregistrement de méthode, adresse IMP exécutable et encodage de type pris en charge à des emplacements ABI source explicites. Les arguments fixes scalaires/pointeurs conservent `self`/`_cmd`, paramètres inutilisés, banques séparées d’entiers/flottants et positions de pile prises en charge. Réinterprétation des bits float/double et conversion numérique restent distinctes. Les indications de type servent à la projection source ; elles ne constituent ni preuve ABI authentifiée ni autorisation de modifier le code exécutable.

`sources/objc.m` place les instructions réellement reconstruites dans des corps `@implementation` et conserve auxiliaires C et appels typés nécessaires. Les cibles d’appel exigent une liaison source prise en charge ; cibles inconnues et groupes de dépendances incomplets restent non récupérés. Définitions absentes, adresses non exécutables, encodages contradictoires, ABI non gérées, décodage incomplet ou IR refusé ne deviennent pas des méthodes récupérées du seul fait d’une déclaration.

Les paramètres des fonctions auxiliaires natives peuvent recevoir un type pointeur pour la projection source lorsque les valeurs entrantes complètes atteignent des arguments pointeurs déjà liés via COPY/PHI, sans utilisation scalaire contradictoire. Cette inférence conserve les emplacements ABI physiques et les types IR génériques ; les corps et tous leurs groupes de dépendances natives restent soumis à validation.

Un auxiliaire scalaire natif peut renvoyer un paramètre entrant observé si son registre ABI exact couvre toute la largeur du résultat. Une preuve bornée combine chaque chemin de retour avec l’entrée initiale et les retours de boucle ; appels et écritures partielles invalident la valeur. Les valeurs entrantes non déclarées, paramètres fictifs inutilisés, marqueurs initiaux SSA et PHI ne prouvent pas les entrées. Cette inférence réservée à la projection source conserve les emplacements physiques et exige toujours la validation complète du corps et des dépendances.

Les fonctions natives locales peuvent exposer des entrées entières de largeur complète dans des registres supplémentaires, y compris des pointeurs de résultat, si l’analyse des entrées observables concorde avec les lectures du LowIR complet. Les registres sauvegardés par l’appelant peuvent ensuite servir de temporaires ; les contextes préservés exigent toujours l’absence d’écriture dans les registres préservés hors cadre. Les définitions implicites des appels, même en version SSA 0, ne sont pas des entrées ; seuls les octets explicitement préservés se propagent à travers un appel. Les appelants et définitions régénérés partagent les mêmes positions physiques. Aucun prototype C/Swift externe n’est déduit ; les corps et dépendances doivent rester entièrement validés.

Les types de rappel C fixes conservent leurs signatures de paramètres et de retour dans les déclarations et conversions source. L’import exact `swift_once` lie un indicateur, un rappel `void (*)(void *)` et un contexte, sans résultat. L’appel au runtime est conservé ; cela ne prouve ni la récupération du corps du rappel ni la propriété du stockage partagé d’initialisation. Les dépendances incomplètes restent non récupérées.

Les imports connus du runtime Objective-C conservent une liaison explicite des arguments et du résultat : retain/release, libération automatique, références fortes/faibles, allocation et setters à signature fixe. Les variantes ARM64 dédiées à un registre lisent ce registre ; identité de l’import et ABI doivent correspondre. Les appels de lecture, écriture et suppression des objets associés préservent objet, clé, valeur et politique de largeur pointeur ; le C généré utilise l’en-tête public du runtime. Les tests de recompilation macOS comparent durée de vie, remise à zéro des références faibles, copie et suppression aux méthodes originales.

Les requêtes optimisées de classe et de sélecteur de libobjc conservent les appels réels du runtime, y compris le traitement de nil et les redéfinitions personnalisées. Le résultat sur un octet conserve les conversions natives de l’appelant ; il n’est pas remplacé par un test supposé de hiérarchie de classes.

La liaison du code source construit la table vérifiée des identités des objets de classe directs uniquement lorsqu’un argument pointeur possède une adresse constante. Cette table reste locale à une opération de liaison ; chaque opération suivante revalide l’image courante, y compris les noms de classes, les indicateurs de métaclasse et les conflits d’identité.

L’export Objective-C lie aussi un ensemble fixe d’imports du runtime Swift avec l’ABI C ordinaire : comptage de références, références faibles natives ou d’objets de type inconnu, métadonnées d’objet et début/fin de contrôle d’accès. Le C généré conserve ces appels et nécessite le runtime Swift à l’édition de liens. Les entrées à registres spécialisés, les conventions d’appel Swift non reconnues et les symboles Swift arbitraires restent exclus ; les identités d’import et les emplacements scalaires doivent correspondre exactement.

Des liaisons distinctes prennent en charge, sur arm64 et x86_64, les imports Darwin exacts String → NSString `_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF` et NSString optionnel → String `_$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ`. Elles préservent la propriété et Clang `swiftcall` ; la liaison nécessite Swift Foundation et Swift Core. Le pont inverse transporte les deux mots String renvoyés dans un entier non signé de 128 bits, réparti dans les registres de retour explicites avant SSA. Ce transport de bits ne constitue ni une disposition String reconstruite ni une ABI générale pour les agrégats. Les signatures inconnues et les dépendances d’initialisation incomplètes restent non prises en charge.

Pour les arguments de clé validés des API d’objets associés, les adresses exactes de chaînes C Mach-O en lecture seule deviennent des identités partagées. Une même adresse d’origine conserve une seule clé ; des décalages internes distincts restent séparés. L’export mobile fusionne automatiquement les fonctions auxiliaires. L’API C les nomme dans `shared_identity_functions` ; chaque définition doit être liée une seule fois entre les unités de méthodes. Ces clés appartiennent au code reconstruit et ne désignent pas le stockage d’une image originale déjà chargée. Les autres usages d’adresses d’image non liées restent une limitation.

Les imports vérifiés de `os_unfair_lock_lock`, `os_unfair_lock_unlock`, `os_unfair_lock_trylock`, `os_unfair_lock_assert_owner`, `os_unfair_lock_assert_not_owner` appellent la véritable implémentation Darwin via `<os/lock.h>`, en préservant l’adresse du verrou, les contrôles de propriété et le résultat booléen. Les variantes inconnues restent non prises en charge. Les résultats entiers ne préservent que les bits déclarés par l’ABI : Darwin arm64 étend les résultats de 8/16 bits à 32 bits selon leur signe ; les autres bits du registre restent inconnus. Les largeurs de retour déclarées entrent dans SSA avant la fusion des branches, afin que les bits hauts inutilisés ne masquent pas les octets bas valides.

Les enregistrements Darwin `__cfstring` vérifiés peuvent être reconstruits en objets constants si l’import de classe, la disposition, le stockage des caractères et les fixups sont complets. Les octets ASCII et unités UTF-16, y compris les NUL intégrés, sont conservés. Une même adresse d’origine partage un objet reconstruit ; les enregistrements distincts restent distincts. Les helpers figurent dans `shared_identity_functions` et nécessitent Foundation à l’édition de liens. Cela n’autorise pas les accès mémoire bruts non vérifiés aux objets constants.

Les compteurs numériques d’une section `__DATA,__llvm_prf_cnts` aux limites établies et sans pointeurs peuvent utiliser un stockage reconstruit partagé. Les octets initiaux, les lectures et écritures de 1–16 octets qui se chevauchent et leurs mises à jour sont conservés entre fichiers de méthodes. L’export mobile fusionne les fonctions auxiliaires ; les utilisateurs de l’API C doivent lier une seule définition de chaque fonction `shared_storage_functions`. Ce stockage est indépendant de l’image d’origine et de son environnement de profilage. Les adresses qui s’échappent, les accès ordonnés, les mappages incomplets et les relocalisations de pointeurs restent non pris en charge. Les rappels block instrumentés peuvent mettre à jour ce stockage de l’image sans exposer leurs adresses privées ; les écritures ordinaires des compteurs sont distinguées des écritures dans le cadre de pile privé.

Les métadonnées de classe conservent superclasse, début/taille d’instance et ivars scalaires/pointeurs dont offsets, largeurs et alignements sont vérifiés. Les déclarations ajoutent du remplissage au besoin. Une méthode exigeant une disposition inconnue reste non récupérée. Les catégories gardent leurs identités classe/catégorie/adresse et implémentations distinctes ; les enregistrements strictement identiques répétés dans les deux inventaires ne comptent qu’une fois. Les catégories externes utilisent une déclaration Foundation existante lorsqu’elle est prise en charge. Les en-têtes externes inconnus sont signalés comme dépendances absentes, sans inventer de classe de remplacement.

La récupération exige aussi des déclarations de classe complètes, les définitions des classes ancêtres locales et un source où chaque valeur est définie avant utilisation sur tous les chemins et où chaque sortie accessible comporte le retour requis ; une superclasse locale vide ne reçoit une `@implementation` vide que si sa disposition vérifiée et un inventaire complet prouvent l’absence de méthodes ordinaires propres. Si ces preuves manquent ou si un nom entre en conflit avec un import Foundation connu, les méthodes concernées restent dans le dénominateur de couverture avec leurs motifs, tandis que les classes récupérables indépendamment restent émises ; ces contrôles de noms ne couvrent pas tous les noms de SDK, SDK iOS ou versions.

Les appels de Blocks Objective-C pris en charge exigent une ABI scalaire fixe complète, avec l’objet Block implicite et tous les emplacements des arguments et du résultat. L’encodage d’exécution `@?` est élargi en `id` uniquement dans les déclarations ; il ne fournit pas le prototype d’appel. Les références aux Blocks globaux conservent l’identité de l’objet partagé. Les captures scalaires synchrones prises en charge nécessitent une preuve du stockage natif des captures et du flux d’appel. Les captures fortes copiées par un appel vérifié à `objc_retainBlock` / `_Block_copy` peuvent s’échapper si leur construction initialise le stockage sur chaque chemin entrant et si les corps d’invocation, de copie et de destruction sont entièrement récupérés. Le descripteur généré conserve les ABI des auxiliaires et la disposition de propriété d’origine. Les références faibles/byref, les dispositions inconnues et les consommateurs non prouvés restent non récupérés. Les paramètres block C déclarés sans échappement par le compilateur sont aussi pris en charge lorsque l’import exact, la position du paramètre et l’ABI complète du rappel concordent. Ce contrat de durée de vie ne signifie pas que la mémoire est en lecture seule. Lors de l’édition de liens des unités C API, chaque entrée de `shared_block_functions` doit avoir une seule définition. L’export mobile fusionne les définitions concordantes et rejette les conflits, y compris les différences dans les fonctions privées appelées.

Les déclarations de méthodes de protocole proviennent des enregistrements locaux résolus du runtime, y compris les protocoles hérités et les méthodes obligatoires ou facultatives, de classe ou d’instance. Les listes ordinaires et relatives partagent le décodeur. Toutes les déclarations de classe et de protocole correspondantes doivent concorder avant de fournir une signature fixe au sélecteur ; les enregistrements mal formés et les cycles d’héritage ne fournissent aucun indice de type. Les pointeurs valides vers des structures, unions et tableaux restent opaques, sans inférence de disposition. `objc_metadata.protocols` expose les déclarations séparément, sans les compter comme implémentations récupérées ni établir la conformité des classes. Si le reste de la signature est identique, les résultats entiers signés et non signés sur 64 bits partagent une représentation binaire non signée ; les conflits sur des entiers plus étroits, flottants, pointeurs ou types d’arguments restent rejetés.

Les appels de format NSString déclarés par le compilateur peuvent retrouver les arguments scalaires promus à partir d’un objet de format constant vérifié. Les positions doivent être complètes et leurs types cohérents. Darwin arm64 lit les arguments variables dans des cases de pile de huit octets ; x86_64 utilise les registres entiers et flottants, puis la pile. Les appels produits conservent les points de suspension et la résolution dynamique. Les formats inconnus, les écritures de compte, long double et les extensions non prises en charge restent refusés. La même analyse couvre les imports C déclarés tels que `NSLog`, avec vérification exacte de la bibliothèque exportatrice et partage du prototype variadique entre les appels ayant des nombres d’arguments différents.

Les écritures dans le cadre de pile privé conservent chaque octet lu dans la fonction. Si les limites du cadre, les alias immuables à l’entrée et l’absence de fuite d’adresse sont prouvés, NeverD peut supprimer une écriture non lue ou raccourcir la fin non lue d’une écriture entière. Les arguments scalaires promus restent ainsi récupérables lorsque seul leur remplissage inutilisé est inconnu. Aucun bit inconnu n’est inventé. Les accès ordonnés, appels non liés, adresses ambiguës, valeurs à effets de bord et budgets d’analyse épuisés conservent les écritures originales.

Les méthodes de catégorie en collision conservent leur déclaration ABI validée même si l’ordre de remplacement est inconnu. Un appel dynamique ne peut l’utiliser que si toutes les déclarations correspondantes concordent ; les implémentations ambiguës restent exclues de la sélection des corps source. Les déclarations incompatibles ou mal formées bloquent toujours l’appel.

Les appels connus à `objc_enumerationMutation` conservent leur argument objet et la suite du flux, car un gestionnaire de mutation installé peut revenir. Les imports Darwin exacts de `__stack_chk_guard` désignent le même objet du runtime ; les lectures, comparaisons et appels à `__stack_chk_fail` restent observables dans le source reconstruit.

Les images Darwin liées en 64 bits qui importent Foundation système consultent aussi les déclarations intégrées extraites par le compilateur. Toutes les déclarations du runtime et du framework pour un sélecteur doivent être compatibles ; les signatures variadiques, les agrégats non pris en charge et les divergences entre plateformes restent non liés. Le catalogue fournit uniquement les types des appels, sans déterminer la classe du récepteur ni produire un corps de fonction. Utiliser NeverD ne nécessite pas de SDK Apple local. CoreData possède un catalogue distinct, activé uniquement par sa dépendance système exacte. Les déclarations appartiennent au framework de leur en-tête public ; les en-têtes de dépendances inclus indirectement n’activent aucun autre framework.

Le catalogue de déclarations C couvre aussi les exports de CoreGraphics et ImageIO. Les pointeurs opaques d’image et de couleur, les nombres entiers et les résultats flottants conservent leur ABI déclaré. Les alias des frameworks système publics sont générés avec les faits d’export ; les chemins privés, les versions différentes et les symboles non déclarés ne sont pas liés. Les déclarations mobile utilisent aussi la grammaire de types du chargeur : les pointeurs d’agrégats valides restent opaques, sans supposer leur disposition.

Les grands littéraux Swift immortels peuvent lier leurs octets UTF-8 à un stockage statique partagé au niveau d’un pont Foundation établi. Le nombre d’octets, les indicateurs, la terminaison, la validité UTF-8, l’immutabilité et l’import exact doivent être vérifiés ensemble. La représentation marquée et le pont d’origine sont conservés ; les zéros intégrés et les autres formes de stockage restent non liés.

Les objets NSString constants validés conservent aussi leur identité partagée lors des affectations et écritures utilisant un entier dont la provenance est une adresse de données complète. Les valeurs scalaires, adresses incomplètes, opérations numériques et accès aux octets internes des objets ne bénéficient pas de cette liaison.

Les lectures scalaires ordinaires dans des octets de l’image prouvés immuables et sans relocalisation peuvent devenir des constantes préservant leurs bits. Les entiers de 1, 2, 4 et 8 octets et les flottants de 4 et 8 octets sont pris en charge. Le stockage modifiable ou ambigu, les lectures ordonnées et les usages comme adresse restent non liés ; une occurrence numérique ne justifie pas les usages du même calcul comme pointeur.

Les appels C à paramètres fixes utilisent aussi les déclarations extraites par le compilateur et les exports et réexports du SDK. La bibliothèque dyld exacte, le symbole et l’ABI scalaire doivent correspondre ; les imports faibles, fournisseurs inconnus et prototypes non pris en charge restent non liés. Le C généré emploie des identifiants distincts liés aux symboles originaux. Les appels ordinaires de synchronisation sans tables de gestion des exceptions conservent leurs appels réels et effets mémoire.

Les lectures scalaires indexées peuvent utiliser une table d’octets immuables si l’analyse commune du flot source prouve une borne supérieure non signée sur chaque chemin entrant. Gardes, masques, largeurs natives et débordements modulaires gardent leur sémantique ; écritures et adresses locales échappées invalident les faits antérieurs. Chaque table est limitée à 4 096 entrées et 65 536 octets. La publication revérifie bornes, stockage, relocalisations et chaque usage des auxiliaires. Des bits de largeur pointeur désignant une section de l’image restent ambigus ; des fragments scalaires plus étroits ou le remplissage d’un segment ne prouvent pas une identité de pointeur. Les octets copiés servent uniquement aux lectures scalaires, sans échappement de l’adresse. Les tests exécutables comparent bits entiers, bits flottants dont le zéro négatif et comportement hors limites aux méthodes originales.

Le catalogue C comprend les déclarations de `sys/mount.h` avec leurs identités de liaison propres à chaque architecture : `getmntinfo` sur ARM64 et `getmntinfo$INODE64` sur x86-64. La sortie reste un pointeur vers un pointeur opaque ; le mode et le résultat restent des entiers signés de 32 bits, avec vérification exacte des exports système. Ni la disposition des enregistrements du système de fichiers ni le contenu du tampon retourné ne sont inventés.

Les liaisons de données externes exigent des déclarations SDK communes sans TLS et des preuves exactes des exports de bibliothèque. Le C généré référence le stockage réel du symbole et conserve les accès mémoire suivants, notamment la distinction entre un pointeur global et sa cible. Les imports faibles, identités contradictoires et stockages non pris en charge restent non liés. Les déclarations de données ne prouvent ni la construction ni la propriété des blocs. Le catalogue couvre les données de CoreData, CoreImage, CoreGraphics, ImageIO et CoreSpotlight. Le stockage des littéraux intégrés est extrait en compilant des collections vides et des objets booléens pour chaque cible ; seules les adresses directes de données externes sans TLS sont admises, avec les mêmes contrôles d’exportation. La génération exige aussi le compilateur Clang via `--clang`, en plus de libclang.

Sur ARM64, les identités de stockage externes de `kCIContextPriorityRequestLow` et `kCIContextUseSoftwareRenderer` reposent sur des déclarations concordantes des SDK complets iPhoneOS et arm64 iPhoneSimulator, avec un import CoreImage exact. Les options du contexte conservent les chargements réels de pointeurs et les valeurs d’exécution ; la liaison ne remplace ni les clés de chaîne ni le comportement de rendu.

Sur ARM64, `UIApplicationDidEnterBackgroundNotification` est également lié à son stockage externe UIKit exact. Les déclarations des deux SDK concordent ; le chargement du pointeur d’origine et l’identité de la notification à l’exécution sont préservés.

La reconstruction du runtime est limitée : propriétés et protocoles complets, annotations de propriété mémoire originales, agrégats arbitraires, arguments variadiques, corps dépendant d’exceptions et dispositions Block/captures non modélisées ne sont pas promis. L’encodage décrit les paramètres fixes et ne prouve pas l’absence de points de suspension dans l’original. Les pointeurs chaînés sont utilisés uniquement lorsque le chargeur a résolu leurs emplacements ; les formats non résolus conservent leurs diagnostics.

## Sources Swift et stockage

La sortie structurée du demangler sépare signatures appelables et métadonnées non appelables. Avant projection du corps natif, les signatures prises en charge sont liées aux symboles, entrées et ABI machine explicites du binaire sélectionné. Les receveurs Swift suivent l’ABI Swift, sans substitution des arguments cachés Objective-C. Un fichier de signatures fourni par l’utilisateur reste une indication à valider.

L’émetteur expérimental construit les fonctions libres, méthodes de classe, initialiseurs désignés et méthodes de structures à disposition fixe pris en charge, dont certaines formes de receveur mutating. Déclarations de classes/structures et champs stockés exigent des métadonnées de disposition récupérées. Les appels natifs ne sont émis que si les déclarations et corps nécessaires forment un groupe de dépendances complet et pris en charge. Les unités source regroupent déclarations et méthodes, sans pont appelant le binaire original.

Les corps des getter/setter Swift pris en charge proviennent de l’implémentation native et sont assemblés en propriétés. Un stockage sous-jacent privé conserve la disposition établie des champs ; les initialiseurs et les autres méthodes utilisent les mêmes noms de stockage. Une déclaration de propriété ou un enregistrement de champ ne suffit pas à établir la récupération du corps d’un accesseur.

Les constructeurs allouants, destructeurs/libérations triviaux, accesseurs de métadonnées de type et entrées `_modify`/resume pris en charge peuvent être projetés dans une unité de type émise. Chaque entrée exige une preuve bornée de l’ensemble du flux natif et de ses effets, des dépendances contexte/initialiseur/propriété effectivement récupérées, ainsi que les audits de gestion des exceptions et d’IR des corps associés. Les écritures de l’allocateur doivent correspondre à l’initialiseur réel ; `_modify` doit lier le champ mutable exact et la continuation. Les appels de métadonnées d’exécution conservent leur sémantique modélisée dans le type récupéré. Ces entrées sont explicitement des projections de source du compilateur, et non des corps de méthodes ordinaires récupérés séparément ni le texte source d’origine.

Dispositions génériques ou résilientes, fonctions async/throwing, conventions inconnues, accesseurs/allocateurs/thunks non pris en charge, initialisation incomplète et dépendances natives/runtime non liées restent individuellement `unrecovered`. Un symbole mangled ou nom de type nominal ne constitue pas une méthode récupérée. Symboles supprimés et nœuds du demangler non classifiés rendent la couverture partielle ou inconnue.

## Sortie et couverture

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

Les fichiers du langage source existent uniquement si du code peut être émis. `objc.json` contient classes, catégories, ivars et encodages bruts ; `objc.h` contient les déclarations prises en charge. `swift.json` contient types nominaux et symboles mangled. Les JSON de signatures/méthodes conservent classification, omissions, raisons et compteurs. Les journaux contiennent les diagnostics natifs et ceux de l’export Swift natif lorsque celui-ci est exécuté. Aucun journal de découverte de chaîne Swift externe ou de démanglage externe n’est produit. Les chemins de `report.json` sont relatifs à son répertoire. Le binaire sélectionné est un artefact d’analyse, jamais une dépendance liée comme pont de récupération dans le code généré.

Copies temporaires du paquet et JSON intermédiaires sont supprimés. Sans corps natifs, un traitement normal échoue même si les métadonnées existent. Le mode métadonnées produit seulement l’artefact sélectionné, `objc.h`, `objc.json`, `swift.json` et `report.json` ; sources et fichiers de signatures/couverture sont absents, et `native_function_count`, `objc_method_recovery`, `swift_method_recovery` valent `null`. Tous les modes utilisent les métadonnées Objective-C résolues par le chargeur natif. Les métadonnées Swift sont lues dans l’image native avec des bornes vérifiées ; les fixups, dispositions relogeables ou références non pris en charge conservent des diagnostics de résultat partiel.

Ce rapport illustratif abrégé montre volontairement une récupération partielle :

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

Le `status: "success"` externe signifie que la sortie validée a été publiée. Les états `recovered`, `partial`, `unrecovered`, `no-methods` décrivent l’inventaire découvert, sans prouver équivalence sémantique ni exhaustivité du programme original. Chaque méthode non récupérée a une raison. Pour Objective-C, `recovered` exige également des métadonnées runtime complètes. Un inventaire vide ne prouve pas l’absence de méthodes.

Une ligne Objective-C non récupérée peut inclure `native_backend: {status, reason, diagnostics}` si un unique résultat du backend correspond exactement à son identité runtime. Ce résumé facultatif et borné en taille conserve le résultat intermédiaire même si une vérification de déclaration ou de disposition mémoire échoue. Le statut, la raison et les compteurs principaux de la ligne restent déterminants ; l’absence de résumé signifie que ces éléments n’étaient pas disponibles.

Le `coverage_status` Swift compte seulement les éléments appelables classifiés. Le `status` Swift global tient aussi compte des symboles inconnus et peut être `unclassified`, `unsupported-architecture` ou `no-symbols`. Les métadonnées non appelables figurent dans `non_method_symbols` avec `not-callable`, les inconnues avec `unclassified`. `types`, `type_metadata_count`, `source_type_count` comptent indépendamment métadonnées/types émis, sans augmenter artificiellement les méthodes.

Chaque ligne Swift récupérée indique `source_representation` avec la valeur `native-method-body` ou `compiler-generated-from-type`. Les projections du compilateur conservent aussi `compiler_projection_kind` et `compiler_projection_evidence`. `source_body_method_count` compte les corps natifs récupérés ; `compiler_projection_method_count` compte les projections du compilateur prouvées. Leur somme vaut `recovered_method_count`. Les entrées du compilateur restent dans le dénominateur `method_count` et conservent leur identité exacte dans une seule unité source `type` correspondante. Des métadonnées de type ou un nom de dépendance seuls n’augmentent pas la couverture récupérée. Le JSON natif par lot inclut `source` dans les lignes du compilateur et les unités de type ; les `source_units` du rapport mobile conservent seulement les descriptions, sans `source`, et le source complet se trouve dans `sources/swift.swift`.

Le lot Swift natif décrit `source_units` par `{kind, module, name, source, method_entries, method_identities}` ; kind vaut `function` ou `type`, et chaque identité `{entry, mangled_symbol}`. Des symboles différents peuvent partager une entrée avec leurs propres projections ABI. Chaque identité récupérée doit apparaître exactement une fois, aucune identité non récupérée ne peut figurer. `method_entries` doit être exactement la projection ordonnée des entrées de `method_identities`, adresses répétées comprises. Des identités strictement identiques ne peuvent être fusionnées silencieusement. `source` concatène les sources des unités avec un saut de ligne chacune. Mobile conserve la source complète dans `sources/swift.swift` et les descriptions dans le JSON de couverture. Le `source` individuel sert à l’inspection ; concaténer ces lignes ne reconstruit pas correctement les classes.

## Exports natifs directs et SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

L’export Swift consomme l’inventaire structuré de signatures produit par le parseur intégré lors d’un traitement mobile normal. Le JSON Objective-C contient `native_source`, `native_function_count`, `objc_metadata`, puis source C, nom, type de retour et paramètres par méthode. Mobile vérifie encore déclarations, corps et dispositions avant `.m` ; sa couverture finale peut être inférieure à celle du C natif. Un export natif réussi peut ne contenir aucune méthode récupérée.

Chaque méthode fournit `projection_diagnostics` : les `items` décrivent les obstacles vérifiables indépendamment avec `code`, `reason` et les preuves disponibles de l’instruction, de l’appel, de la valeur ou de la dépendance. `checks_complete: false` indique un prérequis manquant ou une limite de ressources ; un rapport partiel vide ne prouve pas la récupération. Les contrôles sont partagés avec l’admission et préservent `status`, `reason` et `unbound_call`.

Le `native_dependency_graph` du lot parcourt le LowIR final depuis les signatures prises en charge, en suivant les appels directs vers le code de l’image. Il conserve les adresses des appelants, blocs et instructions, les cibles partagées et les cycles ; les cibles indirectes restent null. Une fonction LowIR requise manquante ou la limite d’enregistrements rend `inventory_complete` false. `targets_complete` exige aussi une cible directe pour chaque appel enregistré. Les racines non prises en charge et les destinations indirectes non résolues sont exclues ; ce graphe ne prouve ni la couverture complète de l’exécution native ni la récupération des sources dépendantes.

Les références locales de protocole validées utilisent `objc_getProtocol` pour préserver l’identité enregistrée. L’inventaire `runtime_protocols` inclut les dépendances des fonctions natives appelées. Les conflits de noms, déclarations incomplètes, emplacements importés et relocalisations non résolues restent sans liaison. L’export autonome signale l’absence d’enregistrement au lieu d’émettre un corps pouvant recevoir un protocole nul.

Le catalogue ABI du runtime Swift est généré depuis des déclarations amont figées et conserve la convention C ou Swift. Les pointeurs connus et entiers non signés de largeur pointeur forment des signatures scalaires fixes. Les appels Swift exigent un import fort exact de libswiftCore ; les deux classes de disponibilité versionnée des métadonnées prises en charge n’autorisent pas les imports faibles. Registres spéciaux, représentations inconnues, disponibilités non prises en charge et conflits restent exclus. Le compilateur ajoute à swift_willThrow des attributs swiftself/swifterror absents du DSL. Les effets et contrats existants de callbacks et d’octets sont conservés. Générer avec `scripts/generate_swift_runtime_declarations.py --input RuntimeFunctions.def --source-sha256 <pinned-hash>` ; `--check` vérifie le catalogue. Révision, empreinte et notices tierces accompagnent les faits.

Les déclarations fixes du runtime Swift préservent aussi les résultats de deux mots : deux pointeurs, ou le pointeur de métadonnées et le mot d’état déclarés. La couche ABI commune attribue les deux registres de résultat ; la validation du code vérifie à nouveau leur ordre, leurs types et l’import exact. Allocation de box et requêtes de métadonnées exécutent les véritables appels du runtime ; paramètres agrégés, dispositions inconnues et contextes cachés restent non pris en charge.

Séparément, les ponts exacts de valeurs Foundation observés par le compilateur pour URLRequest, Notification, URL, Data, Date et IndexPath conservent `swiftcall` et l’identité du fournisseur. Les résultats indirects et le contexte/self utilisent `swift_indirect_result` et `swift_context` dans leurs registres dédiés (x8/x20 sur arm64 et RAX/R13 sur x86_64), sans consommer la banque ordinaire des arguments entiers. Data conserve son transport explicite sur deux mots. Il s’agit de déclarations de supports d’appel, et non de dispositions de valeurs reconstruites ni d’une ABI Swift générale.

L’inférence des retours scalaires natifs reconnaît l’extraction complète et immédiate des deux mots d’un appel dont l’ABI de résultat est validée. Identité temporaire, offsets des membres, largeurs et registres physiques doivent tous correspondre. Une fonction auxiliaire peut ainsi renvoyer directement le premier membre ; les règles des vues de registres, les écrasements par appel et l’exigence d’un résultat défini sur chaque chemin de retour restent applicables.

Le catalogue C fixe préserve aussi les valeurs entières de 32 bits et les résultats booléens explicitement étendus par zéro. Un booléen occupe un octet non signé complet ; cela ne détermine pas les règles d’extension des paramètres booléens. Les représentations plus étroites non établies et les déclarations Swift de 32 bits restent exclues. Les tests exécutés couvrent les conversions réussies ou échouées, les rétentions comptées et la destruction des objets, en conservant les appels et leurs effets sur la propriété.

Les métadonnées des champs distinguent une disposition en octets connue des types de langage inconnus. Les classes Swift à ABI stable utilisent des variables de décalage de la taille des pointeurs, même sur arm64 ; les champs non exposés peuvent avoir un encodage de type Objective-C vide. NeverD conserve cette absence et vérifie décalages, tailles, alignement et chevauchements. Les corps C récupérés obtiennent ces décalages par recherche des ivars à l’exécution. Un type indisponible empêche toujours une déclaration de classe qui exigerait un type inventé.

L’identité des ivars Swift à ABI stable peut être conservée lorsque leurs offsets se trouvent dans un stockage zéro initialisé à l’exécution. Ces classes portent `ivar_status: "runtime"` ; les offsets inconnus sont JSON `null`, et une taille de champ nulle indique une largeur déterminée à l’exécution. Les méthodes peuvent interroger la classe existante, et les types objet déclarés suivent les slots exacts. Les offsets littéraux exigent toujours une disposition connue. Cela ne reconstruit pas la disposition des classes Swift : l’export autonome refuse encore les dispositions et types de champs inconnus.

Pour un appel natif lié directement, l’adresse d’un décalage ivar peut être remplacée par celle d’un scalaire local uniquement si la fonction appelée lit le pointeur une seule fois à l’entrée, avant tout effet observable, sans l’écrire, le conserver, le comparer ni l’utiliser autrement. La valeur locale provient de la recherche ivar existante à l’exécution ; l’ABI et le corps appelé sont conservés. Cette preuve bornée accepte un flot de contrôle linéaire et refuse les usages incertains ou les limites d’analyse dépassées. La C API récupère `.cxx_destruct` ; la syntaxe des méthodes Objective-C autonomes ne permet toujours pas d’émettre ce sélecteur.

Les tampons immuables sont reconstruits uniquement si le contrat vérifié de l’appel importé borne la lecture par une longueur non négative et exclut l’écriture, la conservation du pointeur et la comparaison d’identité des adresses. Les octets et la longueur sont préservés ; les autres usages du pointeur restent non résolus. L’échec d’assertion exact de la bibliothèque standard Swift conserve ses supports scalaires et de pile dérivés du compilateur, `swiftcall`, l’effet `noreturn` et l’orthographe du symbole de liaison ; les fonctions de diagnostic C gardent leur ABI C déclarée et un piège suivant distinct. Les chaînes statiques et le stockage des littéraux String immortels ne sont copiés que pour ce consommateur terminal authentifié qui ne conserve pas le pointeur. Les valeurs String dynamiques ou possédées ne sont pas traitées comme des littéraux.

Pour une session Mach-O déjà chargée, `neverd_objc_methods_json(session, max_functions)` et `neverd_swift_methods_json(session, signatures_json, max_functions)` renvoient ces rapports. Zéro sélectionne toutes les fonctions découvertes. Libérez les chaînes avec `neverd_free_string` ; `NULL` indique un échec expliqué par l’erreur de session. Ces API ne chargent pas les conteneurs IPA ou `.app`.

L’inférence des dépendances natives Objective-C peut réduire un paramètre entier de registre 64 bits issu de `NativeAnalysis` à 32 bits après la liaison des appels source. Une preuve exhaustive des usages HighIR doit établir que chaque occurrence n’observe exactement que les quatre octets de poids faible, soit par une extraction d’octets à décalage nul, soit comme argument entier exact d’un appel lié. La preuve est indépendante pour chaque paramètre ; les usages pleine largeur, à décalage non nul, mal formés ou interrompus par le budget conservent la largeur d’origine. La fonction auxiliaire affinée est de nouveau levée et doit réussir les contrôles ordinaires du corps et de fermeture des dépendances ; l’ABI de réécriture reste inchangée.

Les getters d’objets statiques Swift à initialisation différée exposés par des thunks d’entrée Objective-C ne peuvent être projetés que lorsque le symbole `vgZTo`, la signature de méthode d’exécution, le test du prédicat, l’appel `swift_once`, le chargement du stockage et le résultat de `objc_retainAutoreleaseReturnValue` forment un motif exact du compilateur. Les symboles du prédicat `_Wz`, de l’initialiseur `_WZ` et du stockage `vpZ` doivent correspondre ; le troisième registre brut ne peut apparaître que comme contexte once, et l’initialiseur doit ignorer ce contexte et ne posséder aucun appelant direct ordinaire. La projection reconstruit le prédicat et le stockage de l’objet, transmet null comme contexte sans effet et conserve l’initialiseur comme dépendance vérifiée. Toute divergence de forme, de symbole, d’utilisation des paramètres ou de callback reste non récupérée.

Les thunks de constructeur Swift Objective-C dotés d’un symbole `cfcTo` authentifié et de la signature d’exécution `init` peuvent aussi supprimer un troisième registre d’argument non déclaré si sa seule occurrence est le contexte d’un unique appel exact à `swift_once`. Le prédicat `_Wz` et l’initialiseur `_WZ` doivent correspondre ; le rappel doit ignorer le contexte et n’avoir aucun appelant direct ordinaire. La projection transmet null, déduit uniquement l’étendue de stockage de huit octets du prédicat et conserve tous les autres effets de contrôle, de mémoire et d’appel ; les vérifications habituelles du corps source et de la fermeture des dépendances restent applicables.

Les callbacks once ARM64 peuvent transmettre un contexte x2 autrement inutilisé à un unique `swift_once` imbriqué si le symbole externe `_WZ` et la paire interne `_Wz`/`_WZ` sont exacts, si aucun callback n’a d’appelant direct ordinaire et si le callback terminal, typé indépendamment, ignore son contexte. Le même contrat régit la découverte et la projection. Celle-ci transmet null et conserve chaque instruction ; l’ABI void autorise uniquement la suppression d’un retour pur par registre, temporaire ou constante. Les lectures de pile, chargements et appels dans le retour, ainsi que les dépendances non résolues, restent rejetés.

## Vérification et dépannage

Sur macOS, les builds avec `BUILD_TESTING` activé proposent `check-neverd-mobile-ios`, qui exécute les trois suites de récupération native via CTest.

Python sert uniquement aux scripts de test de développement ci-dessous ; la récupération mobile intégrée s’exécute dans la CLI native C++20.

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

Sur macOS, les scripts Objective-C compilent les originaux, récupèrent le `.m`, puis lient uniquement le source généré à un programme d’appel indépendant. Le script scalaire couvre les bornes entières, branches, boucles, lectures/écritures par pointeur, arguments implicites, identité des bits float/double, arguments mixtes et arguments sur la pile. Le script d’appels ajoute la distribution des messages, l’héritage, les Category, le stockage des variables d’instance, les auxiliaires natifs et les appels/captures/identités partagées des Blocks. Le corpus d’appels exige 21/21 méthodes récupérées et 134/134 résultats indépendants pour chaque variante arm64/x86_64 × classic/default. Exécutez ces contrôles avec la compilation actuelle de la CLI native.

Le script Swift strict vérifie 22 déclarations utilisateur, trois entrées getter/setter et neuf entrées appelables générées par le compilateur ; aucune ne peut disparaître de l’inventaire. Chaque variante comporte 858 vérifications indépendantes des résultats du programme original. Il compile indépendamment le `.swift` généré et son programme d’appel, sans dylib, module ou pont d’origine, ni déclaration de remplacement écrite à la main. Les cas couvrent les appels scalaires/natifs, l’initialisation et le stockage des classes, les méthodes de structures par valeur/mutating, les arguments flottants et sur la pile, les pointeurs et les boucles. La CLI native C++20 doit réussir les quatre variantes arm64/x86_64 × classic/default sans cas ignoré : chacune doit récupérer 25 corps natifs et neuf projections du compilateur, conserver les 34 identités appelables et réussir 858/858 vérifications pour les originaux comme pour le Swift généré compilé indépendamment. Ces résultats se limitent à ce corpus et ne garantissent pas la récupération d’applications quelconques ni du texte source d’origine. Le script refuse la couverture manquante, les échecs de compilation du source et les différences de comportement.

Ces quatre variantes ciblent macOS. Les deux entrées supplémentaires du compilateur sont l’initialiseur de la valeur vide et son accesseur de métadonnées ; les preuves natives les valident séparément et une unité source `struct Empty {}` conserve les deux identités. Réussir ce corpus ne valide pas une application iOS réelle.

Les trois scripts acceptent `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N` et `--work-dir NEW_DIRECTORY`. `--setup-only` valide les originaux et ne teste pas la récupération. L’acceptation exige l’exécution complète de toutes les variantes d’architecture et de fixup demandées, y compris pour le script scalaire. Une variante absente ou une architecture demandée que l’hôte ne peut pas exécuter entraîne un échec ; aucun cas ne peut être ignoré. Les artefacts d’échec conservés permettent de distinguer une couverture source manquante, une erreur de compilation et une différence de comportement. Consultez les résultats actuels avant d’annoncer une prise en charge vérifiée.

Le [workflow Mobile Real Applications](../../.github/workflows/mobile-real-apps.yml) utilise des applications publiques dont les versions sont fixées dans le [manifeste du corpus](../../scripts/mobile_real_apps.json). L’acceptation exige un inventaire indépendant de tous les Mach-O des bundles iOS complets et de tous les DEX des APK, des reconstructions indépendantes des originaux et du source généré, ainsi qu’une comparaison des comportements. Une étape manquante, une couverture d’inventaire inconnue ou un cas obligatoire absent fait échouer cette validation. Les étapes recompile et behavior des applications réelles restent incomplètes : la mention Experimental est donc conservée. La réussite des tests de garde du harnais ne prouve pas celle des applications réelles.

La publication est transactionnelle : choisissez un nouveau répertoire, vérifiez d’abord le code de sortie et placez le JSON redirigé ailleurs. Les échecs suppriment les résultats temporaires et préservent l’existant. Une sortie non nulle du backend inclut une fin de journal bornée. Un dépassement de délai du backend conserve le message initial et y ajoute une fin de journal bornée lorsque le texte de journal déjà capturé est disponible. Les dépassements de budget conservent leurs propres messages. La CLI native renvoie zéro en cas de succès et une valeur non nulle en cas d’échec. Avec `--json`, les erreurs traitées comprennent `schema_version`, `status: "error"` et `error`. L’analyse des arguments, le démarrage de l’exécutable ou des bibliothèques natives et les interruptions peuvent être signalés uniquement sur stderr. Vérifiez d’abord le code de sortie.

Pour une tranche chiffrée, fournissez une entrée lisible ; pour une architecture absente, examinez les tranches disponibles. Consultez les raisons exactes et diagnostics des méthodes omises. Augmenter `--max-func` aide seulement les fonctions exclues par cette limite. Dispositions, signatures, en-têtes externes, exceptions ou ABI manquants exigent une implémentation ou des métadonnées valides supplémentaires, pas une affirmation de récupération complète. Conservez les mentions de licence applicables lors de la distribution des outils ou paquets générés.

L’inférence des auxiliaires scalaires natifs prend aussi en charge les paramètres et résultats float/double en registres. L’analyse partagée des octets d’entrée MedIR doit prouver qu’une entrée vectorielle large n’expose qu’une voie scalaire basse. Une valeur locale CONCAT ne peut être réduite que si toutes ses définitions ont la même largeur basse, si les expressions hautes supprimées sont sans effet et si chaque usage lit explicitement ce préfixe. Les appels, écritures, positions des branches et bits flottants hors NaN sont conservés ; aucun bit haut inconnu n’est inventé.

Un auxiliaire natif feuille transférant le contrôle vers des appels terminaux void externes validés peut recevoir une signature source interne void s’il n’a aucun résultat scalaire complet. LowIR doit faire correspondre chaque appel et son retour synthétique. La preuve de feuille interdit l’écriture des registres préservés, de cadre, de pile et de lien ; un point fixe borné de contamination par octet rejette aussi le stockage d’une valeur dérivée de la pile ou son passage à un appel. Les contrôles du CFG, du corps et des dépendances restent actifs. Aucun bit de résultat n’est fourni : les appelants lisant un résultat inconnu restent non récupérés. Les régressions vérifient la destruction conditionnelle d’objets, les résultats indépendants et le rejet des lectures de résultat inconnu.

Les résumés natifs void prennent aussi en charge les cadres de pile et les appels ordinaires si une analyse LowIR bornée prouve la restauration des octets de registres préservés, du pointeur de pile et du registre de lien à chaque sortie. Écritures partielles, extensions implicites par zéro, stockages superposés et appels invalident les faits concernés. Les stockages par des adresses dont il est prouvé qu’elles ne contiennent aucun octet dérivé du cadre sont disjoints du cadre privé de l’appel ; les alias partiellement ou entièrement dérivés du cadre, ainsi que le stockage ou la fuite de son adresse, font échouer la preuve. La seule exception d’emprunt du cadre est le premier argument exact d’un `objc_msgSendSuper2` lié à la source : il peut désigner un objet `objc_super` complet de 16 octets entièrement situé dans le cadre alloué, car ce contrat d’exécution le lit de manière synchrone. Les messages ordinaires, pointeurs incomplets, objets hors cadre et tout autre argument de cadre restent rejetés. La vivacité par octet de HighIR traite une valeur d’emplacement de pile comme une lecture bornée des octets de cet emplacement, tandis que prendre ou transmettre son adresse reste une fuite ; les accès mal formés ou superposés restent conservateurs. Les stockages privés disjoints peuvent ainsi supprimer uniquement les octets suffixes non lus. Après une nouvelle levée, les paramètres de registres auxiliaires sans occurrence dans HighIR peuvent être retirés avant de relancer le pipeline. Le nettoyage HighIR existant reste responsable des stockages privés ; les paramètres ordinaires et les entrées réellement utilisées sont conservés.

Le catalogue UIKit arm64 lie aussi le résultat objet de `observedProgress` et `setProgress:animated:` de `UIProgressView`. Ce dernier conserve des registres distincts pour les arguments `float` et booléen. Les deux déclarations exigent des preuves concordantes produites par le compilateur pour iPhoneOS et arm64 iPhoneSimulator, ainsi que le fournisseur système UIKit exact. Les autres architectures restent non prises en charge.

Un mot de littéral marqué peut contenir une adresse complète de l’image issue d’un pool immuable de chaînes C sous le marqueur exact du bit de poids fort. La liaison source ne reloge que cette adresse prouvée dans le pool partagé permanent, en conservant le OR entier, le marqueur et le décalage interne. Les valeurs scalaires ou adresses partielles similaires, les provenances contradictoires, les pools modifiables ou avec relocalisations et les usages directs comme adresse mémoire restent refusés ; aucun agencement d’objet Swift String n’est déduit.

`setProgress:` de `UIProgressView` exige un récepteur prouvé, notamment un résultat de propriété typé conservé par un appel ARC d’identité exact. La fermeture vérifiée des superclasses et protocoles sélectionne l’argument `float`. Sans récepteur qualifié, l’appel reste ambigu, car UIKit et des classes locales déclarent aussi des setters à argument objet sous ce sélecteur.

Le catalogue UIKit arm64 décrit aussi `setTitleColor:forState:` de `UIButton`, avec un objet et un `UIControlState` non signé de 64 bits, ainsi que la méthode void `invalidateIntrinsicContentSize` de `UIView`. Les AST complets de l’appareil et du simulateur concordent. Le lien d’héritage observé `UIImageView → UIView` permet aux preuves de receiver existantes de distinguer les setters locaux d’objets des setters flottants homonymes de classes sans rapport. Les receivers inconnus, les déclarations contradictoires de sous-classes, les fournisseurs de parents absents et x86_64 restent non pris en charge.

Une recherche dans un dictionnaire retournant un `id` non qualifié n’acquiert pas de classe réceptrice en rejoignant un chemin qui retourne une classe connue via `new`. Chaque chemin entrant doit conserver une preuve du récepteur ; même un argument pointeur explicitement typé ne permet pas de choisir un setter d’objet face à une déclaration flottante incompatible.

Sur arm64, `+[NSSet setWithObjects:]` conserve la déclaration variadique du SDK terminée par nil. Le premier cas pris en charge exige un import exact de la classe système et un stub de sélecteur, ainsi que des chaînes Objective-C immuables dont les valeurs complètes de huit octets concordent sur tous les chemins entrants. La récupération s’arrête au premier nil certain. Si le premier objet est nil, aucune preuve des arguments supplémentaires sur la pile n’est nécessaire ; les cases suivantes ne sont pas lues. L’ABI variadique Darwin commune conserve trois paramètres fixes et place les autres objets et nil sur la pile. La publication revalide la déclaration, le fournisseur, le récepteur, le sélecteur et chaque argument dans l’image courante. Les objets dynamiques, écritures absentes ou partielles, cadres échappés, redéfinitions locales et autres architectures restent non pris en charge. Un argument sur la pile exige aussi une preuve linéaire dans un cadre privé au moment précis du chargement : une écriture ultérieure ne change pas la valeur déjà lue, et les écritures chevauchantes, appels inconnus, échappements ou flux de contrôle empêchent la certification.
