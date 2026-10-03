**Langues**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← Index de la documentation](README.md)

# Tester NeverD

Les tests de NeverD répondent à trois questions distinctes : la représentation
a-t-elle la forme attendue, un parcours complet fonctionne-t-il avec une
fixture binaire et le code généré préserve-t-il le comportement ? Choisissez la
plus petite suite qui répond à la question du changement, puis exécutez
l’agrégat plus large avant une pull request à haut risque.

## Configurer une compilation de test

Les tests sont désactivés sans `BUILD_TESTING`. Une compilation Release est le
choix normal pour la suite complète ; Debug conserve les assertions et le pas à
pas, mais n’est volontairement pas optimisé ni représentatif des benchmarks de
décodage.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

L’ensemble complet de fixtures nécessite `clang` pour compiler vers plusieurs
cibles et les linkers LLVM (`ld.lld` et `lld-link`) sur le `PATH`. CMake produit
sans condition de nombreux objets relogeables et les fixtures ELF/PE liées
lorsque le linker correspondant existe. Un test ignoré parce que l’hôte ne peut
pas compiler ou lier sa fixture est une couverture non exécutée, pas une
réussite de la cible.

Consultez [CONTRIBUTING.md](CONTRIBUTING.md) pour le clone, les profils
de compilation et LLVM précompilé sur macOS.

## Vérifications de la récupération d’interpréteur

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource)Tests$' \
  --output-on-failure
```

```sh
cmake --build build-release --target NeverDLowIRUndefinedIndependenceTests \
  NeverDOriginalBinaryUndefinedIndependenceTests \
  NeverDX86UndefinedEffectsTests NeverDX86CarryArithmeticFlagTests \
  NeverDX86LogicIdentityTests NeverDX86NoIndexAddressTests --parallel 4
build-release/bin/NeverDLowIRUndefinedIndependenceTests
build-release/bin/NeverDOriginalBinaryUndefinedIndependenceTests \
  --gtest_filter='OriginalBinaryUndefinedIndependence.*'
build-release/bin/NeverDX86UndefinedEffectsTests
build-release/bin/NeverDX86CarryArithmeticFlagTests
build-release/bin/NeverDX86LogicIdentityTests
build-release/bin/NeverDX86NoIndexAddressTests
```

Les tests de récupération couvrent les valeurs par défaut v1/v2/v3, les budgets explicites, les structures tronquées, tous les champs reserved et les extensions futures. Les tests CLI vérifient l’épuisement et la réussite avec les deux ABI et moteurs source, rejettent les limites décimales invalides et exigent `--devirtualize`. Un budget épuisé ne doit publier ni source ni graphe résiduel partiel.

Les tests v4 figent les tailles et le remplissage des préfixes, refusent les structures tronquées et drapeaux inconnus, préservent les anciens/futurs formats et conservent les bornes dans le C sans rapport. Des exemples CLI indépendants exigent le chaînage pour les corrélations et les bornes pour une comparaison non signée de pile ; les deux backends C exécutent O0/O2 avec pièges de comportement indéfini. Désactiver la découverte doit changer un résultat qui en dépend. L’analyse vérifie zéro, valeurs extrêmes, dépassements, plages mal formées et prérequis absents. Python vérifie disposition, drapeaux, signatures et propriété des rapports d’échec.

`NeverDByteMemoryForwardingTests` couvre les dernières écritures qui se chevauchent, les deux ordres des octets, les largeurs multiples de huit jusqu’à i128, les valeurs définies, les instantanés undef/poison corrélés et les écrasements partiels. Les cas négatifs conservent les lectures face aux alias inconnus, conversions d’espace d’adressage, appels, accès ordonnés, changements de durée de vie, octets absents, offsets dynamiques ou invalides, branches et boucles. Les PHI à nombreuses entrées, budgets nuls/exacts/épuisés et refus des instantanés par défaut sont testés. Les LLVM original et réécrit sont exécutés à O0/O2 contre une référence arithmétique indépendante ; les suites MBA, LLVMC et de sources d’interpréteurs protègent la compatibilité.

Les régressions de vivacité par octet couvrent lectures disjointes, écritures partiellement observées et couverture par plusieurs écritures. Les adresses gardées couvrent AND/OR/XOR, 32/64 bits, les deux ordres des octets, racines incorrectes, masques incomplets, jonctions contournant le test et toutes les limites de budget. Les oracles indépendants O0/O2 vérifient les deux branches, les seize résidus d’adresse et chaque octet du tampon.

Les régressions des relations d’adresse couvrent PHI/select entiers et pointeurs, les deux ordres des octets et largeurs de pointeur, les retours de boucle stables ou variables, undef/freeze, les cycles sans ancrage, les budgets adjacents et l’invalidation pour un seul changement d’adresse. À O0/O2, les boucles comparent chaque résultat et chaque octet du tampon à une référence indépendante par itération, avec une adresse mobile qu’il ne faut pas considérer constante.

Les tests numériques couvrent le transfert entier ou partiel depuis un écrivain, undef/poison sans nouvel instantané, les deux ordres d’octets, 32/64 bits, les offsets négatifs modulaires, les chevauchements partiels, les alias entre racines et allocas, les exceptions, les boucles, les limites de budget adjacentes et l’invalidation après seule suppression de stores. À O0/O2, original et transformation sont comparés à un oracle indépendant pour le résultat et chaque octet du tampon aliasé.

`NeverDMedMutableSourceTests` et `NeverDLLVMCValueTests` exécutent à O0/O2 des boucles indépendantes, blocs réordonnés, retours vers l’entrée, calculs de pile à l’exécution, lectures antérieures, jonctions, alias partiels, valeurs logiques et comptages de bits incluant zéro. Les cas négatifs refusent avant émission les entrées mal formées, cibles tronquées, supports ambigus et budgets épuisés. Un cas CLI dépassant la limite SSA exige une sortie LLVMC exécutable et un refus explicite de HighC. Les mises à jour répétées et les chaînes d’expressions stockées entre blocs vérifient aussi la taille et l’exécution du C généré.

Des régressions supplémentaires bornent les lectures et écritures privées avant promotion LLVM et la taille du C. Elles exécutent à O0/O2 de longues chaînes arithmétiques mixtes, des blocs SSA réordonnés, des écritures mémoire qui se chevauchent et des retours zéro. Les cas clés passent aussi par le véritable pipeline d’optimisation LLVM ; la réutilisation de l’émetteur après un refus de génération est vérifiée.

Les régressions de conditions composées exécutent à O0/O2 les conjonctions et disjonctions avec égalité à une constante non nulle, comparaisons non signées, comparaisons signées dans les deux ordres, booléens élargis et toutes les combinaisons de négation. Le C doit conserver toute la table de vérité sans déréférencer un opérande absent de comparaison à zéro. Les écritures d’adresses entières couvrent les valeurs alignées et non alignées sur 32/64/128 bits. Les tableaux d’octets conservent leur alignement explicite et les accès exacts à la base et partiels, sans affectation scalaire au tableau ni alias par des types incompatibles.

`NeverDLowIRRefinementTests` couvre les graphes réellement reconstruits, les boucles finies de structures différentes et leurs cas sans itération, les producteurs dynamiques, les choix conditionnels, les vues d’entrée superposées, les copies et sauvegardes corrélées, les preuves de lecture immuable des deux côtés, les drapeaux système et la préservation du retour. Candidats erronés, écritures supplémentaires, chemins incomplets ou infinis, preuves périmées, collisions temporaires et budgets partagés épuisés doivent refuser le certificat. Les tests d’indépendance existants refusent toujours les valeurs arbitraires observables.

Dans la même cible, `LowIRLoopRefinement.*` et `BinaryLowIRLoopRefinement.*` couvrent les compteurs arbitraires sur 64 bits, les rangs lexicographiques imbriqués, les résidus natifs réels, les préfixes d’entrée, les vues superposées et les sauvegardes corrélées. Les contrôles négatifs rejettent corps incorrects, domaines d’entrée réduits, rangs non décroissants, rebouclages non signés, écritures antérieures oubliées, coupures absentes, modèles malformés et budgets partagés épuisés. Un chemin frère fini réussi ne valide jamais une induction incomplète.

`LowIRLoopInference.*` et `BinaryLowIRLoopInference.*` utilisent des compteurs, sauvegardes sur pile, retours anticipés, appels natifs et drapeaux compactés écrits indépendamment. Ils couvrent l’élargissement arithmétique étroit et les drapeaux égaux malgré des expressions différentes. Graphes malformés, origines absentes ou falsifiées, boucles infinies ou avec rebouclage et budgets épuisés ne doivent produire aucun certificat.

Les régressions à en-tête et retour partagés couvrent les compteurs 32 bits étendus par zéros et 64 bits complets, les rangs scalaires à pas non unitaire, les résultats erronés, les chemins sans progrès ou avec rebouclage modulaire, et les budgets exacts ou épuisés entre recherches scalaire et par tuples. `LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`. Des régressions supplémentaires d’incrémentation et de remise à zéro exigent une convergence sans dérouler un bit de compteur par tour et rejettent l’absence de progrès et le rebouclage non signé. Les régressions d’ordonnancement couvrent les accumulateurs à pas non unitaire, le bruit de compteurs unitaires pouvant reboucler près d’un rang scalaire valide à pas non unitaire, et trois compteurs dont le tuple valide dépasse la fenêtre initiale. Des budgets de rang exacts et réduits d’un essai vérifient une reprise déterministe sans répétition.

`LowIRLoopPlanPairing.*`, dans la même cible, vérifie le renommage des registres, des corps arithmétiques différents, les préfixes propres à chaque côté, les prédicats conservés, les entrées mémoire partagées, les coupures imbriquées et les budgets de preuve indépendants. Relations manquantes, écritures incorrectes, temporaires mal liés, appariements incomplets et limites de métadonnées épuisées ne doivent produire aucun certificat.

`LowIRLoopAlignment.*` vérifie des boucles à compteur en frame ordinaires et tournées, écrites indépendamment : chaque plan par défaut se prouve séparément, le premier appariement échoue, puis une autre coupure candidate établit la relation. Les régressions couvrent permutations de plusieurs coupures, résultats et écritures erronés, relevés originaux manquants ou périmés, witnesses explicites de valeurs indéfinies, compteurs non décroissants ou avec rebouclage, graphes malformés, cumul des requêtes après échec, budget total exact et limites épuisées. Aucun refus ne doit contenir de certificat. Les nouveaux cas vérifient les phases distinctes de remise à zéro et de progression, le déplacement équivalent des gardes de sortie nécessitant un appariement entre familles, le cache sans répétition et le dépassement cumulé des métadonnées. Un cycle indépendant ultérieur vérifie la couverture complète avec une limite explicite de 16384 requêtes d’inférence. Les familles vides ou redondantes ne font aucune requête symbolique ; des coupures insuffisantes sont refusées. Budgets globaux exacts ou réduits d’une tentative, résultats erronés, absence de progression, preuves originales et witness indéfinis restent contrôlés. Les régressions du filtre couvrent les losanges arithmétiques neutres, les convergences locales ou limitées à la frontière et un point de jonction accessible mais contournable vers une sortie ou une frontière de boucle. Elles vérifient la recherche du candidat filtré malgré une famille originale redondante, la réutilisation d’un plan filtré avant une tentative complète ultérieure, les limites exacte, réduite d’une unité ou nulle de `MaxCutSelectionWork`, le cumul des échecs dans `CutSelectionWork` et l’arrêt avant toute inférence symbolique lorsque le travail global est épuisé. La couverture complète des cycles est vérifiée pour les deux familles de branches ; le losange utilise des limites explicites de requêtes d’inférence et de preuve.

Les régressions couvrent tranches de frame et de registre, deux directions, positions basses/intermédiaires/hautes, largeurs inhabituelles, deux ordres des octets et mots de frame de trois octets. Elles vérifient découverte tardive, mutations des bits préservés, absence de progrès, rebouclage sans garde, entrées supplémentaires invalides, budgets exacts/insuffisants et recherche à coupure unique.

Les régressions de phase initiale couvrent deux ou trois boucles successives réutilisant un mot de compte à rebours, leur combinaison avec des phases de boucles imbriquées, les budgets de rang et de requêtes exacts ou réduits d’une tentative, les boucles sans progression et les remises à zéro revenant à une phase antérieure. Des constantes de phase erronées de même largeur, des résultats incorrects ou des écritures de frame incorrectes doivent être refusés sans certificat par le vérificateur complet ; les preuves originales manquantes restent non prises en charge.

`InterpreterMachineStateModel.*` dans `NeverDLowIRRefinementTests` utilise des exemples LowIR indépendants : drapeaux d’entrée bruts, statut distinct de RAX invité, 17 mots d’état, sous-registres, drapeaux empaquetés, rejet dynamique persistant, écritures du cadre invité, deux branches et inférence cyclique suivie d’une nouvelle preuve. Sorties erronées, statut perdu, mémoire modifiée, enregistrements périmés, entrées malformées et budgets épuisés doivent échouer. Les tests source existants exercent aussi les deux voies C à O0/O2 ; les tests du modèle seuls ne certifient pas le C compilé.

`NeverDLLVMInterpreterModelTests` compare du LLVM indépendant à des oracles LowIR de tout l’état : largeurs, PHI parallèles, switch, mémoire invitée, statut distinct, gardes poison, plages intrinsèques, contrats refusés et quatre budgets. Il vérifie une preuve complète de décompte sur un mot arbitraire et rejette un statut modifié. Du C indépendant compilé à O1/O2 doit respecter les mêmes observations. Ces tests valident le modèle admis ; découverte automatique d’invariants et correction du compilateur restent séparées. Les cas de décalage variable couvrent les quatre largeurs, les comptes bornés par masque ou branchement, les valeurs limites et excessives, les indicateurs sans débordement et exacts, le refus strict du poison et du C compilé en O1/O2.

Les régressions couvrent les plages partielles et séparées, les alias fixes, les deux branches, chaque retour, les lectures à la première itération et les écritures précédant les lectures dans une boucle. Lecture avant écriture, écriture manquante, écriture invitée, alias inconnu, accès spécial, plage hors objet et épuisement des budgets doivent échouer. Un exemple C indépendant qui écrit un mot d’état sans le lire est compilé à O1/O2 ; il conserve les attributs LLVM exacts et passe une nouvelle preuve composée du natif vers LLVM.

Les tests de décompte gardé couvrent la reprise après rejet du modèle du corps, une preuve complète sur un mot arbitraire à l’en-tête, les budgets partagés et le refus immédiat d’une violation réelle du contrat d’entrée.

`NeverDInterpreterLLVMRefinementTests` vérifie la composition nouvelle, la liaison texte/fonction exacte, les budgets indépendants, toutes les observations et le domaine source élargi. Des octets, résidus, résultats, drapeaux, statuts, écritures, poison ou plans faux/périmés doivent empêcher l’attestation composée. Le décompte sur un mot arbitraire exige les deux prémisses inductives ; des exemples C indépendants compilés en O1/O2 vérifient le LLVM sérialisé réel. Les régressions refusent les retours d’entrée cachés et bornent les racines sans copier la provenance accessoire.

```sh
cmake --build build-release --target NeverDLLVMInterpreterModelTests --parallel 4
build-release/bin/NeverDLLVMInterpreterModelTests
cmake --build build-release --target NeverDInterpreterLLVMRefinementTests --parallel 4
build-release/bin/NeverDInterpreterLLVMRefinementTests
```

Les sorties par égalité mises en cache dans des boucles à deux et trois niveaux couvrent les opérandes corrélés, les bornes mobiles, les compteurs réinitialisés et les copies altérées.

Les régressions des comparaisons en cache couvrent égalité et inégalité, gardes et initialisations constantes, champs découverts après élargissement et bits 7/31/63 des caches de 1/4/8 octets. Modifier seulement un bit voisin tout en conservant le bit testé doit échouer à la comparaison de tout l’état. Pas nul, bornes mobiles, remises à zéro et budgets épuisés doivent être refusés.

Les régressions de généralisation couvrent les entrées jointes, le premier témoin sans itération, les différences cachées de registres/cadre, les prédicats booléens non canoniques, les conditions de trap natives, les sauvegardes corrélées et les plans invalides ou hors budget. Des compteurs indépendants à deux/trois niveaux et des octets natifs vérifient les bornes non signées, les domaines zéro/maximal, les pas non unitaires et les instructions originales incorrectes. Inférence et preuve finale doivent refuser tout résultat incomplet.

Des régressions indépendantes avec des boucles alternatives couvrent les deux orientations de branchement, les corps incorrects, une branche sœur non terminante et l’épuisement des budgets partagés de recherche/preuve. `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

Les régressions couvrent deux et trois niveaux imbriqués, les compteurs croissants et décroissants, les phases inférées et les coupures dans des corps natifs réels. Les domaines de préfixe inaccessibles ou disjoints, les corps incorrects, les transitions infinies ou avec rebouclage arithmétique et les budgets partagés épuisés doivent être refusés. Un témoin de préfixe ne remplace jamais la couverture complète des segments.

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests` vérifie l’indépendance de deux exécutions sur des graphes LowIR complets et acycliques. Les entrées ordinaires sont partagées ; chaque nouvelle valeur indéfinie par l’architecture conserve ses corrélations dans les copies, écritures superposées, sauvegardes en pile et rechargements. Les prédicats de contrôle sont vérifiés avant les hypothèses de chemin. Un certificat exige des métadonnées d’effets `Complete`, liées exactement aux limites complètes de chaque instruction et à l’empreinte de ses opérations. Preuves manquantes, boucles accessibles, appels, alias inconnus et budgets épuisés entraînent un refus. Le résultat dépend des observations explicites et du contrat de frame sans faute mémoire ; ce n’est pas une preuve complète d’équivalence du code natif vers C.

Le comportement suivant applique le contrat d’audit strict par défaut. `NeverDOriginalBinaryUndefinedIndependenceTests` utilise des octets x64 indépendants à mappages fixes pour vérifier les CALL/RET physiques, les retours modifiés, les ensembles finis exhaustifs de cibles indirectes et les lectures immuables. La même cible vérifie la collecte complète des branches directes, la liaison exacte des octets/effets/mappages/témoins de lecture, la préservation du RSP et de la case de retour d’entrée au retour externe, et la séparation cadre/image. Instructions absentes ou chevauchantes, branches non auditées hors des règles exactes de piège et de projection sous profil explicite, boucles non terminantes ou hors budget, énumération incomplète, profils/contrats incompatibles et budgets épuisés doivent être refusés sans certificat ni code résiduel. Tous les chemins natifs réalisables doivent terminer. Ce contrôle facultatif ne certifie ni invariants de boucle, ni exceptions, ni exécution avec CET, ni équivalence du natif à C ; la récupération ordinaire reste distincte. La cible vérifie aussi les limites terminales de `INT3`/`UD2` liftés strictement et la liaison de tous leurs octets et empreintes d’opérations. Un sidecar de sorties indéfinies `Missing` doit rester `Missing` ; seuls les traps prouvés inaccessibles par exécution symbolique peuvent figurer dans un certificat, tandis que tout chemin réalisable vers un trap doit renvoyer `ContractViolation` sans certificat ni code résiduel. La continuation après les traps et la reprise après exception ne sont pas modélisées, `codeFollowsTrap` n’est pas utilisé et le support de l’API LowIR statique reste inchangé.

Les tests explicites de chevauchement natif couvrent de vrais branchements x64 dans des opérandes immédiats, les résultats des deux chemins possibles et les entrées de retour indirectes dans des instructions précédentes. Les fournisseurs synthétiques vérifient les chevauchements inclus dans les deux ordres de collecte, les octets contradictoires sur une branche directe non prise, la cohérence code/lecture dans les deux ordres et les lectures du candidat. Les budgets exacts et insuffisants comptent aussi les octets répétés après des transferts indirects. Une modification des résultats, un usage statique ou en boucle et des preuves contradictoires doivent refuser le certificat ; modifier l’option ou la limite change les condensats.

Les tests de frontières explicites couvrent RCL, XADD mémoire et REP MOVS inaccessibles, les contradictions symboliques de chemins, les branches dépendant de valeurs arbitraires et les refus exacts depuis l’entrée, un saut indirect, CALL ou RET. Ils vérifient l’accès indépendant à un suffixe accessible, les collisions d’adresses candidat/natif, les preuves mal formées ou partielles, les budgets épuisés, le refus des API statiques/de boucles et les trois couches d’empreintes du raffinement. Modifier une instruction inaccessible ou activer l’option sans frontière retenue change les empreintes. Ces tests valident la portée finie déclarée, pas la sémantique des instructions non auditées.

Les tests de drapeaux regroupés couvrent toutes les combinaisons d’entrée scalaires, les masques de privilège, TF/AC dans les deux exécutions, les producteurs indéfinis distincts, les copies corrélées, les appels natifs, l’état des branches sœurs, l’observation finale obligatoire, les preuves malformées et les budgets. Toutes les entrées réalisables des boucles finies doivent terminer ; une branche sûre ne masque pas un chemin infini ou tronqué. RDSSPD/RDSSPQ couvre les 16 registres généraux, les deux largeurs, les bits hauts conservés, les preuves `Missing` préservées et les projections falsifiées. Les tests d’état machine comparent les deux sorties C à O0/O2 avec pièges de comportement indéfini à un oracle indépendant des drapeaux utilisateur, et vérifient la persistance des violations du profil. Les tests INCSSPD/INCSSPQ couvrent les deux largeurs et tous les registres généraux, les limites inaccessibles conservées, les pièges réalisables après une branche sœur terminée, les opérandes nuls et les preuves de piège falsifiées.

`NeverDX86UndefinedEffectsTests` vérifie les métadonnées des bits indéfinis, les drapeaux définis ou conservés et le refus des certificats périmés. `NeverDX86CarryArithmeticFlagTests` compare la retenue auxiliaire d’ADC/SBB, pour les formes registre et mémoire, à un oracle arithmétique. `NeverDX86LogicIdentityTests` vérifie qu’AND avec deux opérandes identiques efface encore les bits 63:32 du registre de 64 bits correspondant lors de l’écriture d’une destination de 32 bits en mode 64 bits, tout en préservant les bits non écrits des destinations plus étroites.

`X86RotateUndefinedEffects.*` compare tous les comptes bruts, largeurs, chevauchements de CL, alias d’octet haut et destinations mémoire à un oracle arithmétique scalaire. `X86BitTestUndefinedEffects.*` couvre les index registre/immédiat, le chevauchement source/destination, les registres étendus, les indicateurs définis et les écritures des parties hautes. Les contrôles refusent les opérandes, encodages et formes non pris en charge modifiés. Les preuves natives distinguent lectures corrélées et nouveaux bits indépendants, vérifient les budgets exacts/insuffisants et refusent un débordement indéfini observable. Le raffinement de l’état complet accepte le témoin sélectionné et refuse les témoins zéro ou les candidats altérés.

`X86XaddAudit.*` vérifie les 65,536 paires d’opérandes octet, les limites des indicateurs aux largeurs supérieures, les recouvrements de registres/octets hauts, les deux écritures, les limites de largeur octet sous REX et la préservation des registres complets face à un oracle arithmétique non signé. Les contrôles natifs exigent zéro nouveau bit arbitraire sans perdre les dépendances antérieures. Les deux témoins acceptent XADD inchangé et refusent une somme, une source échangée ou un indicateur défini modifié. L’alias `/6` utilise la matrice complète des compteurs de décalage ; les groupes/ID décodés modifiés doivent être refusés.

`NeverDPEFixedImageTests` utilise des fichiers PE construits indépendamment pour vérifier les instructions relocalisées, les données immuables, les écritures des imports, les en-têtes/tables malformés, les alias et la provenance modifiée. Les preuves natives vers LowIR et LLVM exact acceptent les candidats conformes et refusent les résultats, statuts ou octets natifs modifiés. L’épuisement du budget de préparation reste distinct et permet un nouvel essai avec des limites explicitement relevées ; le chargement ordinaire accepte aussi 40000 relocations valides au-delà du budget d’analyse par défaut.

`FrameOffsets.*`, `NativeStackSpecialization.*` et `OriginalBinaryUndefinedIndependence.*` vérifient tous les résidus des alignements 2/4/8/16/32, les bits hauts libres, les sauvegardes entre appels, les boucles de décompte, les corruptions par alias, les sélections erronées, les grands masques inutiles, les élargissements nécessaires et les budgets exacts ou réduits d’une unité. Des contrôles natifs distincts couvrent l’alignement conditionnel, le nettoyage interne non signé, le nettoyage erroné et les retours préfixés. Ces tests ne prouvent pas la couverture automatique natif-vers-LLVM des boucles partitionnées.

Les régressions couvrent les deux ordres d’octets, les bases de cadre hautes, les champs écrasés ou superposés, les arêtes tardives, les prédécesseurs élargis, CALL/RET natifs et les budgets exacts ou insuffisants d’une unité. Les deux voies C exécutent les quatre cas mémoire à O0/O2. Les contrôles natifs fixent séparément deux valeurs du sélecteur ; ce n’est pas une preuve sans contrainte d’entrée. Un exemple LLVM indépendant vérifie qu’un bras faux déplacé avant la jonction commune ne s’exécute pas après le bras vrai, copies PHI et écritures comprises.

Les régressions couvrent les partitions plus fines, tous les résidus admis, différents bits hauts, un dernier cas défaillant et les budgets exacts ou insuffisants d’une unité. Les deux sorties C sont exécutées en O0/O2 avec des adresses invitées rejetées inaccessibles et des drapeaux invalides : le statut 2 doit préserver chaque octet d’état. Les tests du modèle et C/Python vérifient aussi la disposition v5, la propriété des chaînes et les extensions anciennes ou futures. Les preuves natives refusent les domaines non liés.

`StringTransfer.*` et les régressions de copie répétée vérifient recouvrement, compteur nul, isolation des temporaires, limites de capacité et de budget, et invalidation des pointeurs. `MachineStringSourceTests.cpp` compare l’exécution native et les deux voies C à O0/O2 à un oracle indépendant couvrant tous les registres, indicateurs et octets de pile, pour les quatre tailles et les deux sens.

`ControlDiscovery.*` et `NativeStackSpecialization.*` couvrent les conditions sur les bits bas de la racine, les dépendances aux bits hauts et à la racine entière, les parcours incomplets, les budgets de parcours exacts et insuffisants, ainsi que la conservation des preuves d’adresses immuables finies.

Les tests des gardes devant une cible non résolue couvrent une phase invalide inaccessible, une cible inconnue accessible dès l’entrée ou après une arête de retour, les limites exactes de raffinement et des budgets de découverte adjacents réussis ou épuisés. Un refus ne publie ni code résiduel, ni origines, ni témoins de lecture. Le travail facultatif après récupération complète ne définit pas un budget minimal requis.

Les régressions de projection de cadre à la demande couvrent les registres automatiques ou existants, les racines élevées et le rebouclage modulaire, les gardes limitées à l’octet bas, les faits contradictoires ou absents sur les arêtes sœurs, les budgets de requêtes adjacents et le résultat unknown du solveur. Une demande étroite ne transforme pas une preuve partielle de pointeur en preuve complète ; un refus ne publie ni code résiduel ni témoins.

Les régressions des sous-champs finis couvrent un sélecteur dans chaque moitié, des données arbitraires observables, les registres et la trame, les deux ordres d’octets, les champs étroits, l’extension ultérieure du domaine, les masques disjoints et les faits manquants. Une cible accessible absente, un sélecteur libre, un budget partagé épuisé ou un résultat unknown du solveur ne doivent publier ni code résiduel ni témoin. Les tests couvrent aussi la découverte automatique avec une demande sur le mot entier, les indications manuelles inutilisées, les données dérivées de la racine et les fenêtres constantes de tête.

Les tests des budgets publics couvrent les limites par défaut et maximales sur 32 bits, l’épuisement indépendant des évaluations et de la découverte, les deux ABI source et moteurs C, ainsi que les valeurs CLI mal formées. Les contrôles de disposition C/Python couvrent v7, la validation héritée et les extensions futures ignorées par v1–v6. Un refus de budget ne publie ni source ni témoins ; le compteur d’évaluations ne doit pas boucler à sa valeur maximale.

Les tests de coupure optionnelle comparent boucles simples et imbriquées avec un même budget d’opérations, exécutent les boucles résiduelles, distinguent les modes de décodage et vérifient que le chaînage nul reste inchangé. Un contre-exemple de corrélation doit réussir par défaut et refuser la publication avec l’option. Appels natifs répétés, cases de retour, rejeu des dépendances et prédécesseurs tardifs utilisent les deux réglages. Les tests publics couvrent les deux ABI et backends, les indicateurs par défaut ou inconnus, les anciennes API ignorant l’extension et le booléen rapporté.

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer` vérifie les jonctions conditionnelles de pointeurs en registres et dans les emplacements du cadre, les deux ordres des octets, le rebouclage modulaire et les adresses racines hautes, la restauration de la pile et 120 octets du cadre. Les contre-exemples associés refusent les pointeurs corrompus et vérifient les budgets exacts ou réduits d’une unité pour les requêtes, opérations, évaluations et raffinements, ainsi que l’épuisement de la découverte et des contextes.

Les tests de l’observateur couvrent le refus anticipé, les constantes, les projections vides et la dernière requête UNSAT. Les régressions de lecture immuable conservent les lectures à l’exécution après un contre-exemple, revalident les domaines en cache pour une autre étendue de lecture et refusent les certificats mal formés sans publier de preuves partielles.

Les tests de contrôle affine couvrent les conditions sur les bits bas avec un budget de huit requêtes, les registres et emplacements de cadre dans les deux ordres d’octets, le bouclage modulaire, les octets observés du cadre et la restauration de la pile. Une condition contradictoire non littérale doit éliminer une branche non prise en charge ; deux racines ayant les mêmes 32 bits bas mais des bits hauts différents doivent conserver leurs deux destinations indirectes.

`FiniteQueryCache.*` vérifie les limites de stockage exactes et inférieures d’une unité, la récence actualisée par les accès réussis, l’éviction de plusieurs enregistrements de tailles différentes, les évictions répétées et les requêtes avec variables renommées. Les doublons, absences, résultats mal formés et candidats trop volumineux préservent la récence. Les copies de preuves déjà renvoyées restent valides après l’éviction de leurs entrées.

`ControlStateRecovery.*Marginal*` couvre des domaines indépendants de cibles et de données métier dont le produit dépasse la limite conjointe, les résultats concrets du programme résiduel, un prédécesseur tardif ajoutant une cible après élargissement, les cibles accessibles manquantes et l’abandon intégral des domaines trop grands ou incomplètement énumérés. Ces fixtures originales vérifient la reprise de l’analyse après modification d’un domaine et l’absence de publication de graphes partiels en cas d’échec. Une variante sur un emplacement de pile vérifie l’invalidation par alias après élargissement, dans les deux ordres d’octets.

`InterpreterTransferChain.*` vérifie les valeurs corrélées, les branches uniques ou dynamiques, les prédécesseurs tardifs, les bornes de pile, le refus des alias et les budgets. Les tests CALL/RET natifs supplémentaires vérifient les occurrences répétées, les octets de retour et la restauration de pile. Le rejeu des producteurs et les contrôles natif-vers-LLVM couvrent les contrats incompatibles, les justificatifs modifiés, les résultats erronés et les écritures de pile manquantes.

`NeverDX86NoIndexAddressTests` vérifie l’adressage SIB x86 sans index sur 32 et 64 bits : bits d’échelle ignorés, largeur de destination, lectures/écritures, métadonnées complètes des sorties indéfinies, offsets de segment et provenance des adresses. Il refuse les pseudo-registres comme base ou index de largeur incorrecte et conserve les vrais index R12 sélectionnés par REX.X. Les tests EVEX de diffusion et de déplacement masqué couvrent aussi ces formes, la suppression des accès mémoire inactifs et les métadonnées SIB incohérentes.

Les régressions de décalage couvrent tous les comptes bruts de huit bits, les combinaisons de flags à compte nul, les deux modes x86, toutes les largeurs, les alias CL, AH/CH/DH/BH, les registres étendus et la mémoire. L’exécution symbolique par octet est comparée à une arithmétique répétée bit par bit. Les tests relationnels vérifient copies et nouveaux flags, sauvegardes, boucles, comptes issus de valeurs indéfinies, branches, formes invalides, empreintes et budgets. Les lectures finies couvrent 1/2/4/8 octets, sélection selon l’entrée, ensembles unitaires par chemin, témoins complets et limites, et refusent les candidats dépendants, absents, modifiables, non adossés au fichier, relocalisés ou non bornés.

Les tests du cœur vérifient la séparation des contextes, les jonctions au point fixe, les boucles dynamiques, les registres superposés, l’invalidation des alias, la distribution finie et le refus sans remplacement partiel. Les tests de source assemblent des machines x64 originales à registres, à pile et à adresses finies ; ils incluent des champs de contrôle liés et un oracle natif indépendant SysV/Win64. Les deux parcours C sont compilés en O0/O2 avec pièges de comportement indéfini et comparés à un oracle non signé pour les calculs, écritures mémoire et sentinelles de sortie. Les cas négatifs vérifient les certificats manquants et les budgets insuffisants. La CLI publique et ses rapports vérifient les contrôles, budgets, compteurs et refus. Clang multicible et LLD sont requis ; l’exécution de l’ELF original exige aussi un hôte Linux x64. Un outil absent ou un hôte incompatible signifie une couverture ignorée, et non un succès.

`ControlStateRecovery.LongTransparentLoop*` couvre une boucle indépendante de 20 phases, des oracles arithmétiques dynamiques, le refus des sélecteurs inconnus et l’épuisement des budgets. `LongTransparentPhasesKeepExactBitDemands` vérifie que les bits sans rapport dans l’octet du sélecteur restent des données d’exécution observables sans devenir des demandes de contrôle. `ProducerClosureChargesWorkBeforeAnotherRestart` vérifie que la découverte arrière et la réexécution consomment les budgets communs avant le démarrage d’un nouveau graphe, sans publication partielle.

`X86ShiftCarry.*` compare la retenue des décalages arithmétiques à droite étroits,
les comptes masqués et les destinations et suppressions de drapeaux APX à des
décalages successifs d’un bit. `NarrowArithmeticShiftCarrySurvivesBothSourceBackends`
exécute les deux sorties C à O0/O2 avec détection des comportements indéfinis,
pour toutes les valeurs d’octet et tous les comptes bruts.
`NeverDLLVMCIntrinsicSemanticTests` exécute aussi les min/max entiers signés et
non signés i1/8/16/32/64/128 à O0/O2 : résultats affectés ou intégrés, ordre des
producteurs et évaluation unique. Les largeurs scalaires non prises en charge
et les opérandes mal formés doivent échouer explicitement.

## Vérification du contrôle et des appels en C structuré

`HighControlFlowSemantics.*` vérifie que déplacer une sortie ou une fin de boucle conserve les étiquettes visées par d’autres sauts. Les entrées directes dans les sorties en tête ou en fin de boucle et le remplacement de break sont exécutés en C généré à O0/O2, avec des valeurs de retour attendues indépendantes.

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` vérifie que les arguments de registre requis conservent leurs positions inconnues finales. Évaluer un argument requis ou une condition inconnue doit provoquer un piège explicite ; les opérandes omis, nuls ou imbriqués ne doivent pas devenir silencieusement zéro. Les valeurs connues et les opérandes supplémentaires dont l’absence de lecture est prouvée restent exécutables. Un piège marque une limite de diagnostic, pas une preuve d’équivalence du comportement reconstruit.

## Tests d’exécution CPU

`NeverDIntegerABITests` compile des fixtures Clang originales pour Windows x64, Linux x64 et Linux ARM64. Des fonctions réelles à dix arguments vérifient registres, pile et frames d’appel. La matrice Unicorn/KVM/WHP marque explicitement les couples hôte/ISA indisponibles comme ignorés ; un skip n’est pas un succès. `NeverDExecutionBudgetTests` contrôle budgets partagés de continuation, réservations et échéance absolue sans attente dépendante du temps.

`NeverDCPUEmulationTests` couvre instructions ARM64, contrôle, chargements, contextes, alias, invalidation de cache et boucles bornées ; le profil logiciel exécute aussi FP/SIMD et TLS. `NeverDUserExecutionTests` vérifie droits CPL3/EL0, alias, défauts de protection, contextes et changement d’espace. `NeverDServiceRequestTests` prouve que SYSCALL/SVC est intercepté avant le transport et que l’état reste intact jusqu’à consommation unique ; cela établit un protocole de transfert, pas un modèle complet de services OS. `NeverDExecutionConfigurationTests` contrôle la résolution partagée, distingue support du build et sonde live, et rejette les options non prises en charge. Les tests publics SDK/CLI ne requièrent pas le modèle Windows. `NeverDThreadPointerTests` vérifie FS-base, `TPIDR_EL0`, restauration de contexte et permissions. `NeverDKvmCancellationTests` utilise un guest x64 non terminant pour vérifier l’interruption KVM active, la reprise et l’état inchangé des signaux ; il est explicitement ignoré sans KVM.

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

L’absence de matériel ARM64 ou d’hyperviseur est une couverture native omise, pas une réussite. Unicorn et la compilation croisée ne prouvent pas l’exécution native KVM/WHP.

## Tests du profil de processus Linux

Les [suites indépendantes de processus](process-emulation.md) compilent de vraies fixtures ELF x64/AArch64. `NeverDLinuxProcessTests` vérifie démarrage, politique des en-têtes, requêtes de service, sortie binaire, défauts et limites. `NeverDProcessPublicTests` contrôle l’API C/CLI sans modifier l’image d’analyse. `NeverDExecutionSessionTests` couvre deux CPU partageant mémoire/budget et la consommation exactement unique des requêtes/défauts. `NeverDX64MemoryUpdateTests` contrôle arithmétique mémoire, SETcc, BT, XMM/MXCSR, observateurs d’écriture, frontières REP et lectures préparées de périphériques. `DriverBackendParityTests.cpp` exécute les fixtures WDK originales et relocalisées puis compare le rapport observable complet à Unicorn ; images/backends absents sont ignorés.

Le x64 vérifié admet aussi les formes masquées historiques `SS`, `SD`, `PS`, `PD` de `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`. `X64SSEInstructions.def` centralise les largeurs, alignements et règles d’admission. `MaskedSSEArithmeticMatchesIndependentHostExecution` compare les formes registre/RAM à un oracle CPU hôte indépendant : quatre arrondis, FTZ, zéros signés, subnormaux et NaN. `SSEMemoryObserverStopsBeforeResultAndStatusChanges` vérifie l’arrêt avant les effets. DAZ, exceptions non masquées, x87 et AVX restent exclus.

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

Les backends indisponibles sont explicitement ignorés. Compilation croisée et Unicorn ARM64 ne prouvent pas KVM/WHP ARM64 natif.

## Vérifications de l’émulation des pilotes

Activez `NEVERD_ENABLE_DRIVER_EMULATION=ON` avec `BUILD_TESTING=ON` pour compiler
la suite d’exécution ciblée et les vérifications de l’API C/CLI via la
bibliothèque partagée :

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

Les fixtures couvrent l’initialisation invitée, les retours de réussite et
d’échec, les comportements non pris en charge, les fautes mémoire, l’analyse
stricte des scénarios, l’exécution bornée, les E/S synchrones bufferisées et
directes, READ/WRITE, les durées de vie indépendantes des fichiers, les permissions
MDL, les exports dynamiques, les arguments variables invités et les défauts CPU
structurés via create, transferts, cleanup, close et unload. Utilisez la
[CLI `emulate-driver`](driver-emulation.md) pour vérifier le JSON et les codes
de sortie des processus. Les builds de production peuvent activer cette
fonctionnalité avec `BUILD_TESTING=OFF` ; `libneverd` ne doit pas exiger une
configuration Unicorn réservée aux tests.

Les tests supplémentaires couvrent les MDL de pool non paginé appartenant au pilote, les durées de vie indépendantes du descripteur et du tampon, les structures de requête du registre et les tampons courts, les droits des handles, les suppressions et les fuites, ainsi que les 64 bits de `information_hex` pour les IOCTL sans sortie. La validation externe couvre aussi les lectures/écritures directes synchrones et les statistiques de Zero.

Les tests vérifient les contextes CPU complets (registres, flags, SIMD, FPU, CR8), la mémoire partagée et le rejet des contextes étrangers ou en faute. Les fixtures compilées `driver_dispatcher.c` exécutent de vrais DPC et callbacks de travail, les échéances, les événements/timers de notification et synchronisation, les attentes non alertables `KernelMode` de raison `Executive`, délais, piles bloquées multiples, réveils conservés après set/reset, arguments et erreurs IRQL/durée de vie. Les tests de travail conservent la couverture pending/achèvement, files, blocages et budgets communs. Ils prouvent le sous-ensemble décrit, pas tout l’asynchronisme Windows.

`driver_context_limits.c`: Les plafonds IRQL proviennent de `KernelAPIIRQL.def` ; le modèle propriétaire vérifie les restrictions dépendant des arguments. Un DPC ne peut ni appeler le registre ni allouer, libérer ou accéder au pool paginé. Les conversions Unicode de `DbgPrint` exigent `PASSIVE_LEVEL`, tandis que l’ANSI et les opérations non paginées pris en charge restent utilisables à `DISPATCH_LEVEL`. Les piles sont bornées : un pointeur de pile sortant ne peut atteindre celle d’un autre worker bloqué. Un timer armé dans l’extension empêche la destruction prématurée du périphérique. Cela n’expose pas les changements généraux d’IRQL.

`KernelDeviceStackTests.cpp` vérifie les graphes distincts de propriété/attachement, le sommet, l’atomicité des échecs, la capacité, les champs opaques, les handles, la rétention des travaux/requêtes lors du détachement ou de la suppression et les identités fichier/dispatch. Le fixture original `driver_wdm_stack.c` utilise les vrais en-têtes WDK et les aides inline Copy/Skip/SetCompletion ; les chemins facultatifs `NEVERD_WDM_STACK_FIXTURE`/`NEVERD_WDM_STACK_CFG_FIXTURE` sélectionnent les images normales/CFG actif. `DriverWDMStackTests.cpp` couvre relocalisation, statut inférieur, ordre/indicateurs de fin, propagation pending différée, workers/DPC, attentes, `STATUS_MORE_PROCESSING_REQUIRED`, rétention des MDL directs, fin imbriquée et curseurs/contrôles malformés. `DriverScenarioPublicTests.cpp` couvre le transfert C API/CLI et les fins retenues/imbriquées en C API, avec les images CFG configurées. Les artefacts absents sont ignorés explicitement. Ces preuves Linux concernent seulement la pile d’un même pilote, pas PDO/PnP/alimentation. `KernelIRPStackTests.cpp` vérifie les curseurs comptés, les préfixes Copy inline complets, les emplacements effacés, la propagation statut/pending, MPR et fin imbriquée, les propriétaires de continuation et les routes retenues. Les véritables READ/WRITE et cycles de fichiers utilisent aussi Copy inline.

`DriverPnpScenarioTests.cpp` vérifie la concordance stricte JSON/validation native, les données initiales explicites, les limites d’identifiants et de nombre, les combinaisons de champs interdites, les statuts finaux du bus et les rapports observés acceptant null. `KernelPnpDeviceTests.cpp`, `KernelPnpRequestTests.cpp` et `KernelPnpCompletionTests.cpp` couvrent la propriété du fournisseur, les succès/échecs/fuites d’AddDevice, les IRP initiaux, l’admission des fichiers, le retour arrière du cycle de vie, la fin différée, les continuations MPR/imbriquées/en attente et l’atomicité des échecs. Le fixture original `driver_wdm_pnp.c`, compilé avec le véritable WDK, utilise les chemins facultatifs `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE`. `DriverWDMPnpTests.cpp` exécute des images normales et avec CFG actif après relocalisation : AddDevice, E/S de fichier, suppression ordonnée, démarrage/suppression différés, échecs de démarrage/query et échecs d’AddDevice nettoyés ou avec fuite. `DriverScenarioPublicTests.cpp` couvre aussi un scénario PnP différé de sept requêtes via C API et CLI, avec les images CFG configurées. Les artefacts manquants sont explicitement ignorés. Les preuves d’exécution proviennent uniquement de Linux et établissent seulement le sous-ensemble PnP sans ressources documenté.

Les tests du schéma V9 vérifient la lecture et la restitution des huit noms de fonctions mineures et partagent la validation du statut final avec la fin du cycle de vie ; QueryStop 0x119 est rejeté avant le chargement de l’image. Les vérifications étendues du modèle et du fixture authentique couvrent le retour arrière après query-stop, cancel-stop, l’arrêt/redémarrage, la suppression inattendue, les violations des contrats de succès exact, les E/S logicielles à l’arrêt ou en attente de suppression, le rejet par l’invité après suppression inattendue, l’identité des périphériques et les résultats mixtes d’AddDevice. `DriverScenarioPublicTests.cpp` exécute une séquence de 16 requêtes d’arrêt/redémarrage/suppression inattendue via C API et CLI avec des fixtures normales/CFG actif, en conservant les octets de l’IOCTL logiciel réussi, l’IOCTL en échec renvoyé par l’invité après suppression inattendue et les opérations finales cleanup/close/remove. L’exécution publique est séquentielle : un IRP retenu sans source de fin actuellement disponible ne peut attendre une requête ultérieure du scénario qui démarre ou nettoie le périphérique. Les contraintes de vidage avant Remove sont des limites du profil, pas une politique générale d’admission des E/S sous Windows. Les preuves restent limitées à Linux.

`DriverPowerScenarioTests.cpp` vérifie les données strictes des paquets d’alimentation, la concordance JSON/native, le contexte opaque de 32 bits, les limites des FIFO de réponses et les rapports indépendants des enfants. `KernelPowerRequestTests.cpp` et `KernelPowerCompletionTests.cpp` contrôlent la disposition réelle des paquets, les indicateurs de route, la distinction entre cycle de vie et notification par objet, la correspondance FIFO, la propriété du callback terminal, MPR, les attentes et les limites de libération. Le fixture original `driver_wdm_power.c`, compilé avec le véritable WDK, utilise les chemins facultatifs `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE`. `DriverWDMPowerTests.cpp` couvre la relocalisation normale/avec CFG actif, Query/Set directs et imbriqués, les fins indépendantes différées, l’ordre S0 avant D0, les instantanés des callbacks à cinq arguments conservés pendant les attentes, les enfants issus de workers, les callbacks null, le refus de query, les valeurs initiales/FIFO indépendantes par PDO et les échecs explicites dus aux données manquantes. `DriverScenarioPublicTests.cpp` ajoute la validation préalable des paquets d’alimentation malformés et une séquence de veille/reprise avec six requêtes de scénario et trois enfants via C API et CLI. Les véritables artefacts manquants sont explicitement ignorés ; les preuves d’exécution restent limitées à Linux et établissent uniquement le sous-ensemble d’alimentation paginable sans ressources documenté.

`KernelUsbIdleTests.cpp` vérifie propriété, D2, emprunt et première cause ; `KernelUsbIdleBridgeTests.cpp` / `KernelUsbIdleReceiptTests.cpp` les IRP réels, annulations, capacité composite et ordre réception/fin ; `DriverUsbIdleScenarioTests.cpp` les entrées/rapports. `DriverWdmUsbIdleTests.cpp` utilise le vrai `driver_wdm_usb_idle.c` via `NEVERD_WDM_USB_IDLE_FIXTURE` / `NEVERD_WDM_USB_IDLE_CFG_FIXTURE` pour idle, D0/D3, annulation, réveil, réarmement/redémarrage, fonctions indépendantes/composites et routes PDO/FDO. `DriverWdmUsbIdlePublicTests.cpp` exécute le [scénario USB](../examples/driver-wdm-usb-idle-scenario.json) en C API/CLI, normal/active-CFG et bases préférées/relocalisées. Absence explicitement ignorée ; preuves Linux uniquement, sans preuve de support USB KMDF.

`KernelFrameworkUsbIdleTests.cpp`, `KernelFrameworkUsbIdleStorageTests.cpp` et `KernelFrameworkUsbIdleBridgeTests.cpp` vérifient stratégie, stockage et ordonnanceur typé. Le vrai `driver_kmdf_usb_idle.c` n’émet aucun IRP idle lui-même. `DriverKMDFUsbIdleTests.cpp` couvre absence de permission, E/S gérées avec D2/D0 différés, StopIdle avant/pendant callback, échec arm, Maximum explicite, réveil et composite. `DriverKMDFUsbIdlePublicTests.cpp` exécute le [scénario KMDF USB](../examples/driver-kmdf-usb-idle-scenario.json) avec `NEVERD_KMDF_USB_IDLE_FIXTURE` / `NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`, C API/CLI, normal/active-CFG et bases préférées/relocalisées. Absence explicitement ignorée ; preuves Linux uniquement. Les tests du modèle vérifient qu’un épuisement des allocations après un armement réussi exécute le vrai rappel de désarmement et annule WAIT_WAKE sans consommer de réponse D2.

`DriverKMDFUsbPoFxTests.cpp` couvre SystemManaged/WithHint initial, deux autorisations, annulation en D0, D2/D0 retardés et acquittement F0 par vrai worker, StopIdle, réveil avant READ, échec arm, retrait et redémarrage. `DriverKMDFUsbPoFxPublicTests.cpp` exécute le [scénario USB PoFx](../examples/driver-kmdf-usb-pofx-scenario.json) via C API/CLI, deux modes, normal/active-CFG et adresses préférée/rebasée. `DriverKMDFUsbIdleTests.cpp` conserve les régressions de transfert et vérifie READ direct après D0Entry. `KernelFrameworkRequestTests.cpp` couvre associations, propriété caller-context, files manuelles/arrêtées et IRQL. L’épuisement mémoire et les deux barrières sont testés dans le pont modèle, pas dans le pilote réel. Artefacts absents ignorés explicitement ; preuves d’exécution Linux seulement. `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` couvre aussi l’échec D0Entry réel en RemovePending : acquittement Required exact et quiescence permettent le nettoyage sans F0/ActiveCondition, sans prétendre gérer tout échec SET_POWER ni la suppression surprise autonome.

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: Le [scénario WAIT_WAKE natif](../examples/driver-wdm-wait-wake-scenario.json) exécute le vrai fixture WDK `driver_wdm_wait_wake.c` via `NEVERD_WDM_WAIT_WAKE_FIXTURE` / `NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE`. Il couvre émission dans START, arguments, réveil sans D0 implicite, réarmement, annulation, MPR, annulation DPC puis D0 par worker, captures exactes et fournisseurs indépendants. Les preuves normales/active-CFG aux bases préférées/relocalisées restent Linux uniquement ; les fichiers absents sont explicitement ignorés.

`KernelPowerCompletionTests.cpp` couvre APC/DPC, refus atomique de capacité puis reprise, fournisseur seul synchrone/différé avec/sans callback, route retenue et MPR. Le vrai `DriverWdmWaitWakeTests.cpp` vérifie D0 direct depuis l’annulation DPC, Query/Set APC/DPC, IRQL/CR8 inchangés, retour avant exécution PASSIVE et refus de WAIT_WAKE élevé. Le [scénario élevé](../examples/driver-wdm-elevated-power-scenario.json) passe par `DriverWdmWaitWakePublicTests.cpp`, C API/CLI, images normales/active-CFG et bases préférées/relocalisées ; preuves Linux uniquement.

`KernelRemoveLocksTests.cpp` vérifie l’indépendance des identités de verrou et de périphérique, les Tags NULL ou répétés, les tailles retail/DBG exactes, le vidage immédiat et différé, les obligations après un acquire échoué, l’atomicité des erreurs, la capacité et le retrait du stockage. `KernelRemoveLockBridgeTests.cpp` et `DriverWDMRemoveLockTests.cpp` couvrent l’initialisation avant attachement, le stockage opaque dans l’extension, les limites IRQL, la libération après retrait du paquet, la fin du fournisseur après le vidage du verrou, la disponibilité dès la dernière libération avant le retour du callback, les workers en attente et le nettoyage d’un échec AddDevice. Le fixture original `driver_wdm_remove_lock.c`, compilé avec le véritable WDK, existe en variantes retail/DBG et normale/avec CFG actif via les chemins facultatifs `NEVERD_WDM_REMOVE_LOCK_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` et `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`. Les tests publics C API/CLI utilisent le schéma PnP existant et conservent des observations distinctes de réception du bus, de fin du bus et de démontage final. Les artefacts absents sont explicitement ignorés ; les preuves d’exécution restent limitées à Linux et n’établissent ni Driver Verifier complet ni le vidage général de requêtes simultanées.

`DriverResourceScenarioTests.cpp` vérifie les faits JSON/natifs explicites, largeurs entières, nombres, chevauchements physiques/de registres, alignement, ID, banques vides et sérialisation. `KernelMMIOTests.cpp`, `KernelMMIOFailureTests.cpp`, `KernelResourceBridgeTests.cpp` et `UnicornMMIOTests.cpp` couvrent propriété des banques/mappages, alias, générations, durée des listes compactes, chronologie du fournisseur, persistance au redémarrage, accès après surprise/alimentation, transactions CPU/API exactes et atomicité des échecs. Le fixture WDK original `driver_wdm_resources.c` utilise `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`. `DriverWDMResourceTests.cpp` exécute les vrais accesseurs scalaires et REP, le rebasage normal/CFG actif, les alias de sous-plages, les mappages de fin de page, STOP/redémarrage et les accès invalides. C API/CLI refusent les faits invalides avant chargement et exécutent le même scénario de 14 requêtes avec sorties IOCTL persistantes et nombres exacts de map/unmap. Le fichier partagé [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) exige le protocole registres/IOCTL de ce fixture. Les artefacts absents sont explicitement ignorés et les preuves se limitent à Linux ; aucune mémoire physique hôte ni backend général de périphérique n’est exercé.

`DriverDMAScenarioTests.cpp` valide capacités explicites, domaines logiques, limites d’octets/nombre/temps, directions strictes et séparation configuration/observations. `KernelPhysicalMemoryTests.cpp` et `BackendBackingTests.cpp` vérifient les frontières d’allocations partageant une page, les références de maintien, les permissions CPU inchangées, l’exclusion MMIO/réentrance et l’atomicité des échecs de validation sur toute une plage. `KernelRequestMDLTests.cpp` vérifie les alias de descripteurs construits contre les mêmes identités physiques. `KernelDMATests.cpp`, `KernelDMABridgeTests.cpp` et `SchedulerDMATests.cpp` exercent les octets RAM réels, appels de table liés à l’adaptateur, propriété immédiate/FIFO, durées de vie séparées des callbacks/mappages, fragments de pages, directions erronées, contrôle préalable de libération, domaines PDO indépendants et échecs d’époque/alimentation. Le pilote WDK original `driver_wdm_dma.c` utilise `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`. `DriverWDMDMATests.cpp` et les tests C API/CLI exécutent les vrais pointeurs d’adaptateur, le stockage commun/SG et les événements DMA/interruption configurés séparément. Le fichier partagé [driver-dma-scenario.json](../examples/driver-dma-scenario.json) exige le protocole de cette fixture. Les artefacts absents entraînent un saut explicite ; les preuves Linux ne démontrent ni DMA hôte réel, ni PCI, ni moteur général de périphérique. `pluginsdk/python/tests/test_driver_dma_integration.py` exerce la liaison JSON existante avec gestion de propriété via `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_FIXTURE` et `NEVERD_TEST_WDM_DMA_CFG_FIXTURE`, notamment les octets, l’ordre des callbacks et les échecs rapportés.

`KernelSEHTests.cpp` vérifie les plans purs de déroulement, l’ordre des portées, la restauration des GPR non volatils, les piles bornées et les métadonnées explicitement non prises en charge. `KernelExceptionTests.cpp` vérifie l’arité API exacte, les statuts sur 32 bits bas, les exceptions typées, les limites IRQL et l’absence de modification de l’état modèle/CPU. Le véritable WDK `/GS-` `driver_wdm_seh.c` utilise les options `NEVERD_WDM_SEH_FIXTURE` / `NEVERD_WDM_SEH_CFG_FIXTURE`. `DriverWDMSEHTests.cpp` exécute des images normales, CFG actif et rebasées avec levées directes ou depuis une fonction auxiliaire, handlers imbriqués, nouvelles levées, filtres réels, finally pendant le déroulement, ordre de recherche, enregistrements stables, continuation des défauts CPU pris en charge et restauration complète du CPU. Les filtres imbriqués et les finally en collision utilisent des piles logiques liées ; les autres défauts CPU restent explicitement rejetés. L’API C/CLI exécute [driver-seh-scenario.json](../examples/driver-seh-scenario.json) et vérifie les résultats API null avec les véritables messages du handler invité. `pluginsdk/python/tests/test_driver_seh_integration.py` utilise `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_SEH_FIXTURE` et `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`. Les images externes absentes sont explicitement ignorées ; les preuves restent limitées à Linux et n’établissent pas la prise en charge des tampons utilisateur ni du SEH général.

`KernelDMAChannelTests.cpp`, `KernelDMAChannelBridgeTests.cpp` et les tests partagés `SchedulerDMATests.cpp` vérifient la FIFO d’allocations mixtes, les largeurs de retour des callbacks, les prévalidations pures d’admission/libération, la réutilisation des registres, les fragments de pages contigus, le vidage de l’opération entière, les captures CurrentIrp et les durées de vie paquet/MDL/périphérique. Le code original `driver_wdm_dma_channel.c`, compilé avec le véritable WDK, utilise les options `NEVERD_WDM_DMA_CHANNEL_FIXTURE` / `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`. `DriverWDMDMAChannelTests.cpp` exécute des pilotes normaux, avec CFG actif et rebasés, avec de vrais appels MapTransfer et FlushAdapterBuffers, un quota commun tampons/SG/canaux, des transactions explicites, un achèvement IRQ/DPC, des opérations successives, deux PDO et des cas d’échec. L’API C/CLI exécute les sept requêtes de [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json), dont une transaction unique couvrant les deux fragments mappés. `pluginsdk/python/tests/test_driver_dma_channel_integration.py` utilise `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` et `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` pour la même interface JSON publique. Les artefacts absents sont explicitement ignorés ; les preuves Linux n’établissent pas la prise en charge des contrôleurs DMA système ni de formes arbitraires de mappage/vidage HAL.

`DriverInterruptScenarioTests.cpp` couvre les descripteurs bruts/traduits explicites, les affectations mixtes ou uniquement d’interruptions, la validation stricte des champs/nombres d’événements, l’identité source et les observations BOOLEAN indépendantes. `KernelInterruptsTests.cpp`, `KernelInterruptBridgeTests.cpp` et `SchedulerInterruptTests.cpp` couvrent la correspondance exacte des tuples exclusifs, les jetons opaques, la capture de génération/connexion, la durée des événements, les seuls champs Ex sélectionnés, le verrou commun et la restauration IRQL, la propriété des callbacks, la priorité ISR à échéance identique et les échecs de capacité sans mutation. `KernelFrameworkRequestTests.cpp` vérifie les prévisions pures d’annulation et la capacité des jetons par lot, sans publier d’appels ni consommer de références. Le fixture original compilé avec le vrai WDK `driver_wdm_interrupts.c` utilise `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`. `DriverWDMInterruptTests.cpp` teste le rebasage normal/CFG actif, l’ancienne ABI à onze arguments, les versions Ex 1/2/4, l’achèvement réel ISR→DPC, FALSE dans AL, synchronisation/verrous manuels, PDO indépendants, générations au redémarrage et faits matériels invalides. Les tests C API/CLI refusent les déclarations invalides avant chargement et exécutent les sept requêtes de [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json), en vérifiant les octets de l’IOCTL en attente et les observations distinctes de livraison. Les images absentes sont explicitement ignorées ; les preuves restent limitées à Linux et n’établissent ni interruptions partagées/de niveau/MSI ni préemption d’instruction.

`DriverGuardTests.cpp` et quatre variantes originales de `driver_guard.c` couvrent CFG actif/inactif, le changement de base de chargement, l’ABI check/dispatch et les cibles malformées. `KernelFrameworkTests.cpp`, `KernelFrameworkControlTests.cpp`, `KernelFrameworkQueueTests.cpp` et `KernelFrameworkRequestTests.cpp` couvrent les liaisons, la création transactionnelle des périphériques, le routage des files, les longueurs logiques des tampons ainsi que l’ordre de nettoyage et les durées de vie des IRP et des contextes. Les fichiers originaux `driver_kmdf_lifecycle.c` et `driver_kmdf_control.c` se compilent facultativement avec les véritables en-têtes WDK 1.33 et se lient via la bibliothèque réelle `FxDriverEntry`. Définissez les chemins du cache CMake `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` pour les images de cycle de vie et `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` pour les images de périphérique de contrôle normales et avec CFG actif. Les artefacts externes manquants sont explicitement ignorés. `DriverKMDFLifecycleTests.cpp`, `DriverKMDFControlTests.cpp` et les cas C API/CLI de `DriverScenarioPublicTests.cpp` couvrent les callbacks réels, les E/S tamponnées et directes, l’achèvement différé par éléments de travail, les statuts d’échec, le déchargement et l’exécution CFG après changement de base. Les preuves restent limitées à Linux et n’établissent pas une prise en charge complète de KMDF ni de PnP ou de la gestion de l’alimentation.

Les tests de l’ancienne API d’annulation maintiennent la continuation de l’API pendant annulation, nettoyage imbriqué et destruction finale ; Ex conserve son retour d’annulation sans callback pour une requête déjà annulée. `KernelFrameworkRequestAccessorTests.cpp` et `KernelRequestMDLTests.cpp` vérifient Information partagé sur 64 bits, validation de longueur à l’achèvement, identité file/IRP d’origine, handles fichier WDF NULL, résultats sur handle conservé, cache MDL tamponné et ByteCount de première direction, identité directe et mappage différé, invalidation finale et refus de contournement WDM. Les modes L, M, D et C du fixture de contrôle réel exécutent ces chemins dans les images normales/CFG actif : ancienne API d’annulation, MDL/informations tamponnés, MDL READ/WRITE directs et accès après achèvement.

Les tests d’annulation couvrent les échéances virtuelles réservées aux transferts et les champs de rapport, l’achèvement prioritaire, les requêtes déjà annulées, les résultats du marquage/retrait, le droit d’achever selon la mise en file ou la livraison, les attentes et les références internes. Les tests du scheduler vérifient séparément l’ordre DPC/annulation/travail, la capacité, la séparation des identités et la suspension/reprise. L’annulation WDM reste une erreur explicite du modèle.


## Organisation des tests

`add_neverd_unittest` crée un exécutable GoogleTest et attribue à chaque cas
découvert un label CTest identique au nom de cette cible.

| Zone source | Cible et label CTest | Couverture |
|-------------|----------------------|------------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Invocation de sous-processus multiplateforme, quoting, redirections et codes de sortie |
| `unittests/libc` | `NeverDLibCTests` | Noms libc connus et classification |
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Catalogue de puits, priorité d’identité, préfiltre d’arguments, chasse de débordement de copie, audit de durée de vie du tas et matrice obligatoire de six cellules PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/lift` | `NeverDLiftTests` | Formes LowIR decoder/lifter, étapes IR, loaders, relocations, fixtures de format, décompilation et patch représentatif |
| La plupart de `unittests/semantic` | `NeverDSemanticTests` | Sémantique différentielle des instructions, ABI, contrôle, expressions C et lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadata hardfork, normalisation, ambiguïtés ABI/signature, CFG/SSA/récupération, frontières decoder exhaustives et inputs hostiles, faits proxy/call, sémantique interpréteur, différentiels LLVM/C/Solidity et API publique |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | Métadonnées v0-v4 et dispositions ELF, comportement strict du verifier/loader, 23 artefacts ELF épinglés, oracle officiel indépendant, disponibilité exhaustive des opcodes, entrées hostiles, CFG/récupération et différences exécutées LLVM/C/Rust |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Équivalence réécriture/obfuscation sur quatre ISA et trois formats objet |
| Fichiers de transformation ciblés dans `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sondes rapides à relier séparées du gros binaire sémantique |
| `unittests/corpus` (sous-module) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests`, `NeverDAdaDEHCorpusTests` | Métadonnées d’exceptions et d’exécution lues dans 545 binaires réels épinglés, chacun accompagné d’un manifeste énonçant les planchers que sa récupération doit franchir |

Les références d’enregistrement sont
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt) et
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt) et
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt) et
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt).

### Le corpus binaire épinglé

Chaque autre suite construit ce qu’elle teste ; le corpus, non : c’est un
sous-module de binaires produits par de vraies chaînes d’outils, sur des hôtes
et pour des cibles que ce dépôt ne peut pas atteindre. Chacun est épinglé par
empreinte, et le manifeste voisin énonce les planchers que sa récupération doit
franchir. C’est le seul endroit où une affirmation sur ce que NeverD lit dans,
disons, un objet partagé `armv7` compilé en `-O2` et dépouillé trouve une
réponse plutôt qu’un débat.

Les suites ne sont construites que si l’étape de configuration a reçu l’ordre
de les chercher : ce drapeau est donc tout ce qui les maintient sous test.

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` exécute toutes les lignes ;
`check-neverd-windows-eh-corpus`, `check-neverd-rust-eh-corpus`,
`check-neverd-go-eh-corpus`, `check-neverd-cxx-itanium-eh-corpus`,
`check-neverd-objc-eh-corpus` et `check-neverd-ada-d-eh-corpus` en exécutent une chacune. Les trois hôtes de CI
configurent avec le drapeau et passent les six lignes : les octets sont
identiques partout, mais ce qui les lit ne l’est pas, et un passage du corpus
sur un hôte ne prouve rien sur les deux autres.
`scripts/audit_ci_test_inventory.py` refuse un inventaire auquel manque l’un des
six labels, car une compilation qui a cessé sans bruit de lire le corpus est
une régression qu’aucun test ne peut attraper — le test est justement ce qui a
disparu.

L’audit live des opcodes EVM s’exécute ainsi :

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

En local comme en CI, le chemin standard impose
`git fetch --depth=1 --force` sur l’URL officielle
`https://github.com/ethereum/go-ethereum.git` et ne teste que le SHA exact
fraîchement obtenu depuis le `HEAD` distant de la branche par défaut, dans un
worktree detached. Chaque exécution emploie un dépôt bare privé, temporaire et au nom imprévisible,
conserve l’authority ref du fetch et son SHA exact pendant la vie du worktree
detached, puis détruit les deux. Il n’existe ni dépôt Git persistant partagé ni
cache. `local_docs`, un checkout existant et un submodule ne sont jamais des
chemins d’audit, car un pin de submodule serait précisément périmé au moment de
détecter une dérive live.

Chaque commande Git efface d’abord tous les `GIT_*` hérités, dont
`GIT_CONFIG_*`, puis n’installe que les valeurs auditées. `GIT_CONFIG_NOSYSTEM`
et `GIT_CONFIG_GLOBAL` désactivent les configurations système/globale ;
`GIT_ATTR_NOSYSTEM` et `core.attributesFile` au niveau commande désactivent les
attributs système/globaux, tandis que `core.hooksPath` désactive les hooks. Une
configuration inattendue du dépôt privé, des grafts,
`objects/info/alternates` ou `refs/replace` font échouer la validation ;
`GIT_NO_REPLACE_OBJECTS` désactive le replacement lookup.

La sonde reflète tous
les booléens exportés de `params.Rules`, appelle
`LookupInstructionSet(params.Rules)` et parcourt les 256 slots.
`EVMUpstreamOpcodePolicy.def` possède alias et exclusions typées historiques/EOF
non planifiées ; `EVMUpstreamSemanticsPolicy.def` possède l’inventaire Rules
fermé, les mappings de forks, les exceptions base-stack et les familles
dynamic-immediate.

CI n’exécute cet audit live que pour les push vers `dev`, les pull requests, le
déclenchement manuel et le planning quotidien. La sonde Go appelle l’API publique
`LookupInstructionSet(params.Rules)` pour chaque fork mappé.
La CLI publique n’expose que `--manifest-output` ; le manifest fermé utilise
`schema 3` et ne permet pas de choisir source, ref, checkout ou toolchain.
`EVMUpstreamOpcodePolicy.def` porte les alias de noms et exclusions historiques/
EOF non planifiées revues ; l’orthogonal `EVMUpstreamSemanticsPolicy.def` porte
les règles de fork et exceptions de sémantique de pile. Le manifest fermé vérifie
la révision exacte, l’activation, byte/name, `base_min_stack` et
`net_stack_delta`, et rejette les champs, forks, noms ou bytes inconnus ou
dupliqués. L’allocation dépend uniquement de `operation.undefined` ; `HasCost`
n’est qu’un contrôle croisé du coût puisqu’il vaut aussi false pour une opération
définie de coût nul. Chaque slot `defined && !HasCost` doit correspondre
exactement à `EVM_GETH_ACTIVE_WITHOUT_COST` depuis son fork déclaré. Un slot
undefined avec coût, un slot defined non revu ou la perte du marqueur provoquent
un échec fermé. Les déclarations manquantes, hors plage ou non consommées
syntaxiquement échouent aussi : chaque `.def parser` rejette une policy
`partial`. Un échec CI publie révision exacte, manifest et journal comme
artifact. Le parser et les diagnostics ont une couverture unitaire Python indépendante :

`EVMUpstreamSemanticsPolicy.def` attribue chaque champ booléen exporté de
`params.Rules` à un unique `EVM_GETH_RULE_FIELD` : `MappedForkSelector`,
`NoOpcodeAllocation` ou `ExcludedSelectorExpectedError`. Le probe active chaque
champ isolément via `LookupInstructionSet` : les deux premières catégories
exigent nil error, la troisième error, et toute empreinte opcode/stack complète
des 256 slots doit égaler `ExpectedFork`. `IsEIP155`, `IsEIP2929`, `IsEIP4762`
et `IsPetersburg` sont les champs sans allocation donnant Frontier ; `IsUBT`
doit échouer et donner Cancun.

`EVMUpstreamSemanticsPolicy.def` déclare les familles dynamiques EIP-8024, les
types d’opération et les deltas de pile valides ;
`EVMEIP8024Immediates.def` possède séparément le decode des immediates et classe
les 256 bytes single/pair. Avec `go -overlay`, l’audit obtient les vrais handlers
privés `operation.execute` et parcourt les `canonical fork jump tables` ainsi
que les `mainnet active/scheduled jump tables`, table par table. Une famille
`inactive` est consignée, une famille `partial` est rejetée. Chaque table active
teste `DUPN`, `SWAPN` et `EXCHANGE` sur tous les immediates (`3x256`) et les
`3 missing-operand cases`, face aux mêmes sources déclaratives.

`EVM_HARDFORK_LATEST` a une seule cible canonique. Le
`EVMUpstreamForkAliases.def` fermé mappe Prague→Pectra, Osaka et BPO1–BPO5→Fusaka,
et Paris/Shanghai/Cancun/Amsterdam/Bogota vers eux-mêmes ; tout nom inconnu
échoue fermé. Un `audit_unix_time` consigné pilote
`MainnetChainConfig.LatestFork(time)` (doit égaler NeverD latest) et le contrôle
alias/probe de `LatestFork(max uint64)` ; les deux instruction sets sont
intégralement comparés. Le manifest fixe `authority=official-fresh-fetch`, URL
officielle, `HEAD` demandé et SHA. Le probe emploie `GOTOOLCHAIN=local`.

La sonde Go et le contrôleur Python imposent des
`input/collection/string hard limits` ; toute entrée, collection ou chaîne
surdimensionnée échoue de façon fermée. Pour `bounded diagnostic output`, un
affichage trop long inclut le `digest` complet et un
`explicit truncated marker`. Sortie bornée et échéance commune s’appliquent à
chaque enfant ; un dépassement tue tout le `process group`/process tree et draine
les pipes.

Le reçu schema 3 actuel consigne `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, la révision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, `Go 1.24.0` local,
`stack_limit=1024` et `diagnostics=[]`. Il couvre `21 fork tables` et
`20 Rules probes` avec `15 mapped/4 no-op/1 expected-error`. Les deux entrées
`mainnet active/scheduled` indiquent `upstream BPO2`, mappé de façon fermée vers
`NeverD Fusaka`. Sur `23 table targets`, seuls `Amsterdam/Bogota` sont actifs :
`1536 candidate executions` et `6 missing-operand cases`. Les
`three handler symbols` concordent sur les deux cibles actives. L’audit Python
passe `67/67`, tout comme `C++ Opcode 10/10`. Le run macOS réel a réussi sous
`sandbox-exec`, le `go run` final hors ligne ; Linux exige `bubblewrap`.

Toutes les étapes Go — `go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` et `go run` — passent par le sandbox filesystem
`capability-root`. Il ne lit que le probe privé, geth fraîchement récupéré, le
`resolved GOROOT` validé et les racines runtime système exactes nécessaires, et
n’écrit que dans les racines d’environnement isolées. Le réseau n’est accordé
qu’aux étapes de dépendances nécessaires ; le run final reste hors ligne. Les
tests exigent le refus des sentinels du `host HOME/workspace` et l’absence de
leur contenu dans les sorties. Linux teste la même politique `bubblewrap` sans
`/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Les onze cibles de test EVM actuellement enregistrées par CMake sont :

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` épuise tous les inputs de deux octets à chaque
fork modifiant le decoder, compare le décodage complet et les frontières
`JUMPDEST` exactes, puis soumet à tous les forks des inputs hostiles déterministes
de longueur bornée.

Pour une modification du contrôle de flux EVM, exécutez d’abord le contrat de
point fixe et de domaine des hauteurs :

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Ces cas couvrent les retours internes entre blocs, les fusions finies à plusieurs
cibles, la convergence, l’ordre déterministe des arêtes, les lanes de pile
complète sensibles au chemin, la conservation des corrélations, les sauts
inconnus, les cibles exactement invalides, les budgets fail-loud et les fautes
strictes/relâchées. `MayReachable` ne garde qu’un candidat de CFG et ne produit
pas de fait certain. Exécutez ensuite les onze cibles EVM et l’audit live upstream.

Pour les modifications de dataflow MedIR/HighIR, exécutez aussi les contrats de
phi constant, selector, opérande typé, graphe mal formé et chaîne profonde :

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

Ces cas prouvent les phi cycliques égaux et contradictoires, les expressions de
selector non adjacentes et inter-blocs, les deux ordres d’opérandes d’égalité,
les contrôles exacts de largeur ABI, les opérandes typés storage/event/calldata,
le traitement déterministe d’un MedIR mal formé et un parcours itératif de
16 384 valeurs productrices.

## Production des fixtures

### Fixtures de lift et de format

`unittests/lift/CMakeLists.txt` compile les sources C et assembleur vers
plusieurs cibles pendant le build. Les triples Clang produisent des objets ELF
x86-64, i386, AArch64 et ARM32, des objets et images liées PE/COFF, ainsi que des
objets Mach-O i386 PIC/non-PIC. Avec LLD, certains objets sont aussi liés en
exécutables pour les tests de patch. `NeverDLiftTests` dépend de la cible
`lift-test-objects` ; une compilation normale de ce binaire rafraîchit donc les
fixtures générées.

La plupart des tests lift utilisent `NeverDLiftFixture.h` pour invoquer le CLI
`neverd` construit et inspecter LowIR, MedIR, HighIR, LLVM IR, le C généré ou un
binaire réécrit. La variable d’environnement `NEVERD` peut remplacer le chemin
du CLI lors d’une expérience manuelle ciblée ; les exécutions CTest ordinaires
utilisent l’exécutable intégré par CMake.

### Fixtures de sûreté mémoire

`unittests/safety/fixtures/binaries` contient des images PE, ELF et Mach-O
versionnées pour x86-64 et AArch64, accompagnées du PDB ou du dSYM que fournit
chaque format et d’un MAP d’éditeur de liens pour chaque image. Le MAP est ce
qu’une compilation dépouillée livre encore, aussi chaque cellule est-elle
également analysée en nommant le MAP explicitement, ce qui fige ce qu’un
résultat a le droit d’affirmer lorsqu’il ne reste ni types ni lignes source.
`NeverDSafetyIntegrationTests` exécute les six cellules sur chaque hôte ; la
configuration échoue si une image ou un fichier compagnon requis manque, et la
suite n’a aucun chemin de contournement lié à la chaîne d’outils de l’hôte.

Les binaires équivalents proviennent d’un seul fichier source. Reconstruisez la
fixture smoke native de l’hôte avec `make`, ou régénérez la matrice complète
versionnée avec :

```bash
make -C unittests/safety/fixtures matrix
```

La recette de la matrice exige les cibles croisées Linux et Windows de Clang,
les outils COFF de LLD, les deux architectures Darwin et `dsymutil`. Ses chemins
de débogage sont remappés et l’enregistrement de la ligne de commande CodeView
est désactivé, afin que les compagnons versionnés ne capturent pas le chemin
absolu de l’espace de travail d’un développeur.

### Reconstruction des exceptions Windows

Les modifications des exceptions Windows fondées sur des tables exigent à la
fois des tests de représentation et un test de patch sur un PE lié. Le filtre
de lift ciblé couvre le modèle normalisé unwind/SEH/C++, les entrées corrompues,
les arêtes exceptionnelles du CFG, HighIR, la génération LLVM WinEH, le
remplacement du répertoire d’exceptions et la reconstruction Guard CF/EH
continuation :

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

La fixture assembleur x64 protégée nécessite la cible Windows de Clang et
`lld-link` ; son édition de liens CMake utilise `/guard:cf` et `/guard:ehcont`.
Un skip dû à l’absence du cross-linker ne prouve pas le chemin final-image. Un
test d’intégration réussi démontre que le PE réécrit peut être rechargé et que
ses tables runtime-function, unwind, load-config, Guard CF et Guard EH
continuation restent triées, présentes dans le fichier et limitées à des cibles
exécutables.

La fixture FH3 liée couvre séparément la fermeture C++ native : tables d’état
fixes, annotations HighC, conservation de la personality, cibles catch générées
et graphe IP-to-state rechargé.

Voir [Reconstruction des exceptions Windows](windows-exception-reconstruction.md)
pour la matrice de support analyse/native et le contrat de patch fail-closed.

### Modèles d'exceptions par langage

Tout ce qui n'est pas le modèle tabulaire Windows tient dans une cible ciblée.
`NeverDLanguageEHTests` couvre la chaîne de frames DWARF, la zone de données
spécifique au langage d'Itanium, ARM EHABI, le compact unwind de Darwin, les
métadonnées de frame du runtime Go, la machinerie de panique de Rust et les
trois runtimes Objective-C :

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

Les tables de cette suite sont assemblées octet par octet plutôt que compilées,
car la plupart des combinaisons visées ne sont émises conjointement par aucune
chaîne d'outils. Objective-C en est le cas le plus net : les trois runtimes
émettent une LSDA Itanium et ne diffèrent que par le contenu d'un emplacement de
la table de types — et cette différence est totale, non graduelle. L'emplacement
d'Apple adresse un `objc_typeinfo` dont les deux premiers champs imitent
délibérément `std::type_info` ; celui d'Objective-C++ de GNUstep adresse une
véritable sous-classe de `std::type_info` ; et celui du runtime GNU n'est même
pas un pointeur, mais la chaîne du nom de classe elle-même. Appliquer la
convention d'un runtime à la table d'un autre n'échoue pas : cela rapporte un
nom de classe lu au milieu d'autre chose. C'est pourquoi le runtime est établi à
partir de la personality de la frame avant qu'un seul emplacement ne soit lu.

La même suite fige deux distinctions faciles à confondre et fausses une fois
confondues. `@catch(id)` et `@catch(...)` sont des gestionnaires différents — le
premier prend n'importe quel objet Objective-C et laisse une exception étrangère
poursuivre sa route — et chaque runtime les écrit différemment ; un décodeur qui
rapporte les deux comme un catch-all pose un gestionnaire sur des exceptions qui
seraient en fait passées à côté. Et une table de sites d'appel setjmp/longjmp
indexe des sites d'appel et non des adresses : un lecteur qui ne reconnaît pas
l'une des personalities SJLJ n'échoue pas, il invente des plages protégées et
des landing pads que le programme n'a jamais nommés.

Reconnaître cette forme n'est pas la refuser. Une entrée SJLJ est une paire de
valeurs ULEB128 — un sélecteur de dispatch et un décalage d'action — et ce
décalage y signifie exactement ce qu'il signifie dans la forme adressée : la
chaîne d'actions, les types rattrapés et les spécifications d'exception se
lisent donc tous dans une table qui ne nomme aucun code. Seule la région que
garde chaque entrée reste inconnue, car ce sont les écritures que la fonction
fait elle-même dans son emplacement de call-site qui l'énoncent, et non quoi
que ce soit dans la table. La suite fixe aussi l'octet auquel il ne faut pas se
fier ici : GCC écrit `DW_EH_PE_uleb128` comme encodage de call-site et LLVM
écrit `DW_EH_PE_udata4`, tous deux émettent ensuite de l'ULEB128 quoi qu'il
arrive, et aucune personality ne le lit jamais — un décodeur ne le doit donc
pas non plus.

L'identité de la personality est fixée en même temps, car c'est elle qui décide
comment se lit chacune des tables ci-dessus. GNAT nomme sa routine des trois
façons dont GCC nomme celle de chaque frontal — `_v0`, `_sj0`, `_seh0` — et,
sous Windows, enregistre un symbole tout en renvoyant vers un autre : les
quatre graphies doivent donc aboutir à Ada. D en est l'image inversée : trois
compilateurs, trois noms pour une seule routine, un seul jeu de tables derrière
eux.

### Allers-retours différentiels Unicorn

La fixture sémantique teste le comportement plutôt que la forme textuelle :

1. Écrire un petit cas C/assembleur ou construire du LLVM IR.
2. Le compiler avec Clang/LLVM pour la cible demandée.
3. Exécuter le code machine original dans Unicorn et capturer le retour attendu ou un autre état défini par la fixture.
4. Le charger et le lifter dans NeverD, émettre LLVM IR puis recompiler le résultat en code machine.
5. Exécuter le code régénéré avec les mêmes ABI, entrées, disposition mémoire et modèle CPU.
6. Comparer les résultats observables.

L’implémentation principale est
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h).
La fixture patch-full utilise `Codegen::compileForRewrite`, le même backend de
réécriture que les opérations patch, puis compare le code de référence et
transformé sur toute la grille ISA/format 4×3.

Un échec sémantique déterministe de NeverD doit faire échouer le test. Réservez
les skips aux frontières explicites de capacité externe et lisez leur raison :
un résumé vert sans cross-linker ne prouve pas que le parcours du format a été
exécuté.

### Backends différentiels EVM

Les tests interpréteur fournissent un oracle déterministe 256 bits. La suite
emitter compile et exécute LLVM, abaisse C23 avec Clang vers le même host harness
et, si `solc`, `anvil`, `cast` et `jq` sont présents, déploie le Solidity généré
localement. Elle compare status, storage et compteurs de trace. Un corpus raw
séparé exécute ALU pré-Fusaka, copies calldata/memory, `MCOPY` superposé, Keccak
et return data dans l’EVM native d’Anvil.

Les tests Low/Med préservent les execution lanes whole-stack sensibles au chemin
et l’identité de lane des phi ; l’épuisement d’un budget, notamment
`MaxAbstractInstructionTransfers`, est une erreur dure. Strict ne rejette un
opcode inconnu ou inactif que sur une lane prouvée `Reachable`, tandis que
`MayReachable` ne produit aucun fait certain. Les parcours selector, receive et
fallback de HighIR sont contraints à la racine et à un terminal réussi. Un
selector partagé n’est pas une preuve indépendante de standard : une
`KnownFunctionVariantInfo` propre au standard et une forme de retour exacte
commune à tous les terminaux réussis sont nécessaires pour choisir variante et
liste de retours.

L’interpréteur effectue le preflight typé de la pile avant tout effet propre à
l’opcode. `EVMForkSemantics.def` définit l’octet `0x44` comme `DIFFICULTY` avant
Paris et `PREVRANDAO` à partir de Paris. `REVERT`, faults, step limit et
épuisement des ressources restaurent l’état transactionnel. Un échec d’allocation
est `ExecutionFaultKind::ResourceExhausted` ; si le snapshot d’entrée lui-même
ne peut être créé, `HasPersistentStateSnapshot` vaut false et aucun commit n’est
possible.

### Régressions des frontières publiques et budgets EVM

Les tests d’API publique altèrent séparément les
`Code`/`Fork`/`Instructions`/`JumpDestinations` canoniques et chaque table,
range, ID, lane ou référence d’arête LowIR. `execute` doit renvoyer
`llvm::Error` avant le lookup d’instruction ; `lowerToMedIR` doit rejeter tout
LowIR mal formé ou hors budget avant indexation ou allocation proportionnelle à
l’entrée. Pour `lowerToMedIR`, les tests imposent validation des options,
ressources et structure avant un `canonical decode replay` champ par champ et
avant `lowerCanonicalLowToMedIR`. La récupération HighIR publique replay-vérifie
les LowIR/MedIR externes ; seul `analyze` utilise `lowerCanonicalLowToMedIR` et
`recoverCanonicalHighIR` sur son IR canonique sans replay récursif ou dupliqué,
mais avec tous les HighIR option/resource budgets. L’interpréteur teste ensuite la frontière exacte et +1 de toutes les
limites de `EVMInterpreterLimits.def` : `MaxSteps` garde son `StepLimit` dédié ;
l’épuisement de `MaxMemoryBytes`, `MaxTraceEntries`, `MaxLogEntries`, de
l’agrégat `MaxLogDataBytes` ou de `MaxPersistentStateEntries` au runtime renvoie
`ResourceExhausted` et restaure les effets transactionnels. Un agrégat initial
`MaxHostReturnDataBytes` ou un état persistant trop grand est une erreur d’API.
`MaxCalldataBytes`, l’agrégat `MaxHostEnvironmentEntries` sur `BlockHashes`,
`Balances`, `CodeHashes`, `ExternalCode`, `BlobHashes` et l’agrégat
`MaxExternalCodeBytes` sont aussi des erreurs d’API. Le
`const execute preflight` les rejette avant copie d’environment, snapshot ou
result. Les vues return-data `ArrayRef` et le lookup `lower_bound` sur table triée sont
aussi couverts sans copie de buffer ni map de PC.

Des tests LowIR séparés couvrent les limites agrégées de diagnostic
`MaxLowDiagnostics` et `MaxLowDiagnosticBytes` : decode linéaire et construction
CFG préfacturent nombre/octets finaux exacts et rejettent zéro.
Les tests de sûreté HighIR couvrent le domaine trié par lane
`Any/Exact/Excluded`, le match/l’exclusion d’égalité, le match de l’arête false
et le mismatch de l’arête true d’un `XOR(selector, constant)` brut, le
raffinement de word nul/calldata size/call value et les conditions unknown
fail-closed. Leurs tests frontière exacte et -1 couvrent, depuis
`EVMAnalysisLimits.def`, `MaxHighDispatchCandidates`, l’agrégat
`MaxHighRecoveredArguments`, `MaxHighDiagnostics`, `MaxHighDiagnosticBytes`,
`MaxHighReferenceVisits`, `MaxHighMemoryTransferCells` et
`MaxHighMemoryValueVisits`. Tout diagnostic émis, y compris le diagnostic fixe
de malformation, doit facturer nombre et octets finaux avant allocation.
Les budgets de diagnostic LowIR et HighIR sont testés séparément ; la région CFG
racine par défaut doit facturer `MaxHighRegionBlockReferences` avant reserve ou
copie des PC de blocs.
Les régressions de scope de fonction couvrent les back-jumps `EQ` et `raw XOR`
vers le dispatcher partagé. Elles vérifient qu’une autre fonction ne contamine
ni `arguments`, ni `mutability`, ni `return shape`, ni `region`, tout en gardant
les bodies partagés et tail calls atteignables.
Les résultats externes CALL/CREATE sont testés comme outcomes hôte non
déterministes sur les deux arêtes CFG précises, ce qui préserve la récupération
du fallback ERC-1167. Une condition selector illisible reste Unknown et ne peut
inventer de faits fallback ou function.

Les tests CFG dérivent `InvalidJumpDestination` de `EVMLowFaultKinds.def` pour
un `end-of-code JUMPI` : true certain vers une cible invalide n’a aucune fin
réussie et donne un fault certain ; false certain réussit ; unknown garde le
chemin false potentiellement réussi sans marquer toute la lane en fault certain.

Les tests ABI appliquent à la limite exacte et +1 les frontières grammaticales
de `EVMABIParserLimits.def` et les frontières cardinalité/texte des tables
publiques de `EVMABITableLimits.def`. Ils rejettent aussi les enums
kind/standard/evidence invalides, metadata incohérente, signatures/returns non
canoniques, selectors partagés marqués independent par erreur, variantes
pendantes ou dupliquées, et un event-topic `APInt` de mauvaise largeur avant le
lookup selector indexé ou le lookup topic trié.

`NeverDEVMOpcodeTests` impose aussi l’architecture metadata : chaque opcode assigné
fait un roundtrip encoding/valeur typée ; limites de familles, alias hardfork et
maxima stack/host dérivés sont vérifiés.

### Backends différentiels Solana SBF

Les tests de métadonnées SBF valident chaque fonctionnalité de version, les frontières de collision d’opcodes, les hash syscall Murmur3, les relocations et les constantes de machine ELF, de registre et d’adresse VM. Les fixtures du loader génèrent, sans binaire incorporé, les dispositions historiques à sections v0-v2 et les dispositions strictes v3/v4 sans section, fondées sur les program headers.

`NeverDSBFISAConformanceTests` vérifie chaque encodage d’octet pour chaque
version v0-v4 face à un manifeste typé audité indépendamment.
`NeverDSBFExternalOracleTests` compare ensuite les décisions d’activation et
de frontière avec un processus Anza officiel construit séparément.
`NeverDSBFUpstreamConformanceTests` attribue un résultat explicite aux 23 ELF
à la révision Anza épinglée.

`NeverDSBFSemanticTests` exécute directement les octets d’instruction vérifiés et ne consomme pas le MedIR : modifier ou corrompre l’IR normalisé ne peut donc pas faire coïncider accidentellement l’oracle source avec un backend. Il couvre la sémantique v2 non monotone, la mémoire, les syscalls, les frames d’appels internes, les fautes, les traces et les limites de ressources. Les modules LLVM sont vérifiés ; le C généré est compilé avec les avertissements traités comme erreurs, et Rust avec `-D warnings`. Les tests de l’API publique parcourent tous les niveaux IR, le désassemblage, le CFG, les métadonnées, LLVM, C et Rust depuis un ELF SBF strict généré.

## Cibles en une commande

Les cibles personnalisées construisent leurs dépendances puis exécutent CTest
avec un parallélisme dérivé des CPU de l’hôte :

| Cible CMake | Sélection |
|-------------|-----------|
| `check-neverd` | Tous les tests enregistrés |
| `check-neverd-semantic` | `NeverDSemanticTests` uniquement |
| `check-neverd-sbf` | Toutes les cibles/tous les cas `NeverDSBF*Tests` |
| `check-neverd-patch-full` | `NeverDPatchFullTests` uniquement |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests` uniquement |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests` uniquement |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests` uniquement |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` et `NeverDAvxUpperXformTests` n’ont actuellement pas
de cible pratique `check-neverd-*`. Construisez-les et sélectionnez leur label
comme ci-dessous. `check-neverd-semantic` n’inclut pas non plus les binaires de
transformation ou patch-full séparés ; utilisez `check-neverd` pour l’agrégat
complet.

## Flux CTest incrémental

Construisez d’abord l’exécutable propriétaire, puis sélectionnez son label. Vous
évitez ainsi de relier de grandes cibles sémantiques sans rapport.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# Toutes les cibles/tous les cas EVM ciblés
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Toutes les cibles/tous les cas Solana SBF ciblés
cmake --build build-release --target check-neverd-sbf --parallel 4
```

Utilisez un nom CTest dérivé de GoogleTest pour une seule régression :

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Sélecteurs utiles :

| Commande | Rôle |
|----------|------|
| `ctest --test-dir build-release -N` | Lister les cas découverts sans les exécuter |
| `ctest --test-dir build-release -L '<regex>'` | Sélectionner un label de binaire de test |
| `ctest --test-dir build-release -R '<regex>'` | Sélectionner des noms de cas |
| `ctest --test-dir build-release --output-on-failure` | Afficher les diagnostics uniquement en cas d’échec |
| `ctest --test-dir build-release --stop-on-failure` | Arrêter au premier échec |
| `ctest --test-dir build-release --parallel 4` | Exécuter jusqu’à quatre cas en parallèle |

La découverte GoogleTest utilise `DISCOVERY_MODE PRE_TEST` ; le binaire
correspondant doit donc exister avant l’énumération par CTest. Les timeouts par
cas et de découverte séparés sont définis dans `cmake/AddNeverD.cmake` et ne
doivent être élargis que pour des suites dont les cas lourds ont été mesurés.

## Quels tests changent avec le code ?

| Zone modifiée | Commencer par | Puis envisager |
|---------------|---------------|----------------|
| Lifter d’architecture ou decode | Cas nommé dans `NeverDLiftTests` | Aller-retour sémantique de l’ISA correspondant |
| CFG LowIR, découverte de fonctions, tables de saut | Cas lift CFG/switch | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests` ou `NeverDTwoTableXformTests` |
| MedIR, ABI, flags, types, SSA | Cas lift MedIR/convention d’appel | Cas `NeverDSemanticTests` multi-ISA |
| HighIR ou C structuré | Cas HighIR/decompile | `NeverDCFGLoopXformTests` et compilation du C généré |
| Loader PE/ELF/Mach-O ou relocation d’entrée | Fixture de format correspondante dans `unittests/lift` | Test de chargement/décompilation toutes étapes de la cellule |
| Codegen de réécriture ou relocation de sortie | Cas `RewriteCodegenRTTests` | `NeverDPatchFullTests` et fixture patch liée si disponible |
| Transformation LLVM IR utilisée par patch | Binaire de transformation ciblé | Grille de passes composées `NeverDPatchFullTests` |
| C API ou CLI | Test SDK/query direct et `unittests/semantic/CLIEndToEndTests.cpp` | Suite pipeline/format pertinente |
| Loader, opcode, IR ou backend EVM | Plus petite cible propriétaire `NeverDEVM*Tests` | Toutes les cibles EVM et compilation du C/Solidity généré |
| Loader, ISA, IR ou backend SBF | Plus petite cible propriétaire `NeverDSBF*Tests` | Toutes les cibles SBF et compilation du C/Rust généré |
| Reconnaissance libc | `NeverDLibCTests` | Cas sémantiques call/ABI si le comportement change |
| Audit de durée de vie du tas ou chasse de débordement de copie | `NeverDSafetyTests` | Les six cellules de `NeverDSafetyIntegrationTests` |
| Exécution ou quoting de processus | `NeverDTestProcessTests` | Un cas CLI/sémantique affecté sur chaque hôte pris en charge |

Les tests doivent exprimer le contrat à la frontière stable la plus basse. Un
test de forme LowIR est utile pour attribuer le lifter ; un aller-retour
sémantique est nécessaire si deux formes IR plausibles peuvent se comporter
différemment. Évitez les dumps de fonction complets quand une petite assertion
opcode, CFG ou d’état observable suffit.

## Relation avec la CI

La CI construit en Release avec les tests activés sur Linux, macOS et Windows,
puis audite l’inventaire découvert avant d’appliquer les exclusions de labels
propres à la plateforme. Les profils sont définis dans
`.github/workflows/ci.yml` et `scripts/audit_ci_test_inventory.py`.
`NeverDSafetyTests` et `NeverDSafetyIntegrationTests` sont exigés sur chaque
hôte de la matrice ; chaque exécution lit les mêmes fixtures PE, ELF et Mach-O
versionnées pour x86-64 et AArch64. Comme aucun shard de la matrice ne représente
toutes les suites coûteuses, un `check-neverd` local reste le signal pré-fusion
complet le plus clair si la machine possède tous les outils croisés nécessaires.

## Profil actuel de conformité et de sanitizers Solana SBF

Cette liste actuelle remplace la liste SBF abrégée ci-dessus. La suite source
differentielle exige `rustc` en plus de clang ; un compiler skip signifie une
couverture absente. L’agrégat complet comprend `NeverDSBFProgramImageTests`,
`NeverDSBFMalformedCorpusTests`, `NeverDSBFISAConformanceTests`,
`NeverDSBFUpstreamConformanceTests`, `NeverDSBFLLVMDifferentialTests` et
`NeverDSBFSourceDifferentialTests`, ainsi que les targets metadata, loader,
analyzer, semantic, emitter et integration. Le profil intégré enregistre les
targets nommées et leurs résultats, pas un total rapidement variable.

Le profil sanitizer se construit séparément dans `build-sbf-asan-ubsan`. Le
package prebuilt épinglé par révision contient le header fork-only requis ;
l’integration tourne donc dans le même profil ASan/UBSan fail-fast.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

### Snapshot de preuve SBF épinglé (2026-08-24)

La gate épingle Anza `sbpf` sur
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave sur
`ef210d67f2fabeee1730498188fa78854260c679` et le SDK Solana sur
`122f32e571ce39face4beffaccea733e37c207fd`. Le manifest ELF officiel réussit
23/23 ; `NeverDSBFExternalOracleTests` confronte 1,411 cas opcode/boundary via
`SBFOfficialOracleProtocol.def`, `SBFOfficialVerifierCases.def` et
`SBFOfficialExecutionConstants.def`.
`SBFOfficialELFMutations.def` est le contrat tabulé des ELF malformés ; son
total variable n’est pas figé.
Séparément, le `41-case strict ELF differential` exécute toute la matrice
strict-v3 via `verify-elf-batch` officiel et NeverD ; ses 41 cas ne font pas
partie du total 1,411.

La matrice d’exécution officielle supplémentaire reste séparée : exactement 508
cas actifs `(Version,Opcode)` plus 58 cas de frontière donnent 566 cas
d’exécution exacte. Elle ne remplace pas les 1,411 probes du verifier ni le
`41-case strict ELF differential`, et n’entre dans aucun de ces totaux.
`NeverDSBFAgaveConformanceTests` authentifie Firedancer test-vectors
`68bb4af40235562e8852fa23d5727e49c2a0b862` et confronte les 1,955 `sol_compat_elf_loader_v1` fixtures du
loader (1,399 acceptées, 556 rejetées). Pour chaque ELF accepté, elle compare
`entry_pc`, `text_off`, `text_cnt`, `rodata_hash` et `calldests_hash`. Cette gate n’exécute pas le verifier
d’instructions ultérieur.
La Linux Release CI utilise `--print-pinned-revision`,
`--print-test-vectors-revision` et `--print-toolchain`, puis exporte
`NEVERD_SBPF_ORACLE` et `NEVERD_AGAVE_CONFORMANCE_ROOT`, rendant les deux gates
externes obligatoires. Localement, sans environnement oracle/corpus explicite,
les cas sont découverts mais peuvent skip.

`SBF_RUNTIME_VERSION` rend `RuntimeVersionPolicy::ChainProfile` dépendant du
cluster/slot historique : les feature accounts officielles font progresser
l’ISA maximal de V0 à V1, V2 puis V3 ; il reste aujourd’hui V3. v4 explicite
utilise `RuntimeVersionPolicy::UpstreamToolchain` pour
l’analyse offline. La limite actuelle de 10 MiB vaut exactement `10'485'760`
octets ; 65,536 n’est qu’une provenance/test historique. `SBFFaultCodes.def`
stabilise les valeurs de fault d’exécution et `SBFSourceStatuses.def` possède
séparément l’ABI du source généré.

Les fixtures à l’échelle 10,000 protègent worklist, function ownership et
multi-latch sans figer un temps machine. Les lignes cluster/account/slot
permettent un `RPC activation audit`, les tests ordinaires restant déterministes
et offline.

## Performances de l’inventaire des classes Android

Compilez `NeverDMobileTests` en Release et exécutez son label avant de mesurer
`neverd mobile INPUT --list-classes`. Les tests du lecteur couvrent les
métadonnées éparses, Unicode, les références invalides, les corps de méthodes
non pris en charge, les sommes de contrôle et les budgets ; les tests d’archive
distinguent l’extraction complète des requêtes sur charges utiles sélectionnées.
Les tests CLI vérifient le filtrage par préfixe, le périmètre JSON, la préservation
des sorties et l’atomicité des échecs multidex.

Le générateur et banc de mesure indépendants valident l’inventaire complet des
descripteurs de chaque processus avant d’accepter une mesure de temps :

```sh
python3 -m unittest scripts.tests.test_benchmark_mobile_inventory -v
python3 scripts/benchmark_mobile_inventory.py \
  --output-dir /tmp/neverd-inventory-benchmark \
  --class-count 6000 --dex-count 3 --code-units 64 \
  --extra-string-bytes 8388608 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Utilisez un nouveau répertoire de sortie à chaque exécution. `--generate-only`
écrit les jeux d’essai et leur manifeste sans chronométrage. `--workload`
sélectionne des types d’entrée communs pour une éventuelle commande
`--peer-command 'tool {input} {prefix}'` ; la préparation des entrées est hors
chronométrage. Les rapports conservent les empreintes, les commandes, tous les
échantillons issus de nouveaux processus, les hypothèses de cache chaud et,
sous Linux avec GNU time, le RSS maximal des processus enfants. Ce RSS n’est
pas le pic cumulé d’un outil multiprocessus. Les APK synthétiques sont des
conteneurs de requête, pas des applications installables. La vitesse de
l’inventaire ne prouve ni celle des recherches de références ni la qualité de
la reconstruction Java.

Sur les processeurs hybrides, fixez le banc et ses enfants à un même CPU autorisé
(par exemple `taskset -c 4 python3 ...` sous Linux) pour éviter de mélanger les
cœurs de performance et d’efficacité. Le rapport consigne l’affinité CPU héritée.

## Performances des références de code Android

Les requêtes de références partagent les frontières d’instructions et la
validation du code du lecteur de reconstruction. Exécutez la suite mobile
après toute modification de cette frontière. Les tests du lecteur couvrent les
types de pools d’opérandes, les modes de recherche, l’appartenance des méthodes
et le code partagé, les faux semblants dans les charges utiles/valeurs
immédiates, les entrées malformées et les limites de ressources. Les flux de
débogage partagés sont vérifiés avec le cadre, l’étendue et les paramètres de
chaque corps propriétaire. Conservez les grands inventaires de membres et les
corps riches en branchements dans la couverture des limites de stockage ; les
index persistants et la croissance temporaire des conteneurs ont des durées de
vie différentes.
Vérifiez aussi les éléments réordonnés ou se chevauchant, le code partagé entre
prototypes incompatibles de même largeur, le stockage d’entrée non aligné et
les sous-chaînes traversant les frontières des blocs de recherche. Les
optimisations des données privées du décodeur doivent préserver les modèles
complets de reconstruction possédant leurs données et les multiensembles
d’occurrences de références, y compris les échecs sur les métadonnées de
reconstruction non prises en charge. Distinguez les résultats attendus produits
indépendamment de la concordance entre outils sur des entrées réelles.

Le banc de références indépendant enregistre les occurrences attendues pendant
l’émission des instructions. Il vérifie à chaque mesure les identités complètes
de méthodes, les PC en unités de code, les opcodes, les identités cibles, les
unités UTF-16 et les multiplicités. Il vérifie aussi les compteurs de couverture
attendus indépendamment pour NeverD et `code_scan_complete` :

```sh
NEVERD_REFERENCE_TEST_BINARY="$PWD/build-release/bin/neverd" \
  python3 -m unittest scripts.tests.test_benchmark_mobile_references -v
python3 scripts/benchmark_mobile_references.py \
  --output-dir /tmp/neverd-reference-benchmark \
  --class-count 500 --methods-per-class 64 --matching-methods 2 \
  --dex-count 3 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Utilisez `--kind` et `--workload` pour sélectionner les cas. `--extra-strings 65536`
exerce de véritables indices de chaînes sur 32 bits. Les leurres de charges
utiles sont activés par défaut ; `--no-payload-lookalikes` conserve la disposition
et les vraies références tout en remplaçant les valeurs des leurres, pour des
comparaisons sur entrées communes. Conservez les résultats de correction et de
temps. Une requête produisant de fausses références dans les charges utiles ou
omettant des références réelles échoue à la validation ; aucune mesure n’est
alors acceptée.

L’option `--peer-command` accepte un modèle argv contenant `{input}`, `{kind}`
et `{query}`. Adaptez explicitement la syntaxe si un autre outil utilise une
sémantique différente et comparez les multiensembles complets d’occurrences.
Son périmètre de validation déclaré est conservé sans lui attribuer un balayage
complet du code. Les mêmes réserves sur le nouveau répertoire, l’affinité CPU,
les nouveaux processus, le cache chaud et le RSS que pour l’inventaire
s’appliquent. Le test unitaire CLI facultatif est ignoré sauf si
`NEVERD_REFERENCE_TEST_BINARY` désigne l’exécutable compilé ; signalez ce saut.

## Preuves des exports des SDK mobiles

Le workflow manuel `Mobile SDK Export Evidence` exécute `collect_mobile_ios_sdk_declarations.py --exports-only` avec les SDK Xcode épinglés. Il conserve sans modification les tables du linker de Foundation, CoreFoundation et UIKit des SDK iOS pour appareil et simulateur, avec la cible, la version du SDK, le hachage des paramètres du SDK, la taille et le SHA-256. Le collecteur de déclarations habituel les conserve aussi. Un fichier absent, vide, trop volumineux ou situé hors du SDK fait échouer la collecte tout en préservant les preuves déjà recueillies. Ces tables attestent les exports de symboles, sans prouver une ABI d’appel ni la récupération d’une méthode.

## Preuves d’ABI des chaînes Swift mobiles

Le workflow manuel `Mobile Swift String ABI Evidence` compile des sondes Swift fixes d’égalité et d’ordre, ainsi qu’une sonde C utilisant `swiftcall`, avec Xcode 26.5 pour les appareils iOS et simulateurs arm64. `collect_mobile_swift_string_abi.py` conserve les sources, LLVM IR, l’assembleur, l’identité des compilateurs, les paramètres SDK et `libswiftCore.tbd`, avec leurs empreintes. Les deux langages doivent montrer l’import exact de comparaison à cinq arguments retournant `i1` ; C doit explicitement étendre ce résultat sur un octet. Une cible ou signature incorrecte, un échec de commande ou un délai dépassé conserve les preuves partielles et fait échouer la collecte. Ces preuves n’installent aucune déclaration d’exécution et ne démontrent aucune récupération de méthode. Test sans SDK : `python3 -m unittest scripts.tests.test_mobile_swift_string_abi`.

## Simplification MBA modulaire

`SymExpr.*` vérifie les tranches constantes au-dessus du bit inférieur avec tous les masques et entrées sur quatre bits, des valeurs larges, des structures imbriquées, la recomposition de bits connus et inconnus et des contre-exemples de retenue. Les régressions de budget placent un nœud large à la limite de récursion et refusent la copie de constantes trop grandes. Les tranches inconnues doivent rester symboliques sans agrandir le DAG. `SymState.*` distingue aussi les constantes scalaires déduites des constantes littérales des régions, dans les deux ordres des octets, sans agrandir le DAG ni modifier les mots stockés.

`SymReadability.*` couvre l’écriture des soustractions et compléments, le coût des opérateurs associatifs, les littéraux sur un bit ou larges, la saturation des arbres partagés, la sélection bornée des candidats et l’équivalence exhaustive sur trois bits sans échantillonnage. `SymMBASample.*` compare les vérifications étroites et à précision arbitraire à l’évaluateur AP, pour tous les opérateurs, les affectations déterministes et les entrées larges inutilisées. Pour comparer des versions du score, recomptez les deux sorties avec la même mesure ; les compteurs de taille de l’API SDK sont seulement diagnostiques.

## Matrice de tests ARM32 et propagation de pile

```sh
cmake --build build-release --target NeverDSymbolicTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

La matrice des spills couvre aussi x86-32 (ELF/COFF/Mach-O), ARM32 (ARM et Thumb en ELF) et AArch64 (ELF/COFF/Mach-O) avec les deux backends C. Les rechargements répétés de pile privée doivent se réduire à des additions ou soustractions et s’exécuter correctement sur les paires d’octets, les frontières de mots et des mots pseudo-aléatoires déterministes aux deux niveaux d’optimisation. Les contrôles AST de Clang examinent les fonctions entières pour trouver les opérateurs MBA restants sans confondre les adresses valides. HighFrameStoreForwarding vérifie les largeurs, mutations locales, écritures mémoire, alias, chevauchements, accès ordonnés, graphes malformés et limites d’expansion. HighCStoreForwarding garde les définitions des valeurs mises en cache vivantes sur quatre architectures, y compris les réinterprétations flottantes ; SymSimplifyGuard vérifie identité et ordre des chargements, volatile/atomic et limites de poison. ELFARM32ModeTest vérifie la sélection ARM/Thumb, la normalisation des adresses, les métadonnées mixtes et les preuves contradictoires. ELFARM32ModeCAPITest vérifie les erreurs SDK explicites puis la récupération du décodeur après rechargement Thumb ; InstructionMode couvre décodeur, pointeurs de code, branches et génération. L’absence d’un Clang multi-cible est un skip, pas une preuve de compatibilité.

`HighBoundPrivateFrameCopies.*`, dans `NeverDHighControlFlowTests`, vérifie les copies passant par des emplacements de pile privés sans échappement après la liaison des ABI d’appel, notamment les branches, la réutilisation des emplacements et l’accord entre contextes de garde. Le C produit pour x64 et AArch64 est exécuté avec `-O0` et `-O2`, avec arrêt sur comportement indéfini et comparaison à des calculs indépendants. Les contre-exemples exigent de conserver la fonction originale en cas d’échappement, d’ABI inconnue, d’alias de pile absent ou incohérent, de réaffectation des paramètres d’entrée, d’accès superposés, de mémoire ordonnée ou atomique, d’instructions mal formées, de cycles ou de budget épuisé. Une conversion ordinaire ne doit pas devenir une copie PHI.

La projection source revalide aussi les listes d’objets variadiques après ce nettoyage : les ancres d’instruction vides sont admises, les effets cachés et transferts de contrôle sont refusés. Le nettoyage synchronisé admet une seule vue `int64_t` ou `uint64_t` du même récepteur sauvegardé ; les réductions de largeur, conversions flottantes, calculs d’adresse et réaffectations restent refusés. Les ensembles Foundation et les traces de déverrouillage normal ou exceptionnel sont exécutés à `-O0` et `-O2`.

## Exceptions synchrones natives x64

Les `DIV`/`IDIV` checked x64 utilisent le résultat du processeur et `#DE`. KVM emploie une IDT/IST supervisor privée, WHP un bitmap explicite ; le contexte original et les codes disponibles restent distincts des erreurs de transport. L’OS consomme l’événement récupérable avant d’installer une continuation. Le modèle de pilote Windows traduit la division par zéro et le débordement du quotient en `STATUS_INTEGER_DIVIDE_BY_ZERO`, avec de vrais filtres SEH, `__finally` et reprises. `NeverDX64ExceptionTests` se construit sans Unicorn ; `DriverWDMCPUException` valide les cas WDK originaux. Les hôtes ARM64 indisponibles sont explicitement ignorés.

## Effets RAM préparés

`RAMTransaction` conserve uniquement l’union physique des écritures déclarées d’une instruction, sous le verrou d’exécution. La RAM initiale est restaurée avant les observateurs de résultats ; annulation, erreur de transport ou exception de l’observateur ne publient aucun état partiel de RAM ou de registres. Après restauration de la RAM, les fautes CPU conservent leur état architectural d’exception. Les écritures simples et doubles ARM64 utilisent la même autorité. x64 exécute `XCHG`, `XADD` et `CMPXCHG` sur 8/16/32/64 bits, avec alignement naturel pour les formes verrouillées ou implicitement verrouillées. `NeverDRAMTransactionTests` compare les résultats à la CPU hôte et vérifie restauration, alias et permissions ; les plateformes indisponibles sont explicitement ignorées. Périphériques et SMP parallèle restent exclus ; les instantanés CPU ne restaurent pas la RAM déjà validée.

## État x87 complet

`NeverDEmulationArch` possède les contrats ISA, les tables de pages et le format FP partagé par les transports natifs et Unicorn. Les contextes x64 conservent contrôle, état, TOP, tags physiques, opcode, pointeurs instruction/données et huit registres de 80 bits. `FP0`–`FP7` utilisent `RegisterValue` ; les accès scalaires refusent la troncature. `FPTag` est le masque physique des registres non vides. `NeverDX64FPTests` vérifie tous les TOP, les opérations exactes contre FXSAVE/FXRSTOR du processeur hôte et la restauration. Cela ne rend pas les instructions x87 admissibles en mode checked et ne prouve pas tous les arrondis. Les hôtes natifs indisponibles sont explicitement ignorés.

`driver-strict` accepte KVM sur un hôte Linux x64 compatible et WHP sur un hôte Windows x64 compatible ; `auto` sélectionne ce transport natif, et les ISA différentes utilisent Unicorn. Unicorn explicite et l’API V1 conservent le profil logiciel portable. L’exécution native vérifie les adresses canoniques et les effets avant l’entrée ; le matériel indisponible provoque un échec sans repli. Instructions et comportements OS non pris en charge échouent explicitement. La CI native Windows x64 avec Unicorn désactivé réussit les 359 contrôles obligatoires : 131 contrôles CPU, 224 résultats de pilotes issus de 26 images intégrées, 46 images WDK et 40 cas de scénarios aux adresses préférées et relocalisées, ainsi que quatre contrôles de limites SEH ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Les preuves natives ARM64 restent manquantes ; aucune compatibilité universelle des pilotes ou Android/Darwin n’est établie.

La validation native ci-dessus couvre les points d’entrée déclarés des pilotes et les scénarios publiés. Les régressions détaillées par fonctionnalité et les contrôles C API/CLI/Python décrits ci-dessous conservent des preuves limitées à Linux, sauf mention explicite d’une exécution Windows ; la réussite du corpus natif ne valide pas chaque variante de test sous Windows.

Interrogez le profil sélectionné avec `executionCapabilities(Contract, ISA, Backend)`. `NativeLegacyX64` décrit l’exécution native des pilotes x64. `NeverDNativeDriverTests` valide le corpus existant et peut fonctionner dans une compilation sans Unicorn.

Le workflow CI existant exécute tous les tests d’émulation avant les profils généraux et conserve l’inventaire, les résultats JUnit et le journal CTest dans `emulation-focused`. Une défaillance ailleurs ne bloque pas cette exécution. Le matériel indisponible et les pilotes optionnels absents restent des tests explicitement ignorés ; une réussite logicielle ou de compilation ne prouve pas l’exécution native.

Sur Linux, `NeverDUnicornDeadlineTests` fait terminer le véritable thread du minuteur avant l’entrée dans l’invité en contrôlant l’ordonnancement pthread. Il couvre x64, ARM32 et ARM64, exige l’absence d’effets après une annulation préalable et vérifie le budget indépendant de l’exécution suivante. Il utilise les API publiques sans modifier l’état privé du moteur.

`X64StateTransition` dans `NeverDX64ExceptionTests` effectue des lectures RAM indépendantes et des lectures CR8 sur le CPU natif. Il alterne les bases TLS et le privilège, reprend après des fautes de division répétées et change TLS après une entrée annulée. Après une modification du transfert d’état natif, exécutez ce label CTest ainsi que la couverture des alias remappés, contextes CPU, états FP et résultats des pilotes originaux. Un transport KVM/WHP indisponible reste un saut explicite.


`NeverDKvmRunTests` vérifie les transferts empruntés de `KvmRunControl` sans nécessiter `/dev/kvm`. `StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` vérifie que préparation, collecte et entrée hôte interceptée utilisent le même thread, avec une seule préparation malgré les reprises après interruption. Les autres cas couvrent l’échec de préparation sans entrée, l’échec de collecte, l’arrêt pendant la préparation et l’annulation d’une entrée active, puis une nouvelle exécution sans réutilisation des anciens callbacks. Conservez les suites de véritable annulation, de restauration RAM, d’exceptions et de pilotes originaux dans la validation. `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` vérifie que plusieurs entrées sous une même échéance réutilisent le thread, exécutent chaque transfert une seule fois et ne modifient pas les paquets précédents.

KVM x64/ARM64 utilise `KvmRunControl` pour préparer, entrer dans `KVM_RUN` et capturer l’état sur un même worker vCPU privé. La préparation n’a lieu qu’une fois malgré `EINTR` ; annulation et capture échouée interdisent la publication. `KvmAArch64Machine.cpp` exécute aussi la maintenance des traductions et les transferts scalaires/vectoriels complets sous une échéance commune. L’appelant publie après confirmation et conserve décodage ISA, transactions RAM, politique OS et observateurs. Les preuves natives ARM64 restent manquantes.

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` vérifie la continuation et les écritures du CPU après modification hôte des registres généraux, des XMM aux deux extrémités, de MXCSR et du contrôle x87. Les octets réels de `FXSAVE64` vérifient tous les registres physiques de 80 bits, TOP, tags, opcode et pointeurs après une entrée arrêtée ; les fautes de division répétées invalident aussi la réutilisation. Ces tests machine n’admettent aucune instruction x87 supplémentaire dans les profils checked.

`NeverDKvmStateTransferTests` injecte un échec de lecture des registres ou de XSAVE après une véritable exécution KVM, puis reprend avec l’entrée inchangée. Les résultats indépendants entiers et d’octets empaquetés prouvent qu’une collecte échouée ne réutilise pas l’état natif déjà avancé. Seul cet exécutable enveloppe `ioctl` ; les hôtes natifs indisponibles sont explicitement ignorés.

ARM64 vérifié possède une frontière commune pour l’état complet. `Registers.def` définit 39 champs scalaires et 32 vecteurs de 128 bits ; `captureAArch64State` prépare toutes les lectures, applique les largeurs et normalise NZCV avant une publication unique. Unicorn, KVM, WHP et HVF transfèrent le même inventaire, dont TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR et FPSR. Les adaptateurs natifs activent FP/SIMD via CPACR_EL1. Toute lecture échouée ou entrée annulée préserve l’état complet de l’appelant.

Le démarrage ARM64 KVM/WHP/HVF exécute le programme privé `AArch64MachineProbe.def` : NOP, addition FP32 arrondie vers l’infini positif et addition SIMD à deux voies. Chaque étape compare les 39 champs scalaires et 32 vecteurs, dont TLS, NZCV, l’effacement des bits supérieurs du résultat et la conservation/accumulation de FPCR/FPSR. La sonde utilise uniquement la mémoire de supervision et une échéance globale. Les sondes attestent uniquement cette initialisation bornée. La validation des charges Linux ARM64 KVM et Windows ARM64 WHP reste à établir ; les résultats natifs macOS figurent dans le [guide HVF](macos-hvf.md).

L'initialisation native x64 KVM/WHP/HVF exécute `X64MachineProbe.def` dans des pages supervisor privées. Une échéance unique couvre NOP, l'addition FP32 arrondie vers l'infini positif, l'addition SIMD à deux voies, les lectures FS/GS et CS/SS/CR8 ; chaque pas compare tous les états scalaires, XMM, x87 physiques et de contrôle. Les sondes x64 et ARM64 exigent le bail exclusif d'exécution de la mémoire physique. `MemoryProjection` possède l'identité du cache (ISA, espace d'adresses, génération des mappings, privilège et variante du moniteur) et l'historique des racines validées par ISA. Les constructeurs invalident le cache avant toute réécriture : un remplacement échoué ne réutilise pas de tables partielles et les appelants ne fournissent pas de racines périmées. Les sondes attestent uniquement cette initialisation bornée. La validation des charges Linux ARM64 KVM et Windows ARM64 WHP reste à établir ; les résultats natifs macOS figurent dans le [guide HVF](macos-hvf.md).

Le décodeur XSAVE commun distingue l’état SSE initial standard et compacté. Si XSTATE_BV[1] est nul, les deux formats initialisent XMM ; le format standard lit et valide encore MXCSR, tandis que le format compacté l’initialise. `X64XsaveCases.def` fournit des dispositions indépendantes et des programmes XRSTOR hôtes originaux. `X64XsaveTests.cpp` vérifie l’atomicité des refus et compare les deux formats à l’exécution réelle, en préservant l’état FP/SSE de l’appelant. L’oracle est explicitement ignoré si l’architecture hôte ou la fonction d’instruction requise est indisponible.

`X64FPState.def` déclare les dispositions compactées AVX, AVX-512, CET_U/CET_S et AMX, avec alignement des composants sur 64 octets. Les données présentes doivent correspondre à l’état initial nul ; les composants absents et le remplissage ne définissent aucun état. Les bits de disposition fixent les décalages ; disposition inconnue, données non initiales ou longueur incorrecte échouent avant publication. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` et `InitialWideComponentsDoNotHideFPState` couvrent les paquets WHP de 872 et 10752 octets. Ce transport n’autorise pas les instructions de ces extensions.

`WhpXsaveRegisters.def` complète les paquets XSAVE avec les registres de contrôle x87/SSE nommés. Le dernier opcode et les pointeurs d’instruction/données sont écrits explicitement puis relus sur l’hôte. Les champs nuls peuvent être complétés ; les conflits non nuls ou les contrôles communs incohérents échouent avant publication. `NamedMetadataRestoresOmittedPacketFields` vérifie ce cas en conservant toute la charge FP.

Les champs natifs `FOP/FIP/FDP` suivent les règles x87 de l’hôte. AMD peut les effacer sans exception non masquée en attente ; les instantanés conservent les valeurs observées. `X64MachineProbe.def` et les tests exacts NOP/contexte initialisent une exception cohérente en attente afin de comparer chaque champ valide sans masquer les écarts. La référence FXRSTOR64/FXSAVE64 du processus hôte vérifie les deux états ; les backends ne remplacent jamais les résultats hôte par les métadonnées d’entrée.

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` compare FP/SSE complet sauvegardé en RAM par le FXSAVE64 invité et la capture XSAVE hôte. Les deux API vérifient l’installation directe et FXRSTOR64 invité, avec le réglage de sauvegarde des pointeurs par défaut puis celui pris en charge par l’hôte explicitement sélectionné. Le test distingue entrée, exécution et capture sans corriger les valeurs ; toute divergence échoue. La matrice couvre aussi une exception x87 non masquée en attente et consigne une référence FXRSTOR64/FXSAVE64 exécutée dans le processus hôte ainsi que le fournisseur du processeur, pour distinguer sauvegarde conditionnelle des pointeurs et transport WHP.

Le codec partagé `encodeX64XsaveState` / `decodeX64XsaveState` possède les paquets FP/SSE standard ou compactés, la rotation TOP physique, l'état initial des composants absents et la validation atomique. WHP utilise les API XSAVE complètes, préférant `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`, avec les anciennes API XSAVE comme voie de compatibilité. Les anciens registres x87 individuels ne remplacent pas les paquets complets. Les composants étendus non initiaux, en-têtes malformés, contrôles invalides et captures tronquées échouent explicitement. Les erreurs de mapping WHP conservent HRESULT, GPA et taille pour le diagnostic.

`CheckedX64Instructions.def` admet `MUL` non signé sur 8/16/32/64 bits et `CBW/CWDE/CDQE/CWD/CDQ/CQO` via le transport CPU existant. `NeverDX64IntegerTests` utilise les encodages et valeurs attendues indépendants de `X64IntegerCases.def` aux deux niveaux de privilège : préservation partielle des registres, extension par zéro sur 32 bits, deux moitiés du produit, résultats CF/OF définis et indicateurs inchangés après extension de signe. La multiplication en RAM ordinaire conserve les contrôles de droits sur toute la plage et les observateurs de lecture ; un défaut ou un arrêt demandé par un observateur préserve les registres de sortie implicites et PC. Les opérandes de périphérique restent non pris en charge. Ces cas exécutent aussi checked Unicorn ; les transports natifs indisponibles sont explicitement ignorés.

`X64BitInstructions.def` autorise `BT/BTS/BTR/BTC` sur registres et RAM ordinaire à 16/32/64 bits. L’index de registre est signé à la largeur de l’opérande et sélectionne un mot entier ; l’immédiat reste dans le mot de base. La troncature à la largeur d’adresse précède l’ajout de la base FS/GS. Le processeur fournit CF et les valeurs écrites ; `RAMTransaction` garde le résultat privé jusqu’à son acceptation par les observateurs. Les permissions sont vérifiées sur toute l’étendue, y compris les allocations de pages distinctes et les alias. Arrêts, échecs de callbacks et accès refusés préservent CPU et RAM. LOCK est limité aux modifications mémoire naturellement alignées ; MMIO et SMP matériel parallèle restent exclus. `X64BitStringTests.cpp` compare des encodages indépendants à l’exécution x64 réelle et vérifie indices négatifs, troncature, franchissements de pages, annulation et formes LOCK invalides. Voir la [référence Intel](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X64StringInstructions.def` définit `MOVS/STOS/LODS` sur la RAM ordinaire en 8/16/32/64 bits ; `CLD/STD` ne modifie que la direction. Chaque élément REP valide tout son opérande avant observation et se valide à une frontière de reprise. Un défaut ultérieur conserve les éléments terminés ; un arrêt ou une exception de rappel laisse l’élément courant intact. FS/GS ne s’ajoute qu’à la source, après réduction de la largeur d’adresse. AL/AX préserve les bits supérieurs et EAX les met à zéro. Un REP de compte nul en adressage 32 bits exige des bits supérieurs de compte nuls, ainsi que ceux des adresses utilisées pour MOVS/STOS : les processeurs réels divergent autrement. REPNE sur MOVS/STOS/LODS et les opérandes de périphérique STOS/LODS restent exclus. `X64StringTransferTests.cpp` compare aux instructions natives indépendantes les largeurs, directions, chevauchements et comptes nuls ; il vérifie aussi permissions, alias, bouclage d’adresse, défauts et reprise. Le pilote WDK de ressources original exécute les quatre largeurs STOS/LODS via `driver_resource_strings.def`.

`X64StringInstructions.def` définit aussi `CMPS/SCAS` sur RAM ordinaire en 8/16/32/64 bits avec `REPE/REPNE`. Chaque élément valide toutes les lectures avant observation, actualise les six indicateurs arithmétiques et s’arrête à la première condition de fin. Un défaut de données restaure les indicateurs à l’entrée de ce REP ininterrompu, tout en conservant les pointeurs et le compteur des éléments terminés ; une reprise publique repart de l’état CPU publié. Arrêts et exceptions des observateurs ne modifient pas l’élément courant. Une fin anticipée ne lit jamais l’élément suivant. FS/GS ne concerne que la source CMPS ; SCAS conserve l’accumulateur et le registre source inutilisé. Périphériques et bits hauts ambigus à compteur nul en 32 bits restent exclus. `X64StringComparisonTests.cpp` compare instructions hôtes indépendantes, indicateurs, direction, alias, bouclage, permissions et reprise ; son oracle Linux x64 capture les registres au défaut réel. Le pilote WDK original exécute les deux répétitions conditionnelles aux quatre largeurs via `driver_resource_strings.def`. Voir la [référence Intel](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`WhpResourceCache.h` sépare l’état des CPU logiques des partitions WHP. Le runtime conserve une partition native active et la réutilise pour les étapes successives du même CPU. Un changement de CPU détruit l’ancienne partition avant de reconstruire les mappings, le processeur virtuel et l’état complet. Les CPU logiques gardent leurs vues `MemoryProjection` indépendantes et la RAM de référence. L’acquisition du bail respecte l’annulation et l’échéance courante ; détruire un CPU inactif ne détruit pas la partition d’un autre CPU. x64 conserve les fonctions XSAVE par défaut de l’hôte et vérifie la partition effective avec `WHvGetPartitionProperty`, sans supprimer les fonctions dépendantes pour réduire le masque. Cette alternance coopérative ne fournit pas de SMP matériel parallèle.

`NeverDX64FPTests` vérifie les 79 positions de corruption et exécute les instructions indépendamment assemblées de `X64ProbeCases.def` sur les transports natifs, avec une échéance unique et RAM invitée inchangée. `NeverDProjectionCacheTests` couvre appelants, ordre ISA, historique des racines, variantes privilège/moniteur, générations, identité des espaces et remplacement échoué. `NeverDRunControlTests` inclut `WhpXsaveTests.cpp` pour les paquets des API anciennes et modernes, chaque TOP, les tailles et l’état inchangé après erreur ; ces tests de protocole en mémoire ne constituent pas une preuve WHP native. Les transports natifs indisponibles sont explicitement ignorés.

Les diagnostics XSAVE distinguent requête de taille, préparation locale et décodage du paquet capturé. Ils conservent le nom API, les tailles retournée et allouée ainsi que des métadonnées limitées d’en-tête et de contrôle, avec des attentes indépendantes dans `WhpHostFailureCases.def`, sans afficher les données des registres invités. `InvalidInputReportsPreparationWithoutHostMutation` vérifie aussi qu’une entrée refusée ne provoque aucun appel hôte ni modification de son paquet. Le codec ISA commun reste l’unique autorité de validation.

Les erreurs hôte WHP lors des requêtes de capacités, de la configuration des partitions/CPU virtuels, du transfert des registres/XSAVE et de l’exécution conservent le HRESULT et le nom d’API déclaré dans `WhpProtocol.def` ; les échecs de requête de capacités conservent le résultat typé d’indisponibilité. `WhpHostFailureCases.def` définit des attentes indépendantes pour les erreurs hôte simultanées à une annulation et les échecs de requête, d’installation et de capture XSAVE modernes ou anciens. Le CI Windows ciblé exige 210 succès natifs : 16 cas de mappage, deux de démarrage, dix FP/contexte, sept CPU partagés, huit entiers et les deux variantes API de `NativeInstallRetainsFPStateBeforeAnyGuestExecution`. Ces dernières comparent FP/SSE complet et métadonnées lues séparément avant toute exécution invitée. Toute inscription manquante, tout test ignoré, désactivé ou non exécuté fait échouer l’audit des preuves natives. Les 26 contrôles supplémentaires couvrent tous les cas de `X64BitStringTests.cpp` aux deux niveaux de privilège. Windows PE64 exige 67 cas de processus WHP et onze cas indépendants avec Windows natif.

`NeverDMemoryLifecycleTests` se construit indépendamment d’Unicorn, y compris dans les configurations natives seules. Les cas de projection et de périphériques propres au logiciel sont explicitement ignorés si Unicorn est désactivé ; les cas de CPU partageant la RAM sur l’hôte correspondant restent enregistrés. `WhpMemoryTests.cpp` isole l’API mémoire native avec 16 cas de `WhpMemoryCases.def` : taille d’une page ou de la projection, allocations partagées ou indépendantes, octets non touchés ou résidents, présence ou absence du premier processeur virtuel. Chaque cas conserve deux propriétaires logiques, alterne plusieurs fois leur partition mappée, détruit le propriétaire inactif et vérifie que le mapping restant fonctionne sans recréation. Une erreur réelle conserve son HRESULT et fait échouer le test ; cette preuve de l’API mémoire ne prouve pas l’exécution d’instructions.

`X64MachineProbe.def` identifie l’instruction de démarrage en échec et toutes les différences de scalaires, TLS, privilège, contrôles x87, voies FP physiques et mots XMM, avec valeurs attendues et observées. `DiagnosticIdentifiesStepFieldAndBothValues` vérifie des messages attendus indépendants. La comparaison reste exacte : le diagnostic distingue une perte de transfert d’un problème d’exécution sans valider une sonde native ayant échoué.

`WhpResourceTests.cpp` couvre la réutilisation, la destruction avant remplacement, la reprise après échec et les courses entre échéance et arrêt. `LogicalCPUSwitchingRestoresPhysicalFPAndTLS` alterne deux machines dans les deux modes de privilège, vérifie leurs états physiques x87/XMM et FS/GS indépendants, puis reprend la survivante après destruction de sa paire. La CI Windows exige les deux cas WHP.

`NEVERD_ENABLE_SEMANTIC_TESTS`, activé par défaut (`ON`), contrôle le groupe de tests de `unittests/semantic` et ses cibles agrégées. Pour construire les tests CPU natifs sans Unicorn, conserver `BUILD_TESTING=ON` et définir `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` ainsi que `NEVERD_EMULATION_BACKEND_UNICORN=OFF`. Les tests natifs KVM/WHP restent disponibles, y compris avec Windows ARM64/MSVC et les en-têtes SDK appropriés. Activer Unicorn sous Windows ARM64 exige toujours une chaîne ARM64 LLVM-MinGW. Cette séparation de construction ne constitue pas une validation native ARM64.

Le CI CPU natif initialise les sources Capstone à la révision fixée et utilise le paquet LLVM précompilé vérifié. Avec `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` et l’adaptateur Unicorn désactivé, la configuration, la compilation et l’édition de liens des tests CPU ne nécessitent aucune source Unicorn. Les signatures et le corpus externe ne sont pas requis. Le CI par défaut conserve le groupe complet de tests sémantiques.

Le fichier `ci.yml` existant propose le mode manuel explicite `native_cpu_only` sur son runner Windows x64. `NativeCPUTests.def` sélectionne onze cibles ; `run_native_cpu_ci.py` les construit avant le CTest filtré et conserve inventaire, JUnit, journaux et synthèse. Les parseurs CI communs distinguent réussite, échec, omission, désactivation et non-exécution. Tous les cas natifs WHP déclarés doivent être découverts et exécutés ; une preuve absente ou ignorée fait échouer ce job ciblé. La CI par défaut construisant LLVM depuis les sources ne change pas. Les tests de protocole et la compilation ne remplacent pas la validation native des charges WHP ou ARM64.

Avec `native_cpu_only=true`, `native_driver_tests=true` active `NeverDNativeDriverTests` sans Unicorn. Avant la configuration, `build_wdk_driver_fixtures.py` vérifie le SHA-256 intégral des paquets Microsoft officiels WDK/SDK 10.0.26100.6584 et reconstruit 46 images de pilotes normales/CFG/DBG depuis les sources originales. `WDKDriverFixtures.def` déclare les paquets, les arguments de compilation et d’édition de liens et les associations des fixtures. Les fichiers Microsoft non modifiés et leurs licences restent dans les répertoires locaux de compilation/cache ; la CI ne publie que les métadonnées et journaux de compilation. Le manifeste conserve versions des outils, commandes, empreintes des sources/en-têtes et empreintes des images produites.

`NativeDriverTests.def` exige 224 résultats WHP pour les 112 charges de `DriverBuiltinImages.def` et `DriverBackendParityCases.def` : 26 images intégrées, 46 images WDK et 40 scénarios de requêtes, aux adresses initiales et relocalisées. Avec les 210 contrôles CPU et quatre régressions partagées de continuation SEH, 438 résultats sont obligatoires. Les images fixes conservent le rejet de relocalisation attendu. Une image ou un scénario WDK absent ou ignoré fait échouer cette tâche CI facultative ; les builds locaux ordinaires gardent les fixtures externes facultatives. `run_native_cpu_ci.py --with-drivers` enregistre les cibles configurées et les preuves complètes d’inventaire/JUnit. La compilation ne prouve pas l’exécution native Windows ou ARM64. Les commandes suivantes permettent une reproduction locale ; le cache généré peut aussi être chargé dans un build d’émulation existant. `210 CPU + 224 WHP + 4 SEH = 438`.

Les plages C SEH restent semi-ouvertes. Une cible valide de `__C_specific_handler` peut se trouver dans sa plage protégée : [LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) émet `EndLabel + 1` comme borne de fin. Le modèle Windows conserve ces bornes et vérifie séparément que la cible est exécutable, appartient à la fonction et correspond à la continuation, y compris après relocalisation. `KernelSEHContinuationCases.def` conserve la disposition de la fixture originale ; `ScopeEndLabelMayOverlapTheHandlerLandingPad` couvre les gestionnaires constants et les filtres. Les tests associés vérifient la borne exclusive et le rejet des cibles invalides sans consommer l’état de dispatch. Ces contrôles purs s’exécutent dans `NeverDNativeDriverTests` avec Unicorn désactivé.

Le déroulement vers une cible utilise aussi la borne brute : un `finally` dont la plage contient encore la cible du gestionnaire n’est pas quitté. `FinallyRespectsRawScopeEndAtHandlerTarget` vérifie les deux côtés de cette limite et les compare directement à `ntdll.dll!__C_specific_handler` sous Windows x64. NeverD ne répare pas les plages produites par le compilateur. Les builds Clang 20/21 de la fixture originale renvoient un échec invité dans les modes `T` et `J`, car la borne décalée inclut la cible choisie ; Clang 23 exécute les deux nettoyages. Le [changement LLVM #144745](https://github.com/llvm/llvm-project/pull/144745) supprime l’ancien biais `+1`. Ces résultats propres au compilateur sont distincts des pannes du backend.

L’inventaire couvre tous les programmes C WDM/KMDF originaux et tous les chemins WDK optionnels de CMake. Chaque `driver-*-scenario.json` publié possède des cas normal et CFG dans `DriverBackendParityCases.def` ; une liaison source, compilation ou scénario manquante fait échouer les tests d’inventaire. `Original` efface l’adresse imposée par le scénario et vérifie la base préférée de l’image ; `Rebased` vérifie la base relocalisée déclarée. Le scénario IRP appartenant au pilote annule volontairement une requête enfant : `DriverNativeOutcomes.def` conserve donc son échec global attendu après un nettoyage réussi.

```bash
python3 scripts/build_wdk_driver_fixtures.py \
  --output build-driver-fixtures --cache build-driver-packages
cmake -S . -B build-native -G Ninja \
  -C build-driver-fixtures/fixtures.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
```

`NeverDAArch64StateTests` vérifie la corruption de chaque champ scalaire et des deux mots de chaque vecteur, les changements de privilège, l’absence d’exécution flottante et la conservation des diagnostics de transport. `NeverDAArch64FPTests` exécute `OriginalProgramChecksCompleteStateAndOneDeadline` aux deux privilèges sur les transports réels avec les instructions assemblées indépendamment dans `AArch64ProbeCases.def`. Le test déplace ces mots indépendants du PC vers le code invité sans ouvrir les pages de supervision à l’utilisateur. Unicorn et les omissions natives explicites ne remplacent pas la preuve de démarrage ARM64 natif.

`CheckedAArch64Instructions.def` et `AArch64InstructionEffects` admettent à EL0/EL1 un sous-ensemble borné FP32/FP64, comparaisons, déplacements et SIMD à largeur fixe. FPCR conserve quatre modes d’arrondi, FZ et DN ; FPSR conserve les statuts cumulés et QC. Les bits non pris en charge sont rejetés avant mutation. FP16 arithmétique, SVE/SME, exceptions non masquées, extensions optionnelles et formes absentes échouent explicitement. Cela n’ajoute ni chargement des pilotes Windows ARM64 ni nouvel environnement OS.

`AArch64InstructionEffects` possède les empreintes RAM scalaires et FP/SIMD simples ou par paires, jusqu’à 128 bits par opérande. L’espace partagé vérifie chaque page avant l’entrée ; `RAMTransaction` ne valide que les écritures physiques déclarées complètes. Un observateur de 128 bits reçoit deux mots ordonnés de 64 bits avant les effets. Arrêts et fautes préservent RAM, vecteurs et mise à jour d’adresse. Le même numéro Xn/Vn est permis ; les paires dont l’adresse boucle sont rejetées. `NeverDAArch64MemoryTests` utilise `AArch64CrossPageCases.def` et `AArch64VectorMemoryCases.def` indépendants.

`NeverDAArch64StateTests` vérifie les 71 lectures de l’état complet aux deux privilèges, normalisation, lecteurs absents et reprise. `NeverDAArch64FPTests` exécute les instructions originales de `AArch64FPCases.def` : tous les éléments vectoriels, arithmétique empaquetée, FP scalaire/vectoriel, quatre arrondis, FZ/DN, FPSR cumulé, contextes et rejets. `NeverDAArch64MemoryTests` vérifie chaque décalage interpage, ordre/arrêt des observateurs, permissions, alias et entrées vectorielles restaurées. `NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`) injecte chaque échec de lecture scalaire/vectorielle après exécution réelle. Les transports natifs absents sont explicitement ignorés ; ces preuves ne remplacent pas une exécution ARM64 KVM/WHP native.

`NeverDAArch64MemoryTests` couvre 18 formes scalaires/par paires avec Unicorn, KVM et WHP aux deux niveaux de privilège : chaque décalage entre pages, signe et largeur, ordre des observateurs, seconde page interdite ou absente, consommation explicite du défaut et nouvel essai, alias physiques répétés et restauration du contexte après remplacement des alias. L’ancien rejet d’un chargement valide entre pages a été reproduit avant modification. Les transports indisponibles sont explicitement ignorés ; Unicorn et la compilation croisée ne remplacent pas une preuve native ARM64 KVM/WHP.

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) vérifie les métadonnées CFG inactives sans indicateur, les pointeurs de repli inchangés aux deux adresses de chargement, les emplacements/cibles invalides et les relocalisations manquantes. Ses cas utilisent explicitement Unicorn/KVM/WHP avec `driver-strict` et `checked-x64-v1` ; chaque moteur indisponible est ignoré séparément. `DriverPublicCLICases.def` sélectionne `--backend unicorn` pour comparer la CLI à l’API C v1 compatible. Les choix natifs et `auto` conservent leurs tests publics distincts, sans repli silencieux lorsque l’API hôte est indisponible.

Unicorn vérifié utilise `MachineRunControl` : une seule allocation couvre maintenance ARM64, exécution invitée et capture complète de l’état. `UC_HOOK_CODE` contrôle le jeton d’arrêt emprunté et l’échéance à l’entrée de l’instruction. L’appel synchrone libère l’emprunt du hook avant son retour, mais le pas machine conserve le contrôle jusqu’à la publication. Unicorn et WHP préparent tout l’état CPU et vérifient le même contrôle avant de publier un pas réussi. WHP crée son allocation une fois avant la préparation. Une exception CPU x64 authentifiée prime sur un arrêt reçu pendant la capture. La transaction RAM vérifiée abandonne les écritures spéculatives si la capture est annulée ; le contrat logiciel non restreint reste inchangé. `MachineInterruptedError` distingue une annulation confirmée d’un échec hôte ou de capture. Le CPU vérifié commun renvoie `Stopped` ou `Deadline`, conserve CPU/RAM et permet la reprise ; les vrais échecs restent `BackendFailure` même avec un arrêt simultané.

Régressions de capture : `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests` utilise les instructions de stockage originales de `UnicornMachineControlCases.def` sur de vrais moteurs x64 et ARM64 aux deux privilèges. `RejectedEntryPreservesStateAndRAMAndAllowsRetry` vérifie l’annulation avant le pas, l’arrêt ou l’expiration à l’entrée invitée réelle, la conservation de l’état d’entrée complet et de la RAM, puis un stockage réussi. Son wrapper d’entrée réservé aux tests ne nécessite pas d’hyperviseur et ne prouve pas l’exécution native ARM64/WHP.

`RunDeadline::invoke` refuse une entrée WHP déjà arrêtée ou expirée avant l'appel hôte, conserve le résultat hôte réel pendant l'annulation et attend la fin des rappels d'interruption avant de libérer le jeton emprunté. KVM et WHP valident l'état privé entièrement capturé sur le thread appelant détenteur du bail d'exécution, avant de classer un arrêt ou une échéance simultanés. Les erreurs réelles d'hôte ou de capture et les exceptions CPU x64 authentifiées restent prioritaires. Un état ordinaire réussi reste privé jusqu'à la fin des contrôles d'annulation ; une interruption acquittée abandonne les effets CPU/RAM spéculatifs et autorise une nouvelle tentative. Préparation, exécution native et capture partagent un seul délai de grâce par pas. Ce contrôle est coopératif et ne garantit aucune limite stricte en temps réel.

`NeverDRunControlTests` comprend les tests portables `NativeEntryTests.cpp` et, sous Windows avec WHP, `WhpEntryControlTests.cpp`. Des rappels hôtes en mémoire vérifient refus d'entrée, nouvelle tentative, annulation tardive, conservation des erreurs, priorité du résultat et durée de vie acquittée, sans Hyper-V. `NeverDKvmRunTests` contrôle la finalisation sur le thread appelant, la priorité des erreurs et le refus de réentrée. Les tests réels `NeverDKvmStateTransferTests` exécutent les instructions originales de `KvmStateTransferCases.def` ; `ActualCPUExceptionOutranksStopDuringCapture` et `PublicCPUExceptionOutranksStopDuringCapture` arrêtent après lecture réelle des registres/XSAVE et préservent l'exception de division, le contexte initial, la RAM et la reprise explicite. Les tests portables sous ABI Windows avec Wine prouvent seulement le contrôle et les threads, pas l'exécution WHP native. Les transports natifs indisponibles restent explicitement ignorés.

`windows-pe64-v1` prend en charge des processus console Windows x64/ARM64 bornés avec PEB/TEB, TLS statique et dynamique, `DllMain`, API Win32 nommées et graphes DLL explicites sans cycle. Les modules invités acceptent les imports de code/données par nom ou ordinal, DIR64, les exports redirigés et les véritables listes du chargeur. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` et `GetProcAddress` utilisent le catalogue configuré. CRT/GUI, SEH utilisateur ARM64 fondé sur les cadres de pile, threads et compatibilité Windows générale restent inachevés ; les preuves natives ARM64 KVM/WHP manquent encore.

Les octets d’entrée et les étendues cumulées des images partagent chacun `memory_limit` ; le runtime compte aussi dans le budget image. La préparation partage 65,536 enregistrements, 64 MiB de lectures de métadonnées, des noms bornés et l’échéance du travail, sans garantie stricte pour les E/S hôtes. Le fixture original EXE→DLL→DLL vérifie rebasage, ordinaux, données partagées, identité API, `MEM_IMAGE`, listes et attach/detach TLS de l’EXE. `NeverDWindowsProcessTests` inclut l’oracle Windows natif, `NeverDPEProgramExportsTests` les métadonnées invalides et budgets, `NeverDProcessPublicTests` la parité C ABI/CLI. Les transports indisponibles sont explicitement ignorés.

`WindowsProcessLifetime` exécute les callbacks TLS puis `DllMain` des DLL en ordre de dépendance, puis le TLS et l’entrée EXE, sur un CPU avec un budget commun. Chaque module possède un index TLS et un bloc aligné indépendant, copié depuis l’image relocalisée et liée dans une arène de 64 KiB. L’argument réservé TLS vaut zéro ; celui de `DllMain` au démarrage/détachement du processus est opaque et non nul. Une sortie explicite détache les DLL initialisées en ordre inverse de la liste du chargeur puis le TLS EXE, même avant l’initialisation EXE. `DllMain(FALSE)` au démarrage termine avec `0xc0000142` sans détachement. Fautes et budgets épuisés n’inventent aucun nettoyage. Le retour de l’entrée PE avec DLL invitées exige une terminaison de thread non prise en charge et s’arrête explicitement. `SizeOfZeroFill` non nul reste exclu ; les octets initialisés à zéro du modèle TLS réel sont admis. Les DLL sans entrée reçoivent TLS attach, mais aucune notification de détachement du processus.

`WindowsProcessExports` partage la résolution nom/ordinal entre imports statiques et `GetProcAddress`, avec code, données, alias et redirections en chaîne. Seules les redirections initiales utilisées ajoutent des modules du catalogue et des dépendances d’initialisation ; les autres ne chargent aucun fichier. Les noms respectent la casse ; un nom absent renvoie NULL/erreur 127, un ordinal directement recherché absent (y compris un trou) NULL/erreur 182, et un argument de requête NULL l’erreur 87, un succès conserve LastError. Les handles inconnus restent non pris en charge. Les entrées API exactes fournisseur/nom sont réservées une fois depuis le registre borné. La résolution vérifie les en-têtes PE et métadonnées d’export actuels de chaque image, refuse toute modification ou lecture impossible, limite la chaîne à 64 entrées et partage les crédits de métadonnées restants et l’échéance du processus. Une redirection vers un trou renvoie la base de l’image cible et conserve LastError ; vers l’ordinal zéro, elle renvoie l’erreur 87. Cette base est une adresse de données et n’autorise pas l’exécution des en-têtes. Les redirections à l’exécution peuvent charger les modules configurés et terminer leur initialisation avant de renvoyer le résultat. La modification active de la table des exports reste non prise en charge.

`WindowsProcessLoader` charge les noms de base DLL ASCII de `windows.modules` et gère références explicites, dépendances partagées et maintien des modules de démarrage. Répéter une résolution redirigée ne rajoute pas de référence. Chaque rechargement attribue une nouvelle génération résidente au même emplacement du catalogue. TLS et `DllMain` utilisent le même CPU, sous les cadres API suspendus ; restaurer les registres conserve les écritures invitées et utilise le retour actuel. Les pointeurs réservés attach/detach dynamiques valent zéro. Un attach échoué lors d’un chargement explicite renvoie 1114 après nettoyage, sans annuler les chargements imbriqués indépendants réussis. Le déchargement libère images et TLS ; le rechargement restaure les octets originaux. Toute modification externe des listes du chargeur ou pointeurs TLS est refusée. Les budgets de fichiers, images et métadonnées restent cumulatifs, même après échec. Les fournisseurs système utilisent la base de leur PE mappé comme handle. Recherche de fichiers, chemins non ASCII, options `LoadLibraryEx`, cycles et transitions réentrantes du même module en initialisation/déchargement restent non pris en charge.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` partagent le bloc invité courant des paramètres de processus du PEB. Les noms ASCII ignorent la casse ; les valeurs sont en UTF-16. Les modifications valident les entrées, la capacité et les droits d’écriture avant publication. Les instantanés restent indépendants des modifications et libèrent leur mémoire invitée. Le modèle limite le bloc à 64 KiB ; chaînes et expansions sont bornées et vérifient l’échéance. La propriété inconnue des pointeurs, les blocs mal formés, les pages de codes ANSI et le chevauchement des tampons d’expansion restent non pris en charge. `WindowsEnvironmentTests.cpp` compare des fixtures originales x64/ARM64 sur les backends disponibles ; la CI exige un oracle Windows natif indépendant.

`WindowsProcessHeap` centralise allocation, `HeapReAlloc`, libération et taille du tas du processus. Le redimensionnement conserve les octets retenus ; `HEAP_ZERO_MEMORY` initialise les octets ajoutés et `HEAP_REALLOC_IN_PLACE_ONLY` interdit le déplacement. Un redimensionnement échoué conserve le bloc et renvoie NULL avec `ERROR_NOT_ENOUGH_MEMORY` (8), conformément aux observations natives. Les pages indépendantes restituent leur capacité lors des réductions et libérations ; croissance préparée et copies bornées vérifient l’échéance. Tas personnalisés, indicateurs générant des exceptions, propriété inconnue et plages inaccessibles arrêtent explicitement l’exécution. `WindowsHeapTests.cpp` couvre les deux ISA, déplacement imposé, réutilisation du budget et atomicité des échecs ; CI exécute aussi le même EXE original sur Windows natif. Les cas PE par lots utilisent le délai CTest commun de 120 secondes ; chaque invité conserve son budget fini. La fixture du tas permet 20 secondes par processus pour vérifier toutes les données avec WHP.

`WindowsSystemModules` construit des images modèles PE64 bornées pour `ntdll.dll`, `kernelbase.dll` et `kernel32.dll` sur les deux ISA. Les recherches ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` et `GetProcAddress` partagent leurs bases mappées ; PEB/LDR et `MEM_IMAGE` décrivent les mêmes images. Imports statiques, recherches nommées et redirections invitées partagent les portes API et le résolveur. Les fournisseurs restent résidents, sans rappel invité d’initialisation, et ne bloquent pas le retour du point d’entrée après déchargement des DLL invitées ordinaires. Toute modification des en-têtes ou métadonnées d’export arrête la recherche. Noms système non modélisés et ordinaux non nuls arrêtent explicitement l’exécution ; une différence de casse d’un nom modélisé ou un nom vide renvoie 127, une requête NULL renvoie 87. Octets et adresses générés relèvent du modèle ; dispositions propres aux versions Windows, ordinaux natifs et alias entre fournisseurs ne sont pas reconstruits. `WindowsSystemTests.cpp` compare des EXE originaux x64/ARM64 à Windows natif, avec huit observations indépendantes du retour du thread initial.

`WindowsProcessExceptions` implémente `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` et `RaiseException` sur le même CPU et budget de processus. Les gestionnaires ordonnés peuvent modifier les inscriptions, lever des exceptions imbriquées, appeler les API modélisées, charger des DLL et terminer le processus. Les violations de données x64/ARM64 et divisions entières x64 reprennent après validation des modifications de `CONTEXT`, en conservant registres généraux, SIMD et état FP pris en charge. Les exceptions logicielles reprennent via une véritable instruction de retour du fournisseur modélisé. Limites : 128 inscriptions conservées et 16 cadres imbriqués. Dispositions invalides, pointeurs modifiés, champs non pris en charge et dépassements échouent explicitement. SEH ARM64 et déroulement fondés sur la pile, débogage et fautes d’exécution/de garde restent non pris en charge. `WindowsExceptionTests.cpp` compare des EXE/DLL originaux à Windows natif ; les preuves ARM64 KVM/WHP natives restent à obtenir. Les enregistrements des exceptions logicielles portent `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), indépendamment du drapeau de non-continuation fourni par l’appelant ; l’exécutable Windows original vérifie les valeurs exactes des drapeaux des exceptions logicielles et matérielles.

`AddVectoredContinueHandler` et `RemoveVectoredContinueHandler` gèrent une liste ordonnée distincte, avec une limite commune de 128 inscriptions conservées avec les gestionnaires d’exception. Après acceptation de la reprise par un gestionnaire d’exception vectorisé, les callbacks de continuation voient le même enregistrement modifiable et le même `CONTEXT`. La validation finale suit ces callbacks, y compris les exceptions imbriquées et les notifications DLL. Un handle ne peut pas être retiré par l’autre famille de gestionnaires. `WindowsContinuationTests.cpp` compare des EXE originaux à Windows natif pour l’ordre, l’arrêt anticipé, les modifications de liste, la réparation du contexte, les appels imbriqués, le chargement et la sortie du processus. Le chemin vectorisé Windows x64 testé autorise la reprise avec `EXCEPTION_NONCONTINUABLE` ; cela ne prouve pas le comportement SEH fondé sur la pile. L’exécution ARM64 native reste non vérifiée.

`WindowsProcessSEH` utilise le `X64SEH` partagé dans `os/windows/exception/` (`NeverDEmulationWindowsException`, disponible sans pilotes) pour x64 `__C_specific_handler` et UNWIND_INFO V1. Après VEH, il gère filtres, finally, transfert non local, exceptions imbriquées, collisions de déroulement et cadres EXE/DLL relocalisés, avec conservation des GPR/XMM non volatils. Une continuation par filtre exécute VCH avec le même `CONTEXT`. `WindowsSEHTests.cpp` compare 14 scénarios originaux à Windows natif ; KVM/WHP/Unicorn partagent cette sémantique. Le budget du processus couvre la revalidation des générations, en-têtes, octets de déroulement/portées, régions du gestionnaire de langage et liaisons IAT. Métadonnées modifiées ou images retenues déchargées provoquent un échec explicite. SEH ARM64 par cadres, C++ EH, tables de fonctions dynamiques, RtlUnwind/NtContinue généraux, reprise des exceptions de cadre non continuables et déroulement traversant des callbacks du chargeur/VEH/VCH restent exclus. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37135388077).

`WindowsDynamicTests.cpp` compare des DLL/EXE x64/ARM64 originaux aux observations Windows natives indépendantes : références, dépendances partagées, chargements imbriqués, nettoyage après échec, redirections, sortie, DLL sans entrée et TLS neuf au rechargement. Les régressions refusent les métadonnées modifiées et pointeurs de code périmés, conservent les budgets cumulatifs et les résultats API interrompus non terminés. La CI Windows impose oracle natif et cas WHP ; compilation croisée et Unicorn ARM64 ne prouvent pas l’exécution native ARM64.

Une bibliothèque absente dans la chaîne de transfert de `GetProcAddress` renvoie 127 ; un `LoadLibrary` explicite pour un module absent du catalogue renvoie 126. L’oracle natif et chaque backend disponible vérifient les 41 scénarios déclarés. Sous Windows, le retour après déchargement de toutes les DLL est observé 16 fois par variante de DLL. Un échec d’initialisation lors d’un transfert de `GetProcAddress` renvoie également 127 après nettoyage. Les rappels de détachement du processus préservent le contenu de la pile de l’appelant qui termine.

`WindowsExportTests.cpp` utilise des DLL et EXE x64/ARM64 originaux pour vérifier code/données/ordinaux redirigés, alias, requêtes d’initialisation, rebasage, casse, absences, LastError, cycles, cibles non résidentes, pointeurs invalides et modifications après une requête réussie. Le même EXE dispose d’un oracle Windows natif indépendant ; les cas WHP sont obligatoires en CI native. Les tests C ABI/CLI comparent les rapports complets. Les preuves matérielles ARM64 natives restent à obtenir. Des variantes EXE avec et sans table d’exports couvrent les deux graphes, l’ordre PEB, le détachement et les erreurs nom/ordinal/NULL.

`WindowsLifetimeTests.cpp` compare des traces figées aux processus Windows natifs indépendants et à KVM/WHP/Unicorn : sortie normale, retour d’entrée, deux échecs DLL, quatre sorties précoces et DLL sans entrée. Il vérifie aussi fautes des callbacks, budgets communs, champs TLS relocalisés et capacité TLS cumulée. La sonde native de retour conserve le handle du thread initial et vérifie 64 fois son code de sortie et la séquence exacte de notifications thread/processus. Les threads enfants restants sont terminés après observation ; la sortie du processus ne représente pas le retour de l’entrée.

La mémoire virtuelle Windows ajoute `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` et `FlushInstructionCache` pour le processus courant. La couche OS possède les réservations ; `AddressSpace` reste la référence pour les pages validées, les permissions et leur stockage. Les tests couvrent la réécriture de code, les défauts d’accès et la réutilisation du budget mémoire.

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

Les régressions couvrent budgets exacts ou insuffisants, mots partiels, deux ordres d’octets, identité de mémoire intacte, bornes des sauvegardes affines, écrasements par prédécesseurs tardifs et invalidation par défaut. C API/CLI vérifient la compatibilité v6 et les domaines invalides. HighC et LLVMC exécutés en O0/O2 contrôlent retour, mémoire, pile et état préservé, sans constituer un certificat d’équivalence native.

Les régressions couvrent phases en registre et en pile, deux boutismes, branches invalides accessibles, prédécesseurs tardifs, gardes internes épuisées et limites de découverte adjacentes. Les tests de visites couvrent corrélations arithmétiques, boucles imbriquées, modes, passages séquentiels, budget total et priorité historique. Le CLI exécute les deux chemins C et ABI source à O0/O2 ; C/Python v8 vérifient dispositions, champs invalides et extensions futures ignorées. Les régressions vérifient aussi que de grands sélecteurs finis sans rapport laissent du budget aux gardes natives et que le dernier raffinement autorisé revient à un producteur déjà proposé.
