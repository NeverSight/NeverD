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

Les régressions supplémentaires conservent les intrinsics de calcul sur les bits dans les deux modes mémoire et ordres des octets. Les appels ordinaires, operand bundles, contrats convergents, durées de vie, effets mémoire et traps restent des barrières. Le code original et transformé est exécuté à O0/O2 avec traps de comportement indéfini, contre un oracle indépendant vérifiant l’adresse numérique dans le résultat et chaque octet du tampon.

Les régressions de vivacité par octet couvrent lectures disjointes, écritures partiellement observées et couverture par plusieurs écritures. Les adresses gardées couvrent AND/OR/XOR, 32/64 bits, les deux ordres des octets, racines incorrectes, masques incomplets, jonctions contournant le test et toutes les limites de budget. Les oracles indépendants O0/O2 vérifient les deux branches, les seize résidus d’adresse et chaque octet du tampon.

Les régressions des relations d’adresse couvrent PHI/select entiers et pointeurs, les deux ordres des octets et largeurs de pointeur, les retours de boucle stables ou variables, undef/freeze, les cycles sans ancrage, les budgets adjacents et l’invalidation pour un seul changement d’adresse. À O0/O2, les boucles comparent chaque résultat et chaque octet du tampon à une référence indépendante par itération, avec une adresse mobile qu’il ne faut pas considérer constante.

Les tests numériques couvrent le transfert entier ou partiel depuis un écrivain, undef/poison sans nouvel instantané, les deux ordres d’octets, 32/64 bits, les offsets négatifs modulaires, les chevauchements partiels, les alias entre racines et allocas, les exceptions, les boucles, les limites de budget adjacentes et l’invalidation après seule suppression de stores. À O0/O2, original et transformation sont comparés à un oracle indépendant pour le résultat et chaque octet du tampon aliasé.

`NeverDMedMutableSourceTests` et `NeverDLLVMCValueTests` exécutent à O0/O2 des boucles indépendantes, blocs réordonnés, retours vers l’entrée, calculs de pile à l’exécution, lectures antérieures, jonctions, alias partiels, valeurs logiques et comptages de bits incluant zéro. Les cas négatifs refusent avant émission les entrées mal formées, cibles tronquées, supports ambigus et budgets épuisés. Un cas CLI dépassant la limite SSA exige une sortie LLVMC exécutable et un refus explicite de HighC. Les mises à jour répétées et les chaînes d’expressions stockées entre blocs vérifient aussi la taille et l’exécution du C généré.

Des régressions supplémentaires bornent les lectures et écritures privées avant promotion LLVM et la taille du C. Elles exécutent à O0/O2 de longues chaînes arithmétiques mixtes, des blocs SSA réordonnés, des écritures mémoire qui se chevauchent et des retours zéro. Les cas clés passent aussi par le véritable pipeline d’optimisation LLVM ; la réutilisation de l’émetteur après un refus de génération est vérifiée.

Les régressions de conditions composées exécutent à O0/O2 les conjonctions et disjonctions avec égalité à une constante non nulle, comparaisons non signées, comparaisons signées dans les deux ordres, booléens élargis et toutes les combinaisons de négation. Le C doit conserver toute la table de vérité sans déréférencer un opérande absent de comparaison à zéro. Les écritures d’adresses entières couvrent les valeurs alignées et non alignées sur 32/64/128 bits. Les tableaux d’octets conservent leur alignement explicite et les accès exacts à la base et partiels, sans affectation scalaire au tableau ni alias par des types incompatibles.

`NeverDLowIRRefinementTests` couvre les graphes réellement reconstruits, les boucles finies de structures différentes et leurs cas sans itération, les producteurs dynamiques, les choix conditionnels, les vues d’entrée superposées, les copies et sauvegardes corrélées, les preuves de lecture immuable des deux côtés, les drapeaux système et la préservation du retour. Candidats erronés, écritures supplémentaires, chemins incomplets ou infinis, preuves périmées, collisions temporaires et budgets partagés épuisés doivent refuser le certificat. Les tests d’indépendance existants refusent toujours les valeurs arbitraires observables.

Les cas `CompleteModel`, `CompletedTargetFacts`, `ConditionalImplication` et `PartitionedCoverage` de `NeverDLowIRRefinementTests` utilisent des oracles exhaustifs indépendants sur de petits domaines et vérifient les entrées malformées, les caches périmés, les énumérations incomplètes et les budgets exacts/insuffisants. Les tests réels LowIR et binaires vérifient les produits conditionnels et toutes les branches terminales originales/reconstruites sous un plafond fixe de portes, en rejetant les observations finales modifiées, les domaines sans rapport et les cibles absentes. `FiniteValues` distingue l’échec d’encodage des refus liés à la recherche, au nombre de valeurs et au budget global.

Dans la même cible, `LowIRLoopRefinement.*` et `BinaryLowIRLoopRefinement.*` couvrent les compteurs arbitraires sur 64 bits, les rangs lexicographiques imbriqués, les résidus natifs réels, les préfixes d’entrée, les vues superposées et les sauvegardes corrélées. Les contrôles négatifs rejettent corps incorrects, domaines d’entrée réduits, rangs non décroissants, rebouclages non signés, écritures antérieures oubliées, coupures absentes, modèles malformés et budgets partagés épuisés. Un chemin frère fini réussi ne valide jamais une induction incomplète.

`LowIRLoopInference.*` et `BinaryLowIRLoopInference.*` utilisent des compteurs, sauvegardes sur pile, retours anticipés, appels natifs et drapeaux compactés écrits indépendamment. Ils couvrent l’élargissement arithmétique étroit et les drapeaux égaux malgré des expressions différentes. Graphes malformés, origines absentes ou falsifiées, boucles infinies ou avec rebouclage et budgets épuisés ne doivent produire aucun certificat.

Les régressions de préfixe zéro couvrent le regroupement, les largeurs inhabituelles et toutes les paires d’octets, en conservant les bits inconnus et non nuls. Des boucles de cadre indépendantes vérifient les écritures séparées de valeur basse et de zéros hauts dans les deux ordres d’octets, les erreurs de calcul et de remplissage, les budgets exacts ou insuffisants et le budget distinct de la preuve complète.

Les régressions à en-tête et retour partagés couvrent les compteurs 32 bits étendus par zéros et 64 bits complets, les rangs scalaires à pas non unitaire, les résultats erronés, les chemins sans progrès ou avec rebouclage modulaire, et les budgets exacts ou épuisés entre recherches scalaire et par tuples. `LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`. Des régressions supplémentaires d’incrémentation et de remise à zéro exigent une convergence sans dérouler un bit de compteur par tour et rejettent l’absence de progrès et le rebouclage non signé. Les régressions d’ordonnancement couvrent les accumulateurs à pas non unitaire, le bruit de compteurs unitaires pouvant reboucler près d’un rang scalaire valide à pas non unitaire, et trois compteurs dont le tuple valide dépasse la fenêtre initiale. Des budgets de rang exacts et réduits d’un essai vérifient une reprise déterministe sans répétition.

`LowIRLoopPlanPairing.*`, dans la même cible, vérifie le renommage des registres, des corps arithmétiques différents, les préfixes propres à chaque côté, les prédicats conservés, les entrées mémoire partagées, les coupures imbriquées et les budgets de preuve indépendants. Relations manquantes, écritures incorrectes, temporaires mal liés, appariements incomplets et limites de métadonnées épuisées ne doivent produire aucun certificat.

`LowIRLoopAlignment.*` vérifie des boucles à compteur en frame ordinaires et tournées, écrites indépendamment : chaque plan par défaut se prouve séparément, le premier appariement échoue, puis une autre coupure candidate établit la relation. Les régressions couvrent permutations de plusieurs coupures, résultats et écritures erronés, relevés originaux manquants ou périmés, witnesses explicites de valeurs indéfinies, compteurs non décroissants ou avec rebouclage, graphes malformés, cumul des requêtes après échec, budget total exact et limites épuisées. Aucun refus ne doit contenir de certificat. Les nouveaux cas vérifient les phases distinctes de remise à zéro et de progression, le déplacement équivalent des gardes de sortie nécessitant un appariement entre familles, le cache sans répétition et le dépassement cumulé des métadonnées. Un cycle indépendant ultérieur vérifie la couverture complète avec une limite explicite de 16384 requêtes d’inférence. Les familles vides ou redondantes ne font aucune requête symbolique ; des coupures insuffisantes sont refusées. Budgets globaux exacts ou réduits d’une tentative, résultats erronés, absence de progression, preuves originales et witness indéfinis restent contrôlés. Les régressions du filtre couvrent les losanges arithmétiques neutres, les convergences locales ou limitées à la frontière et un point de jonction accessible mais contournable vers une sortie ou une frontière de boucle. Elles vérifient la recherche du candidat filtré malgré une famille originale redondante, la réutilisation d’un plan filtré avant une tentative complète ultérieure, les limites exacte, réduite d’une unité ou nulle de `MaxCutSelectionWork`, le cumul des échecs dans `CutSelectionWork` et l’arrêt avant toute inférence symbolique lorsque le travail global est épuisé. La couverture complète des cycles est vérifiée pour les deux familles de branches ; le losange utilise des limites explicites de requêtes d’inférence et de preuve.

Les régressions couvrent tranches de frame et de registre, deux directions, positions basses/intermédiaires/hautes, largeurs inhabituelles, deux ordres des octets et mots de frame de trois octets. Elles vérifient découverte tardive, mutations des bits préservés, absence de progrès, rebouclage sans garde, entrées supplémentaires invalides, budgets exacts/insuffisants et recherche à coupure unique.

Les régressions de phase initiale couvrent deux ou trois boucles successives réutilisant un mot de compte à rebours, leur combinaison avec des phases de boucles imbriquées, les budgets de rang et de requêtes exacts ou réduits d’une tentative, les boucles sans progression et les remises à zéro revenant à une phase antérieure. Des constantes de phase erronées de même largeur, des résultats incorrects ou des écritures de frame incorrectes doivent être refusés sans certificat par le vérificateur complet ; les preuves originales manquantes restent non prises en charge.

`InterpreterMachineStateModel.*` dans `NeverDLowIRRefinementTests` utilise des exemples LowIR indépendants : drapeaux d’entrée bruts, statut distinct de RAX invité, 17 mots d’état, sous-registres, drapeaux empaquetés, rejet dynamique persistant, écritures du cadre invité, deux branches et inférence cyclique suivie d’une nouvelle preuve. Sorties erronées, statut perdu, mémoire modifiée, enregistrements périmés, entrées malformées et budgets épuisés doivent échouer. Les tests source existants exercent aussi les deux voies C à O0/O2 ; les tests du modèle seuls ne certifient pas le C compilé.

`NeverDLLVMInterpreterModelTests` compare du LLVM indépendant à des oracles LowIR de tout l’état : largeurs, PHI parallèles, switch, mémoire invitée, statut distinct, gardes poison, plages intrinsèques, contrats refusés et quatre budgets. Il vérifie une preuve complète de décompte sur un mot arbitraire et rejette un statut modifié. Du C indépendant compilé à O1/O2 doit respecter les mêmes observations. Ces tests valident le modèle admis ; découverte automatique d’invariants et correction du compilateur restent séparées. Les cas de décalage variable couvrent les quatre largeurs, les comptes bornés par masque ou branchement, les valeurs limites et excessives, les indicateurs sans débordement et exacts, le refus strict du poison et du C compilé en O1/O2.

`NeverDLLVMScalarEquivalenceTests` vérifie les domaines complets des boucles, zéro itération, les échanges PHI simultanés, switch, les bits hauts d’entrée, les contre-exemples de dernière partition, les mises à jour supplémentaires produisant poison, les plages de retour, les contrats non pris en charge et les budgets exacts, insuffisants d’une unité ou nuls. Des oracles indépendants de largeur double et de débordement couvrent les extrémités funnel et les produits contraints à chaque largeur admise ; du C indépendant à boucles imbriquées en O1/O2 vérifie le profil d’entrée compilateur. La suite du modèle d’état vérifie aussi les extrémités. `SymExpr.ConstantWindowSharesActualWorkWithoutRelaxingQueryCeilings` vérifie la comptabilité cumulée et les plafonds locaux inchangés.

`LLVMScalarDecision.*` couvre les obligations profondes de décalage exact et d’extension, les branches constantes, les deux arêtes de retour, les bits de données hauts conservés, les opérations indéfinies tardives, la non-terminaison, les modifications après vérification et les budgets exacts, courts ou locaux. `LLVMScalarDecisionCompiled.DeepOneAndTwoBackedgeOracles` compare des récurrences indépendantes à une ou deux arêtes de retour avec un oracle C non signé à O0/O2 sur 32 768 appels. Il s’agit de tests du modèle scalaire, pas d’une couverture de l’ABI native ou de la récupération d’un binaire entier.

`LLVMScalarDemand.*` prouve deux corps de boucle non linéaires sous budget fixe, conserve les seize partitions et les bits hauts libres, refuse les erreurs de sortie ou de définition dans la dernière partition et vérifie les budgets exacts/insuffisants ainsi que les limites de nœuds. `LLVMScalarDemandCompiled.*` les compare à un oracle arithmétique non signé indépendant à O0/O2 sur 262 144 appels. Aucun retrait d’opération source ni restriction du domaine d’entrée ne justifie ces résultats.

`SymKnownBitsTests` vérifie les faits sur toutes les paires d’octets et sur des valeurs limites en précision arbitraire : extensions, sommes sans rebouclage, racines distinctes et décalages totalement définis. Il couvre les budgets exacts ou inférieurs d’une unité, le coût du cache, sa capacité, les contextes distincts, la profondeur, le nombre d’opérandes et les largeurs non prises en charge. `SymExprExtensionTests` vérifie les constantes hautes et les décalages de pleine largeur. Les tests d’équivalence conservent les données symboliques en prouvant les contraintes de plage, rejettent les différences dans la dernière partition et le poison exécuté, et vérifient le budget de la preuve entière. `SymMBAExtensionTests` exige une dérivation sans vérification par échantillons, contrôle toutes les paires d’octets et conserve les limites de signe, de retenue étroite, de complément et d’épuisement du travail.

`NeverDLLVMScalarLoopRecoveryTests` couvre préfixes, états des prédécesseurs, chemins à zéro itération, boucles sur elles-mêmes, états affines, égalité après bouclage arithmétique, poison d'une mise à jour supplémentaire, bits de données hauts et contrats refusés. Des budgets exacts ou réduits d'une unité vérifient le refus atomique. Des oracles arithmétiques indépendants exécutent les LLVM original et reconstruit à O0/O2 pour toutes les entrées de contrôle sur un octet. Cela ne prouve ni récupération de l'ABI native ni sortie C par défaut.

Les régressions couvrent les PHI de 8/16/32/64 bits, comparaisons et polarités inversées, extensions signées ou non, constantes d’entrée distinctes, observations incompatibles, plusieurs retours, contre-exemples symboliques, troncature refusée, poison/non-terminaison et refus atomique aux limites de construction, preuve, candidats et transformations. Des oracles non signés indépendants comparent LLVM original et récupéré à O0/O2 sur 458 752 appels avec pièges de comportement indéfini. Source et module parent restent intacts.

Les tests couvrent gardes modulaires de 8/16/32/64 bits, pas décroissants, polarités, zéro tour, sorties partagées, faux voisins équivalents seulement sur données nulles, bornes inaccessibles, tranches trop grandes, poison, feuilles indisponibles et budgets atomiques. Quarante arguments inutilisés ne doivent pas évincer l’initialisation utile du préfixe. Les oracles non signés indépendants exécutent LLVM original et récupéré à O0/O2 sur 458 752 appels avec pièges de comportement indéfini.

La même cible vérifie aussi `recoverLLVMScalarSource` : préparation avant recherche, nettoyage seul, état inutilisé de pleine largeur, obligations mortes de débordement, décalage exact, division et assume, effets refusés, budgets cumulés exacts ou réduits d’une unité et continuation bornée. Des oracles arithmétiques indépendants exécutent les LLVM original et préparé à O0/O2 pour tous les contrôles d’un octet et des états déterministes de pleine largeur. Source et module parent restent intacts en cas de succès comme de refus.

Les tests de garde couvrent les identités modulaires annotées, les expressions équivalentes de prédécesseurs distincts, les valeurs de branches différentes, les refus de débordement/décalage exact/troncature/extension d’origine et les budgets cumulés exacts ou insuffisants. Des oracles non signés indépendants comparent les corps originaux et préparés à O0/O2 sur 131 072 appels avec pièges de comportement indéfini. Les tests existants conservent les refus de la passe autonome.

Les régressions des masques couvrent les opérandes permutés, les champs nuls, les bits hauts d’entrée conservés, une alternative après échec sur les données complètes, tous les arcs de retour, le refus des rebouclages/débordements, plus de 32 propositions et les budgets exacts ou inférieurs d’une unité. Des oracles arithmétiques LLVM et C indépendants à O0/O2 vérifient la composition avec la réduction de largeur. Les requêtes réflexives conservent les domaines complets, le refus de poison/undef et des contrats non pris en charge, la non-terminaison, les plafonds locaux et la comptabilité exacte ; modifier la même fonction invalide le résultat antérieur. Cette couverture LLVM scalaire ne certifie pas l’ABI native.

Les tests de confinement des masques épuisent toutes les paires d’octets, couvrent les masques non contigus et les largeurs jusqu’à 128 bits, préservent les bits inconnus/hauts et bornent la croissance des nœuds au-delà des plafonds. Une boucle symbolique à deux arcs de retour doit prouver sa récurrence XOR masquée contre une forme fermée indépendante. La découverte des dépendances facture le stockage normalisé des décalages tout en gardant les budgets exacts/courts et le repli conservateur pour les valeurs larges.

Les régressions de largeur couvrent les constantes non nulles, signatures inchangées, entrées et intrinsèques plus larges, bits hauts observables, ordre signé, nouveaux débordements et budgets exacts/insuffisants. Les mêmes candidats scalaires sont testés sous les triples x86-64, AArch64, AArch64 big-endian et ARM32 ; cette couverture LLVM ne certifie pas l’ABI native. Des oracles arithmétiques indépendants exécutent LLVM original/reconstruit et C émis en O0/O2, avec pièges de comportement indéfini pour C.

Les régressions couvrent toutes les entrées, plusieurs retours de boucle, les données hautes cachées, poison, échanges simultanés, division des lots, plus de 32 variables et budgets atomiques. Les preuves scalaires distinguent une requête inconnue terminée de l’épuisement global et prouvent les décalages sûrs sans énumérer les bits de données. Les tests symboliques énumèrent valeurs d’octet, masques et comptes tout en conservant bits observables, identité des sources et grands comptes. Les vues de largeurs différentes partagent la valeur numérique complète du compte. Ces contrôles ne certifient pas l’ABI native.

Les régressions supplémentaires couvrent les derniers indices étroits avec rebouclage, les blocs body/latch séparés, les deux polarités des gardes, les opérandes d’égalité inversés, les récurrences unitaires réordonnées ou décroissantes, les différences dans les bits hauts sur le chemin vide, le poison nouvellement exécuté et les mauvaises bornes. Les budgets exacts ou réduits d’une unité et l’épuisement des candidats conservent le refus atomique. Le LLVM original et le C produit sont exécutés à O0/O2 face à des oracles arithmétiques indépendants.

`NeverDLLVMCScalarLoopRecoveryTests` vérifie les sorties par défaut complètes et ciblées, la reprise après nettoyage des retours, identité et attributs, appelants, intrinsèques existants ou nouveaux et collisions, budgets partagés, appels à effets, entrées sans garantie de définition, métadonnées, images, adresses de blocs externes et sélections étrangères. Des oracles indépendants d’arithmétique et de rotation exécutent le C à O0/O2 avec pièges de comportement indéfini. L’arithmétique compare aussi le LLVM original compilé séparément sur tous les contrôles d’un octet, valeurs limites et données déterministes de pleine largeur.

`SymSimplifyPredicates.*` compare aussi les politiques de la phase autonome et de la passe complète, le travail rapporté, les budgets exacts ou insuffisants d’une unité, la phase désactivée et les fonctions marquées comme obfusquées. Les régressions du source scalaire couvrent les conditions d’arrêt de boucle encodées arithmétiquement sur 8/32/64 bits, le refus des débordements signés et les limites de construction partagées entre itérations et fonctions. Un échec de publication conserve l’IR original ; le C émis est exécuté à O0/O2 face au LLVM original compilé indépendamment et à un oracle arithmétique.

`SymKnownBits.*` vérifie les masques sans perte et les décalages signés aller-retour par une arithmétique exhaustive sur les paires d’octets : valeurs négatives, sources différentes, bits inconnus supprimés, facteurs/comptes incompatibles et décalages excessifs larges. Jusqu’à 128 bits, les budgets exacts ou insuffisants d’une unité et l’absence de croissance du DAG restent vérifiés. Les tests de décision scalaire ajoutent des mises à jour positives et négatives sur deux arêtes de retour, les refus de faits périmés et de débordements, des bits hauts symboliques et 16 384 appels à O0/O2 comparés à un oracle non signé indépendant.

`SymKnownBits.*` vérifie aussi exhaustivement les paires d’octets pour l’ordre des multiples et les produits entre largeurs, refusant débordements, coefficients ou multiplicités incorrects et déplacement d’un débordement étroit vers un mot plus large. Jusqu’à 128 bits, les requêtes gardent les budgets exacts ou insuffisants d’une unité et un DAG inchangé. Les tests scalaires prouvent additions répétées et multiplications sans énumérer les bits de données, conservent chaque obligation de débordement des boucles et exécutent 16 384 appels O0/O2 contre un oracle indépendant.

`LLVMScalarAssume*` vérifie les domaines complets de boucle, les échecs de la dernière partition, les conditions fausses inaccessibles ou atteintes, la définissabilité cumulative pour toutes les entrées d’un octet, les budgets exacts ou réduits d’une unité, les modifications d’IR et les contrats d’appel non pris en charge. Quatre triples cibles exercent le modèle partagé ; 8 192 appels O0/O2 sont comparés à un oracle non signé indépendant. La suite du modèle d’état vérifie séparément la même obligation et le refus des bundles d’opérandes.

`LLVMScalarProjection.*` couvre champs imbriqués, fenêtres, arguments inutilisés conservés, retours multiples, arêtes arrière, obligations overflow/shift/assume non sélectionnées, échec de dernière partition, non-terminaison, contrats inconnus, entrée modifiée et budgets exacts/insuffisants d’une unité. Quatre triplets testent la sémantique partagée. `LLVMScalarProjectionCompiled.*` compare l’agrégat original via un pont tableau LLVM et les projections à une arithmétique non signée indépendante en O0/O2. `SymExpr.RightShift*` épuise les paires d’octets et vérifie extension signée, retenues, bits hauts conservés, comptes complets et limites de recherche.

`LLVMScalarInputs.*` vérifie les correspondances ordonnées de largeurs mixtes, les interfaces sans argument ou sans nom, les besoins de l’arithmétique morte et d’assume, les sorties modifiées, contrats inconnus, refus d’agrégats et budgets cumulés exacts ou insuffisants. Les preuves restaurent la signature complète sans fixer les entrées omises. `LLVMScalarInputsCompiled.*` compare les boucles originales et réduites à un oracle non signé indépendant à O0/O2 en variant tous les arguments omis.

`NeverDLLVMScalarStateProjectionTests` couvre fenêtres superposées/non alignées, cellules 8/16/32/64 bits, boucles, masques, modifications de source, plages de statut, poison conservé, mémoire externe et budgets exacts/insuffisants. Les corps mémoire et ponts agrégés LLVM effectuent 172 032 comparaisons O0/O2 avec des oracles indépendants ; les preuves scalaires restent distinctes. `SymKnownBits.*` épuise les paires d’octets et vérifie 128 bits, facteurs différents, masques élargis, sommes débordantes et budgets. `LLVMCIntrinsicSemantics.AssumeEvaluatesItsConditionAndRefusesBundles` vérifie l’évaluation unique à O0/O2 et le refus des bundles.

Les régressions de métadonnées de boucle comparent des boucles comptées à une formule indépendante sur toutes les partitions de contrôle et avec des budgets exacts ou réduits d’une unité. Un historique de peeling grand ou nul ne masque ni résultat erroné, ni non-terminaison, ni poison. Des métadonnées mal formées construites par API vérifient le refus de l’importateur séparément de l’analyse d’assemblage LLVM ; les tests d’état machine préservent les effets d’état et les limites d’entrée.

Les régressions couvrent les plages partielles et séparées, les alias fixes, les deux branches, chaque retour, les lectures à la première itération et les écritures précédant les lectures dans une boucle. Lecture avant écriture, écriture manquante, écriture invitée, alias inconnu, accès spécial, plage hors objet et épuisement des budgets doivent échouer. Un exemple C indépendant qui écrit un mot d’état sans le lire est compilé à O1/O2 ; il conserve les attributs LLVM exacts et passe une nouvelle preuve composée du natif vers LLVM.

Les tests de décompte gardé couvrent la reprise après rejet du modèle du corps, une preuve complète sur un mot arbitraire à l’en-tête, les budgets partagés et le refus immédiat d’une violation réelle du contrat d’entrée.

`NeverDInterpreterLLVMRefinementTests` vérifie la composition nouvelle, la liaison texte/fonction exacte, les budgets indépendants, toutes les observations et le domaine source élargi. Des octets, résidus, résultats, drapeaux, statuts, écritures, poison ou plans faux/périmés doivent empêcher l’attestation composée. Le décompte sur un mot arbitraire exige les deux prémisses inductives ; des exemples C indépendants compilés en O1/O2 vérifient le LLVM sérialisé réel. Les régressions refusent les retours d’entrée cachés et bornent les racines sans copier la provenance accessoire.

`InterpreterLLVMRefinement.Preservation*` couvre plages partielles ou superposées, requêtes invalides, coût de préparation calculé indépendamment, altérations finales identiques, sauvegarde/restauration des valeurs d’entrée à travers les boucles, preuves opaques nouvelles et refus tardifs. Reconstruire aussi le consommateur d’API `NeverDPEFixedImageTests`. Comparer séparément résultats, compteurs et résumés sans requête à la référence.

`InterpreterLLVMRefinement.Collection*` vérifie rétention et report nécessaires aux preuves finies et inductives, branches invalides accessibles, refus source tardif, préservation à l’entrée et les quatre identités des options natives. Compiler des fautes omettant leur transmission dans le module de composition pour vérifier chaque option nécessaire.

```sh
cmake --build build-release --target NeverDLLVMCScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMCScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarEquivalenceTests --parallel 4
build-release/bin/NeverDLLVMScalarEquivalenceTests
cmake --build build-release --target NeverDLLVMScalarResultProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarResultProjectionTests
cmake --build build-release --target NeverDLLVMScalarStateProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarStateProjectionTests
cmake --build build-release --target NeverDLLVMScalarInputProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarInputProjectionTests
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

Les régressions de retour physique couvrent les appels directs et indirects sautant des octets invalides, les mauvaises continuations atteignables, l’énumération complète et les budgets exacts ou insuffisants. Le raffinement de l’état complet refuse les résultats modifiés. Le C indépendant compilé à O1/O2 doit préserver toute l’écriture du cadre ; un résultat identique ne masque pas un octet modifié dans la case de retour.

Les tests explicites de chevauchement natif couvrent de vrais branchements x64 dans des opérandes immédiats, les résultats des deux chemins possibles et les entrées de retour indirectes dans des instructions précédentes. Les fournisseurs synthétiques vérifient les chevauchements inclus dans les deux ordres de collecte, les octets contradictoires sur une branche directe non prise, la cohérence code/lecture dans les deux ordres et les lectures du candidat. Les budgets exacts et insuffisants comptent aussi les octets répétés après des transferts indirects. Une modification des résultats, un usage statique ou en boucle et des preuves contradictoires doivent refuser le certificat ; modifier l’option ou la limite change les condensats.

Les tests de frontières explicites couvrent RCL, XADD mémoire LOCK et REP MOVS inaccessibles, les contradictions symboliques de chemins, les branches dépendant de valeurs arbitraires et les refus exacts depuis l’entrée, un saut indirect, CALL ou RET. Ils vérifient l’accès indépendant à un suffixe accessible, les collisions d’adresses candidat/natif, les preuves mal formées ou partielles, les budgets épuisés, le refus des API statiques/de boucles et les trois couches d’empreintes du raffinement. Modifier une instruction inaccessible ou activer l’option sans frontière retenue change les empreintes. Ces tests valident la portée finie déclarée, pas la sémantique des instructions non auditées.

Les tests de drapeaux regroupés couvrent toutes les combinaisons d’entrée scalaires, les masques de privilège, TF/AC dans les deux exécutions, les producteurs indéfinis distincts, les copies corrélées, les appels natifs, l’état des branches sœurs, l’observation finale obligatoire, les preuves malformées et les budgets. Toutes les entrées réalisables des boucles finies doivent terminer ; une branche sûre ne masque pas un chemin infini ou tronqué. RDSSPD/RDSSPQ couvre les 16 registres généraux, les deux largeurs, les bits hauts conservés, les preuves `Missing` préservées et les projections falsifiées. Les tests d’état machine comparent les deux sorties C à O0/O2 avec pièges de comportement indéfini à un oracle indépendant des drapeaux utilisateur, et vérifient la persistance des violations du profil. Les tests INCSSPD/INCSSPQ couvrent les deux largeurs et tous les registres généraux, les limites inaccessibles conservées, les pièges réalisables après une branche sœur terminée, les opérandes nuls et les preuves de piège falsifiées.

`NeverDX86DecodeDetailTests` couvre les trois voies de décodage, les deux largeurs d’adresse x64, les bornes signées, les préfixes obligatoires, disp16 i386, moffs, les entrées tronquées et la réutilisation sans détails. Seules les occurrences exactes de relocalisation sont liées ; largeur, position ou valeur incorrectes sont refusées. Les tests natifs d’indépendance et de raffinement conservent toutes les écritures du cadre et refusent les drapeaux arbitraires observés ainsi qu’un candidat de décalage modifié.

`NeverDLowUndefinedDigestTests` vérifie des vecteurs SHA-256 indépendants, chaque champ stocké, les bits de séquence signée, l’ordre, l’exclusion du remplissage et l’absence de modification des entrées. Les cas couvrent l’agrandissement du tampon intégré, la frontière 199/200 opérations et les séquences incrémentales plus longues. `LowIRRefinement.StaleUnusedInputRefusesAcrossDigestStorageBoundaries` vérifie le rejet réel de preuves périmées et leur réassociation sur les deux chemins. Conserver `InputDigest.*` dans `NeverDLiftTests` et reconstruire les appelants concernés après modification de l’implémentation séparée. Indiquer la couverture réelle des sanitizers, du chemin portable et des hôtes ; ces microbenchmarks ne certifient pas l’équivalence native.

```bash
cmake --build build-release --target NeverDLowUndefinedDigestTests --parallel 4
build-release/bin/NeverDLowUndefinedDigestTests
```

`NeverDX86UndefinedEffectsTests` vérifie les métadonnées des bits indéfinis, les drapeaux définis ou conservés et le refus des certificats périmés. `NeverDX86CarryArithmeticFlagTests` compare la retenue auxiliaire d’ADC/SBB, pour les formes registre et mémoire, à un oracle arithmétique. `NeverDX86LogicIdentityTests` vérifie qu’AND avec deux opérandes identiques efface encore les bits 63:32 du registre de 64 bits correspondant lors de l’écriture d’une destination de 32 bits en mode 64 bits, tout en préservant les bits non écrits des destinations plus étroites.

`X86RotateUndefinedEffects.*` compare tous les comptes bruts, largeurs, chevauchements de CL, alias d’octet haut et destinations mémoire à un oracle arithmétique scalaire. `X86BitTestUndefinedEffects.*` couvre les index registre/immédiat, le chevauchement source/destination, les registres étendus, les indicateurs définis et les écritures des parties hautes. Les contrôles refusent les opérandes, encodages et formes non pris en charge modifiés. Les preuves natives distinguent lectures corrélées et nouveaux bits indépendants, vérifient les budgets exacts/insuffisants et refusent un débordement indéfini observable. Le raffinement de l’état complet accepte le témoin sélectionné et refuse les témoins zéro ou les candidats altérés.

`X86XaddAudit.*` vérifie les 65,536 paires d’opérandes octet, les limites des indicateurs aux largeurs supérieures, les recouvrements de registres/octets hauts, les deux écritures, les limites de largeur octet sous REX et la préservation des registres complets face à un oracle arithmétique non signé. Les contrôles natifs exigent zéro nouveau bit arbitraire sans perdre les dépendances antérieures. Les deux témoins acceptent XADD inchangé et refusent une somme, une source échangée ou un indicateur défini modifié. L’alias `/6` utilise la matrice complète des compteurs de décalage ; les groupes/ID décodés modifiés doivent être refusés. Les tests mémoire couvrent aussi toutes les paires d’octets, les changements de taille d’adresse, extensions, adresses relatives à IP et i386 16 bits, déplacements signés, octets voisins et détails périmés. Les preuves natives du cadre complet conservent les dépendances arbitraires antérieures et les limites exactes/insuffisantes ; modifier l’adresse du stockage viole le contrat du retour.

`X86DoubleShiftUndefinedEffects.*` vérifie chaque compteur octet aux largeurs 16/32/64 par des transferts indépendants bit à bit, avec alias source/destination/CL et gardes exactes. Les preuves natives isolent les indicateurs après suppression de RAX, distinguent 16 de 17 et conservent dépendances antérieures et budgets exacts/insuffisants. Les relations complètes refusent les tranches définies modifiées et un témoin nul ; un mot bas arbitraire ne permet pas d’effacer les bits hauts définis. Les formes malformées ne publient aucune preuve partielle.

`*Deferred*` couvre branches et continuations mortes, code absent ou malformé, gardes symboliques contradictoires, contrôle arbitraire, témoins et mutations de l’état complet. Les fournisseurs synthétiques ne chargent pas les successeurs morts et refusent les métadonnées atteintes malformées. Les budgets exacts/insuffisants d’instructions, opérations, visites et requêtes, les alternatives invalides faisables et les boucles infinies ne certifient aucun préfixe. La politique modifie les empreintes ; les API statiques/de boucles refusent l’option.

`NeverDPEFixedImageTests` utilise des fichiers PE construits indépendamment pour vérifier les instructions relocalisées, les données immuables, les écritures des imports, les en-têtes/tables malformés, les alias et la provenance modifiée. Les preuves natives vers LowIR et LLVM exact acceptent les candidats conformes et refusent les résultats, statuts ou octets natifs modifiés. L’épuisement du budget de préparation reste distinct et permet un nouvel essai avec des limites explicitement relevées ; le chargement ordinaire accepte aussi 40000 relocations valides au-delà du budget d’analyse par défaut.

`FrameOffsets.*`, `NativeStackSpecialization.*` et `OriginalBinaryUndefinedIndependence.*` vérifient tous les résidus des alignements 2/4/8/16/32, les bits hauts libres, les sauvegardes entre appels, les boucles de décompte, les corruptions par alias, les sélections erronées, les grands masques inutiles, les élargissements nécessaires et les budgets exacts ou réduits d’une unité. Des contrôles natifs distincts couvrent l’alignement conditionnel, le nettoyage interne non signé, le nettoyage erroné et les retours préfixés. Ces tests ne prouvent pas la couverture automatique natif-vers-LLVM des boucles partitionnées.

Les régressions natives couvrent 64 lectures alignées avec le budget d’une lecture, un budget inférieur de un, des adresses modifiées hors cadre et une même adresse sous des prédicats différents après le retour d’un chemin. Les tests de mutation d’état, de clés et de capacité restent requis.

Les régressions de faisabilité répétée conservent les 130 instructions natives avec le budget de requêtes de deux instructions linéaires. Elles refusent les budgets de requêtes/instructions inférieurs de un, les pièges accessibles après changement de branche ou de domaine d’entrée, les portes du solveur épuisées et les états candidats modifiés.

Les régressions vérifient 558 combinaisons de largeur, alignement, résidu et biais sous un budget de portes insuffisant pour soustraire les racines complètes. Elles couvrent tranches de sources/biais différents, masques clairsemés non contraints, retenues et débordements modulaires, masques imbriqués et épuisement des nœuds/requêtes. Les tests natifs vérifient les stores via pointeurs partiellement alignés et refusent alignement absent, accès hors trame et valeurs stockées modifiées dans le raffinement complet.

Les tests couvrent tous les restes pour 1/2/4/16, deux racines, bits supérieurs libres, domaines invalides, constantes contradictoires, bornes, trous d’exclusion, préservation et budgets exacts/moins un. Les modèles inductifs conservent le prédicat initial. Les preuves natives et LLVM fraîches refusent domaines divergents et statuts modifiés ; les condensats lient les deux domaines. Les cas sont indépendants, sans déduction depuis une ABI ou une exécution. Un C indépendant avec garde est compilé sans modification à O1/O2 et prouvé face aux instructions natives réelles pour deux restes ; les mêmes artefacts doivent être refusés pour un autre reste.

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

`NeverDParallelExecutionTests` force le chevauchement des appels, l’annulation d’un écrivain en attente, les états CPU indépendants, la concurrence par alias et la préparation privée. `NeverDRunControlTests` vérifie deux baux WHP indépendants simultanés et la sérialisation du cache commun. `NeverDMMIOAtomicTests` compare RAM/périphérique pour les instructions x64 atomiques/de mise à jour et tous les cas ARM64 LSE, les deux observations larges, les aperçus périmés, les erreurs et la course commit/stop. `KernelMMIOFailure` couvre alias, écritures identiques, double commit, alimentation, unmap et propriétaire détruit. Les plateformes absentes sont explicitement ignorées. Le rendez-vous du wrapper prouve des appels concurrents, pas un retrait matériel simultané. KVM/WHP ARM64 exige l’hôte correspondant.

```bash
cmake --build build-cpu --target NeverDParallelExecutionTests NeverDMMIOAtomicTests NeverDRunControlTests --parallel 4
ctest --test-dir build-cpu/unittests/emulation -L '^NeverD(ParallelExecution|MMIOAtomic|RunControl)Tests$' --output-on-failure
```

## Tests du profil de processus Linux

Les [suites indépendantes de processus](process-emulation.md) compilent de vraies fixtures ELF x64/AArch64. `NeverDLinuxProcessTests` vérifie démarrage, politique des en-têtes, requêtes de service, sortie binaire, défauts et limites. `NeverDProcessPublicTests` contrôle l’API C/CLI sans modifier l’image d’analyse. `NeverDExecutionSessionTests` couvre deux CPU partageant mémoire/budget et la consommation exactement unique des requêtes/défauts. `NeverDX64MemoryUpdateTests` contrôle arithmétique mémoire, SETcc, BT, XMM/MXCSR, observateurs d’écriture, frontières REP et lectures préparées de périphériques. `DriverBackendParityTests.cpp` exécute les fixtures WDK originales et relocalisées puis compare le rapport observable complet à Unicorn ; images/backends absents sont ignorés.

Le x64 vérifié admet aussi les formes masquées historiques `SS`, `SD`, `PS`, `PD` de `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`. `X64SSEInstructions.def` centralise les largeurs, alignements et règles d’admission. `MaskedSSEArithmeticMatchesIndependentHostExecution` compare les formes registre/RAM à un oracle CPU hôte indépendant : quatre arrondis, FTZ, zéros signés, subnormaux et NaN. `SSEMemoryObserverStopsBeforeResultAndStatusChanges` vérifie l’arrêt avant les effets. Les exceptions non masquées, x87 et AVX restent exclus.

`X64PackedIntegerTests.cpp` utilise les encodages originaux et 180 résultats fixes de `X64PackedIntegerCases.def`, comparés indépendamment aux intrinsics du compilateur x64 natif. Les cas registres et RAM aliasée en fin de page préservent les autres XMM, sentinelles entières, FLAGS, MXCSR et octets sources. Arrêts/erreurs des observateurs et défauts de lecture récupérables préservent l’état ; une réparation permet une reprise. Un mauvais alignement déclenche `#GP(0)` ; MMX, LOCK et MMIO restent rejetés avant les rappels. Les deux privilèges WHP sont obligatoires en CI native.

`X64PackedShiftTests.cpp` et les cas originaux de `X64PackedShiftCases.def` comparent dix décalages à des calculs scalaires indépendants et aux intrinsics SSE2 natifs, avec 16 compteurs immédiats et 21 variables. Ils couvrent les alias compteur/destination, les bits hauts ignorés, l’alignement, les observateurs, les défauts récupérables et le rejet des périphériques. `X64VectorTestSupport.h` partage les assertions de registres et de RAM avec les tests arithmétiques empaquetés. Les deux modes de privilège WHP sont obligatoires en CI native.

`X64VectorMaskTests.cpp` compare des encodages bruts indépendants à une extraction scalaire et aux intrinsics SSE natifs : chaque bit source et les 16 GPR × 16 XMM sont vérifiés avec les deux valeurs de REX.W. Les instantanés de tous les registres publics, la RAM et les observateurs vérifient l’extension par zéro et la conservation d’état. Arrêts, échecs de rappel et formes non prises en charge ne publient aucun effet. La validation native KVM/WHP exige les deux modes de privilège.

`X64ShuffleTests.cpp` utilise les encodages indépendants de `X64ShuffleCases.def` et une sélection scalaire des voies comparée aux intrinsèques natifs. Il couvre les 256 contrôles avec registres, source identique et alias en fin de page, toutes les paires XMM, l’état public complet du CPU et la RAM, les arrêts et exceptions des observateurs, les permissions, les défauts d’alignement et les reprises. MMX, VEX/EVEX, LOCK et les opérandes périphériques doivent être refusés sans effet. La validation native KVM/WHP exige ces cas aux deux niveaux de privilège ; les couples hôte/ISA indisponibles restent explicitement ignorés. Les cas `UNPCKLPS`, `UNPCKHPS`, `UNPCKLPD` et `UNPCKHPD` réutilisent la même matrice d’état et de défauts ainsi que les oracles scalaires et natifs indépendants. Chaque destination XMM est également vérifiée avec une source mémoire.

`X64PartialMoveTests.cpp` et `X64PartialMoveCases.def` comparent des références scalaires/natives indépendantes de chargement et d’écriture, les 16 registres XMM et les bits bruts des NaN/subnormaux. L’état CPU complet et deux pages RAM sont vérifiés pour les accès non alignés, alias, fautes interpages, droits réparés, arrêts/échecs des observateurs et reprises. En fin de page, huit octets suffisent ; écrire ne demande pas de droit de lecture. Alias registre, formes refusées et callbacks de périphérique sont vérifiés séparément ; les cas natifs KVM/WHP sont obligatoires aux deux privilèges.

`X64IntegerFloatTests.cpp` utilise les encodages indépendants de `X64IntegerFloatCases.def`, les attentes `APFloat` et des instructions natives avec sauvegarde/restauration FP. Il vérifie les deux largeurs entières, quatre arrondis, l’état de précision persistant, FTZ, chaque paire GPR/XMM et tout l’état CPU/RAM. Sources non alignées, interpages ou en fin de page, droits réparés, arrêts/échecs et reprises gardent des plages exactes. Les cas natifs KVM/WHP sont requis aux deux privilèges.

`X64FloatIntegerTests.cpp` vérifie arrondi et troncature avec les encodages indépendants de `X64FloatIntegerCases.def`, `APFloat` et des instructions natives sur registre/mémoire. Limites signées, valeurs à mi-chemin, NaN, infinis, subnormaux, tous les arrondis, états persistants et FTZ couvrent les deux largeurs entières. Chaque paire GPR/XMM, tout l’état CPU/RAM, les lectures exactes en fin de page, les fautes interpages récupérables et l’annulation/reprise des observateurs sont vérifiés. Les résultats natifs KVM/WHP aux deux privilèges sont obligatoires.

`X64SSEComparisonTests.cpp` utilise les encodages indépendants de `X64SSEComparisonCases.def`, l’ordre `APFloat` et des instructions natives sauvegardant/restaurant FLAGS et l’état FP de l’hôte. Toutes les paires de 21 entrées brutes couvrent priorité NaN/dénormal, zéros, infinis et valeurs adjacentes. Il vérifie chaque paire XMM et alias, MXCSR persistant, indépendance de l’arrondi, conservation de DF, état CPU/RAM complet, lectures exactes de fin de page/interpages et annulation/reprise. Les résultats natifs KVM/WHP aux deux privilèges sont obligatoires.

`X64SSEPredicateTests.cpp` utilise les prédicats indépendants de `X64SSEPredicateCases.def`, les entrées brutes partagées de `X64SSEComparisonCases.def`, l’ordre `APFloat` et les instructions natives originales. Il couvre toutes les paires, la priorité des exceptions entre voies, les voies scalaires hautes, les alias XMM, l’état CPU/RAM complet, les lectures de fin de page/interpages, la priorité d’alignement et les reprises après observation/faute. Les contrôles Capstone directs couvrent tous les octets de contrôle, deux syntaxes, deux API et les modes 32/64 bits. KVM/WHP natifs sont obligatoires aux deux privilèges ; contrôles réservés, VEX/EVEX et opérandes périphériques restent exclus. Les matrices de valeurs sont réparties par instruction et prédicat, celles des arrondis et contrôles par instruction. `NativeCPUTests.def` exige toutes les combinaisons originales, sans modifier les délais invités ni les 15 secondes de CTest. La compilation vérifie que chaque paire instruction/prédicat apparaît exactement une fois.

`X64SSEPrecisionTests.cpp` associe les encodages indépendants de `X64SSEPrecisionCases.def`, l’arrondi `APFloat` et les instructions natives originales. Les limites utilisent un exposant non borné, y compris débordement dirigé vers une valeur finie et petits résultats arrondis en nombres normaux. Sont couverts les deux signes, charges NaN, arrondis/FTZ/états persistants, agrégation vectorielle, alias XMM, CPU/RAM complets, largeurs exactes, priorité d’alignement, défauts de page et annulation/reprise des observateurs. KVM/WHP sont obligatoires aux deux privilèges.

`X64PackedFloatTests.cpp` utilise les encodages indépendants de `X64PackedFloatCases.def`, les attentes signées `APFloat` et les instructions natives originales. Toutes les paires d’entrées, limites entières, valeurs à mi-chemin, arrondis, états persistants et FTZ sont vérifiés. Alias, paires XMM, CPU/RAM complets, toutes les coupures m64, fins de page, priorité d’alignement et reprises après observation/défaut couvrent les deux privilèges. Tous les cas KVM/WHP sont obligatoires.

`X64PackedFloatIntegerTests.cpp` combine les encodages indépendants de `X64PackedFloatIntegerCases.def`, les entrées partagées de `X64FloatIntegerCases.def`, `APFloat`/`APSInt` et les instructions natives. Toutes les paires de 43 entrées couvrent limites signed32, voisins des points moyens, NaN, infinis et dénormaux. Des matrices distinctes varient indépendamment chaque voie exacte, inexacte, invalide ou dénormale. Tous les arrondis/FTZ/états persistants, alias XMM, CPU/RAM complets, fins de page alignées, priorités et reprises sont vérifiés. KVM/WHP sont obligatoires aux deux privilèges.

`DAZBackends` étend avec DAZ les matrices de comparaisons, de prédicats et de conversions scalaires ou vectorielles entre entiers, flottants et précisions. `X64DAZTestSupport.h` fournit une normalisation indépendante des entrées avec `APFloat` et vérifie le `MXCSR_MASK` de l’hôte avant les instructions natives de référence. Les tests couvrent zéros signés, sous-normaux, NaN, voies mixtes, tous les arrondis, FTZ et états persistants, tout en vérifiant les octets sources, les autres registres, FLAGS et toute la RAM. Les opérandes en registre, par alias et en limite de page conservent leurs observations d’accès. KVM/WHP exigent tous les cas DAZ aux deux niveaux de privilège et les 17 cas de référence sur l’hôte ; les exécutions portables ignorent explicitement les hôtes incompatibles. Les cas sans DAZ et les délais existants sont conservés.

`X64AlignmentTests.cpp` vérifie que les opérandes non alignés des instructions aligned SSE admises signalent un `#GP(0)` récupérable ou terminal avant les observateurs, les permissions ou les callbacks de périphérique. Le contexte public x64 complet, PC et RAM restent intacts. Le rebouclage à la largeur d’adresse précède l’ajout de FS/GS ; réparer l’adresse permet de réessayer l’instruction. Des tests directs KVM/WHP vérifient indépendamment cette frontière matérielle. Windows ring3 distribue les fautes classées `operand_alignment` ; les autres causes de `#GP` restent non prises en charge.

`X64SIMDExceptionTests.cpp` contourne l’admission checked pour vérifier le transport natif de `#XM` sur KVM/WHP aux deux privilèges. Les huit cas originaux de `X64SIMDExceptionCases.def` couvrent six types d’exception, dont les résultats minuscules exacts et les dépassements exacts avec exposant non borné. Les formes registre et RAM préservent tout l’état GPR, XMM, x87, FLAGS, FS/GS et mémoire invitée lors du défaut, hormis le statut MXCSR prescrit. Masquer l’exception permet de réessayer l’instruction ; réparer les opérandes en conservant les états persistants vérifie que les anciens drapeaux ne la redéclenchent pas. Les contrats publics checked et pilote exécutent aussi ces cas originaux de défaut et reprise. `WindowsSIMDExecutionTests.cpp` exécute un défaut natif réel, les instructions VEH/VCH invitées et les continuations par saut, masquage ou réparation des opérandes. Les contrôles actifs et le contexte sauvegardé sont vérifiés séparément. Les tests négatifs de démarrage refusent les défauts absents, vecteurs erronés et destinations modifiées sans publier de capacité.

`check_windows_simd.py` construit un programme Windows x64 original indépendant à partir de `WindowsSIMDCases.def` et des cas scalaires. Ses 6 144 observations couvrent les opérandes registre/RAM, tous les masques, les indicateurs persistants nuls ou tous actifs et trois suites : sauter, masquer puis réessayer, ou réparer les opérandes et réessayer sans changer les masques. Les entrées assembleur enregistrent les états MXCSR et x87 réels de VEH/VCH séparément du `CONTEXT` sauvegardé. Le programme vérifie le PC exact, la préservation de l’état, le contexte réparé et le résultat avant de restaurer l’hôte. La CI conserve les données brutes et les empreintes des sources. `--build-only` prouve seulement la compilation. Ces observations n’activent pas le SIMD non masqué checked et ne prouvent pas une exécution native ARM64.

`WindowsSIMDStatusCases.def` fige les 63 combinaisons non vides d’états actifs observées sous Windows natif. `WindowsSIMDMappingTests.cpp` vérifie les codes et paramètres exacts, rejette les défauts incohérents et contrôles invalides, puis injecte la frontière du défaut pour vérifier les enregistrements, les deux contrôles de CONTEXT et une continuation masquée. L’exécutable original de la CI Windows vérifie indépendamment ces résultats. Ce test injecté ne prouve pas la livraison des exceptions par Unicorn et n’active pas le SIMD non masqué en exécution checked.

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

`DriverThreadPriorityTests.cpp` exécute le pilote original compilé `driver_thread_priority.c` sur Unicorn/KVM/WHP explicites, en contrats driver et checked. Les cas vérifient les changements de priorité en file et en attente, les réveils événement/timer avant la fin du quantum, la rotation à priorité égale, le masquage DISPATCH_LEVEL et les timers malgré la famine des threads inférieurs. Deux boucles de comptage comparées prouvent la conservation exacte du quantum restant. Les tests du modèle couvrent l’ABI signée, l’atomicité des refus, les références terminées, l’identité imbriquée et le réemploi des piles indépendantes. Les cas natifs sont obligatoires dans `NativeDriverTests.def` ; les transports indisponibles restent des skips locaux explicites.

`DriverMutexThreadTests.cpp` exécute quatre modes WDK originaux de `driver_seh_mutex.def` : récursion dans un filtre SEH, propriété acquise par un filtre ou un finally exceptionnel, et reprise d’un filtre bloqué après libération du mutex par un autre thread système. Les contrats driver et checked d’Unicorn/KVM/WHP couvrent les images normales/CFG actif, les adresses préférées/rebasées et les quanta coopératifs/de 1/17 instructions. Les tests du modèle vérifient aussi la désactivation des APC après retrait de la pile imbriquée, le refus d’une libération par un autre thread et le contrôle au retour externe ; les cas KVM/WHP sont obligatoires dans `NativeDriverTests.def`.

`KernelWaitSetTests.cpp` exécute seize cas de modèle sans Unicorn : `WaitAll` partiel, premier `WaitAny` prêt, indices capturés, nettoyage au délai, objets/stockages ultérieurs invalides, limite de 64, IRQL, threads terminés conservés et deux temporisateurs synchrones. `DriverMultipleWaitTests.cpp` exécute sept modes WDK originaux de `driver_wdm_multiple_wait.c` et `DriverMultipleWaitCases.def` sur Unicorn/KVM/WHP, les deux contrats de pilote, images normales/CFG, relocalisation et quanta coopératifs/1/17 instructions. Ces 30 résultats modèle/natifs sont obligatoires dans `NativeDriverTests.def`. Les régressions couvrent aussi la double complétion après succès ou expiration, la modification de l’état capturé et la double complétion d’un délai.

`KernelMultipleWait.DispatcherScalarParametersIgnoreUpperRegisterBits` vérifie les bits hauts contaminés, les limites signées, les dépassements et la préservation de l’état des objets. Les wrappers de saut terminal sans prologue de `DriverMultipleWaitCases.def` exécutent les mêmes appels ABI valides via les véritables imports WDK de `driver_wdm_multiple_wait.c`, avec CFG actif, relocalisation et préemption par instruction.

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
| `unittests/loader` | `NeverDRawISATests` | Fichiers binaires : identification du jeu d'instructions à partir des octets (données, code propre du test, code décalé de deux octets, encodages 32 et 64 bits par famille, fichiers commençant par des zéros) et table de vecteurs Cortex-M. `scripts/validate_isa_model.py --engine build/bin/libneverd.so` vérifie le modèle sur 180 programmes et bibliothèques réels qu'il n'a jamais vus, téléchargés par hachage ; il demande le réseau et ne fait pas partie de CTest |
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

`SymSimplifyFinite.*` couvre les domaines complets à deux valeurs de 8 à 512 bits, le coût des usages partagés, toutes les annotations prises en charge pouvant produire poison, les lectures volatile et freeze indépendants, undef/poison explicites, les parcours itératifs profonds, les budgets et la marque d’obfuscation. Les IR original et simplifié sont exécutés à O0/O2 face à un oracle indépendant pour tous les octets et des entrées aléatoires de pleine largeur. Les tests d’objets traduits exigent des identités de cache distinctes pour des budgets de valeurs finies distincts.

Les tests de domaines fusionnés couvrent selects imbriqués, PHI en losange et cycles de copie sur 8–512 bits ; retours conflictuels, conditions indéfinies, composantes sans origine, observations PHI/freeze indépendantes et producteurs annotés conservés ; ainsi que les limites exactes de nœuds, arêtes et travail. Les oracles O0/O2 parcourent toutes les paires d’octets et varient les opérandes pleine largeur pour les sélections, jonctions et boucles d’état bornées.

Les tests à deux valeurs couvrent aussi les masques de conjonctions imbriquées sur 8–512 bits, les opérandes permutés, le refus OR/undef, la profondeur bornée, le comptage autonome, le marquage d’obfuscation et le budget exact de la première réécriture. Les oracles exécutent toutes les paires d’octets et font varier les autres données sur 64 bits, en comparant IR original et simplifié à O0/O2.

`SymSimplifyPredicates.*` énumère exhaustivement décalages, signes et entrées sur quatre bits, vérifie les compositions booléennes et ensembles disjoints, puis exécute des oracles indépendants sur un octet et pleine largeur à O0/O2. Il couvre annotations poison, entrées indéfinies cachées aux jointures, lectures/freezes indépendants, PHI de boucle conservés, rentabilité des usages partagés, travail cumulé, nombreux usages, limites de récursion et marque d’obfuscation. Les deux clés de cache distinguent les budgets d’analyse des prédicats.

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

`HighIntegerSignedness.*` dans `NeverDHighControlFlowTests` vérifie la passe tardive qui déclare chaque variable locale de registre ou temporaire signée ou non signée selon ce que lisent la plupart de ses usages. L'arithmétique modulaire, les décalages logiques et les comparaisons non signées favorisent le non signé ; les comparaisons, divisions et décalages arithmétiques signés et l'extension de signe favorisent le signé ; une locale ayant un usage non entier garde ses types. Le C émis s'exécute en `-O0` et `-O2` avec des pièges de comportement indéfini face à une arithmétique de référence indépendante, y compris une comparaison signée d'une locale devenue non signée.

`HighValueForward.*` dans `NeverDHighControlFlowTests` vérifie quand le rédacteur HighC peut replier une valeur à usage unique dans son usage. Une condition de boucle garde une valeur dont la boucle affecte les variables, car un même nom peut désigner plusieurs valeurs SSA ; un emplacement de pile relu garde sa valeur à travers une écriture dans cet emplacement et se replie au-delà d'une écriture dans un autre. Une copie garde sa valeur quand sa source est réaffectée avant l'usage. Chaque cas s'exécute en `-O0` et `-O2` avec des pièges de comportement indéfini.

`HighCIntegerConversion.*` dans `NeverDHighControlFlowTests` vérifie les conversions entières que le rédacteur HighC laisse au C. Une conversion à l'intérieur d'un opérande qui garde les octets qu'une conversion extérieure garde n'imprime pas de cast propre ; une affectation à une variable locale entière déclarée et un return convertissent implicitement, et un littéral s'écrit comme la valeur vers laquelle il se convertit, tandis qu'un pointeur garde sa conversion explicite. Les écritures en mémoire convertissent comme les affectations, et un argument étendu par des zéros pour un paramètre typé plus large garde son extension. Chaque cas s'exécute en `-O0` et `-O2` avec des pièges de comportement indéfini face à une arithmétique de référence.

La projection source revalide aussi les listes d’objets variadiques après ce nettoyage : les ancres d’instruction vides sont admises, les effets cachés et transferts de contrôle sont refusés. Le nettoyage synchronisé admet une seule vue `int64_t` ou `uint64_t` du même récepteur sauvegardé ; les réductions de largeur, conversions flottantes, calculs d’adresse et réaffectations restent refusés. Les ensembles Foundation et les traces de déverrouillage normal ou exceptionnel sont exécutés à `-O0` et `-O2`.

## Exceptions synchrones natives x64

Les `DIV`/`IDIV` checked x64 utilisent le résultat du processeur et `#DE`. KVM emploie une IDT/IST supervisor privée, WHP un bitmap explicite ; le contexte original et les codes disponibles restent distincts des erreurs de transport. L’OS consomme l’événement récupérable avant d’installer une continuation. Le modèle de pilote Windows traduit la division par zéro et le débordement du quotient en `STATUS_INTEGER_DIVIDE_BY_ZERO`, avec de vrais filtres SEH, `__finally` et reprises. `NeverDX64ExceptionTests` se construit sans Unicorn ; `DriverWDMCPUException` valide les cas WDK originaux. Les hôtes ARM64 indisponibles sont explicitement ignorés.

## Effets RAM préparés

`RAMTransaction` conserve uniquement l’union physique des écritures déclarées d’une instruction, sous le verrou d’exécution. La RAM initiale est restaurée avant les observateurs de résultats ; annulation, erreur de transport ou exception de l’observateur ne publient aucun état partiel de RAM ou de registres. Après restauration de la RAM, les fautes CPU conservent leur état architectural d’exception. Les écritures simples et doubles ARM64 utilisent la même autorité. x64 exécute `XCHG`, `XADD` et `CMPXCHG` sur 8/16/32/64 bits, avec alignement naturel pour les formes verrouillées ou implicitement verrouillées. `NeverDRAMTransactionTests` compare les résultats à la CPU hôte et vérifie restauration, alias et permissions ; les plateformes indisponibles sont explicitement ignorées. Périphériques et SMP parallèle restent exclus ; les instantanés CPU ne restaurent pas la RAM déjà validée.

`CMPXCHG8B` et `CMPXCHG16B` exécutent leurs instructions originales avec KVM, WHP et checked Unicorn dans les profils pilote et utilisateur. Une comparaison réussie ou échouée exige les droits de lecture et d’écriture ; les défauts sont classés comme écritures. `CMPXCHG16B` vérifie l’alignement sur 16 octets avant tout accès mémoire et signale `#GP(0)` en cas de violation. Ses deux observations partagent une transaction RAM : un arrêt ou une exception dans l’une annule toute publication des registres et de la RAM. `CMPXCHG8B` sans verrou peut traverser une page ; les opérations verrouillées exigent toujours l’alignement naturel. `X64WideAtomicTests.cpp` compare les résultats originaux de l’hôte et les défauts natifs directs, puis vérifie alias, préfixes, adressage, réparation et annulation. Les fixtures originales de pilote Windows et de PE ring3 couvrent les deux largeurs ; la fixture WDK exécute aussi `_InterlockedCompareExchange128`. Le modèle CPU doit prendre en charge `CMPXCHG16B`.

## État x87 complet

`NeverDEmulationArch` possède les contrats ISA, les tables de pages et le format FP partagé par les transports natifs et Unicorn. Les contextes x64 conservent contrôle, état, TOP, tags physiques, opcode, pointeurs instruction/données et huit registres de 80 bits. `FP0`–`FP7` utilisent `RegisterValue` ; les accès scalaires refusent la troncature. `FPTag` est le masque physique des registres non vides. `NeverDX64FPTests` vérifie tous les TOP, les opérations exactes contre FXSAVE/FXRSTOR du processeur hôte et la restauration. Cela ne rend pas les instructions x87 admissibles en mode checked et ne prouve pas tous les arrondis. Les hôtes natifs indisponibles sont explicitement ignorés.

`driver-strict` accepte KVM sur un hôte Linux x64 compatible et WHP sur un hôte Windows x64 compatible ; `auto` sélectionne ce transport natif, et les ISA différentes utilisent Unicorn. Unicorn explicite et l’API V1 conservent le profil logiciel portable. L’exécution native vérifie les adresses canoniques et les effets avant l’entrée ; le matériel indisponible provoque un échec sans repli. Instructions et comportements OS non pris en charge échouent explicitement. La CI native Windows x64 avec Unicorn désactivé réussit les 359 contrôles obligatoires : 131 contrôles CPU, 224 résultats de pilotes issus de 26 images intégrées, 46 images WDK et 40 cas de scénarios aux adresses préférées et relocalisées, ainsi que quatre contrôles de limites SEH ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Les preuves natives ARM64 restent manquantes ; aucune compatibilité universelle des pilotes ou Android/Darwin n’est établie.

La validation native ci-dessus couvre les points d’entrée déclarés des pilotes et les scénarios publiés. Les régressions détaillées par fonctionnalité et les contrôles C API/CLI/Python décrits ci-dessous conservent des preuves limitées à Linux, sauf mention explicite d’une exécution Windows ; la réussite du corpus natif ne valide pas chaque variante de test sous Windows.

Interrogez le profil sélectionné avec `executionCapabilities(Contract, ISA, Backend)`. `NativeLegacyX64` décrit l’exécution native des pilotes x64. `NeverDNativeDriverTests` valide le corpus existant et peut fonctionner dans une compilation sans Unicorn.

Le workflow CI existant exécute tous les tests d’émulation avant les profils généraux et conserve l’inventaire, les résultats JUnit et le journal CTest dans `emulation-focused`. Une défaillance ailleurs ne bloque pas cette exécution. Le matériel indisponible et les pilotes optionnels absents restent des tests explicitement ignorés ; une réussite logicielle ou de compilation ne prouve pas l’exécution native.

Sur Linux, `NeverDUnicornDeadlineTests` fait terminer le véritable thread du minuteur avant l’entrée dans l’invité en contrôlant l’ordonnancement pthread. Il couvre x64, ARM32 et ARM64, exige l’absence d’effets après une annulation préalable et vérifie le budget indépendant de l’exécution suivante. Il utilise les API publiques sans modifier l’état privé du moteur.

`X64StateTransition` dans `NeverDX64ExceptionTests` effectue des lectures RAM indépendantes et des lectures CR8 sur le CPU natif. Il alterne les bases TLS et le privilège, reprend après des fautes de division répétées et change TLS après une entrée annulée. Après une modification du transfert d’état natif, exécutez ce label CTest ainsi que la couverture des alias remappés, contextes CPU, états FP et résultats des pilotes originaux. Un transport KVM/WHP indisponible reste un saut explicite.


`NeverDKvmRunTests` vérifie les transferts empruntés de `KvmRunControl` sans nécessiter `/dev/kvm`. `StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` vérifie que préparation, collecte et entrée hôte interceptée utilisent le même thread, avec une seule préparation malgré les reprises après interruption. Les autres cas couvrent l’échec de préparation sans entrée, l’échec de collecte, l’arrêt pendant la préparation et l’annulation d’une entrée active, puis une nouvelle exécution sans réutilisation des anciens callbacks. Conservez les suites de véritable annulation, de restauration RAM, d’exceptions et de pilotes originaux dans la validation. `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` vérifie que plusieurs entrées sous une même échéance réutilisent le thread, exécutent chaque transfert une seule fois et ne modifient pas les paquets précédents.

`KvmHandoffPolicy` limite chaque attente active à 8 μs, passe à une attente bloquante après deux échecs consécutifs et réessaie après 256 échanges. L’appelant et le worker s’adaptent indépendamment ; l’appelant respecte aussi l’échéance et le jeton d’arrêt initiaux. Les indicateurs atomiques ne sont que des indices d’ordonnancement : le mutex protège toujours les paquets, la durée de vie des callbacks et l’acquittement de l’annulation. `NeverDKvmRunTests` vérifie la limitation des attentes improductives, la reprise, les variations de latence du pair et l’annulation avant réutilisation des paquets.

KVM x64/ARM64 utilise `KvmRunControl` pour préparer, entrer dans `KVM_RUN` et capturer l’état sur un même worker vCPU privé. La préparation n’a lieu qu’une fois malgré `EINTR` ; annulation et capture échouée interdisent la publication. `KvmAArch64Machine.cpp` exécute aussi la maintenance des traductions et les transferts scalaires/vectoriels complets sous une échéance commune. L’appelant publie après confirmation et conserve décodage ISA, transactions RAM, politique OS et observateurs. Les preuves natives ARM64 restent manquantes.

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` vérifie la continuation et les écritures du CPU après modification hôte des registres généraux, des XMM aux deux extrémités, de MXCSR et du contrôle x87. Les octets réels de `FXSAVE64` vérifient tous les registres physiques de 80 bits, TOP, tags, opcode et pointeurs après une entrée arrêtée ; les fautes de division répétées invalident aussi la réutilisation. Ces tests machine n’admettent aucune instruction x87 supplémentaire dans les profils checked.

`NeverDKvmStateTransferTests` injecte un échec de lecture des registres ou de XSAVE après une véritable exécution KVM, puis reprend avec l’entrée inchangée. Les résultats indépendants entiers et d’octets empaquetés prouvent qu’une collecte échouée ne réutilise pas l’état natif déjà avancé. Seul cet exécutable enveloppe `ioctl` ; les hôtes natifs indisponibles sont explicitement ignorés.

`NeverDKvmStateTransferTests` couvre sur KVM réel les ensembles `KVM_CAP_SYNC_REGS` absents, individuels ou combinés et les échecs de requête. `SynchronizedCapturesRemoveOnlySupportedReadIoctls` compte les lectures réelles et vérifie tout l’état CPU après des pas consécutifs. `CancelledWarmEntryRequiresFreshSpecialStateOnRetry` exige une nouvelle lecture des registres spéciaux après annulation. Échecs de capture, reprises entier/SIMD, restauration de RAM spéculative et priorité des exceptions utilisent la même matrice ; la couverture native indisponible est explicitement ignorée.

ARM64 vérifié possède une frontière commune pour l’état complet. `Registers.def` définit 39 champs scalaires et 32 vecteurs de 128 bits ; `captureAArch64State` prépare toutes les lectures, applique les largeurs et normalise NZCV avant une publication unique. Unicorn, KVM, WHP et HVF transfèrent le même inventaire, dont TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR et FPSR. Les adaptateurs natifs activent FP/SIMD via CPACR_EL1. Toute lecture échouée ou entrée annulée préserve l’état complet de l’appelant.

Le démarrage ARM64 KVM/WHP/HVF exécute le programme privé `AArch64MachineProbe.def` : NOP, addition FP32 arrondie vers l’infini positif et addition SIMD à deux voies. Chaque étape compare les 39 champs scalaires et 32 vecteurs, dont TLS, NZCV, l’effacement des bits supérieurs du résultat et la conservation/accumulation de FPCR/FPSR. La sonde utilise uniquement la mémoire de supervision et une échéance globale. Les sondes attestent uniquement cette initialisation bornée. La validation des charges Linux ARM64 KVM et Windows ARM64 WHP reste à établir ; les résultats natifs macOS figurent dans le [guide HVF](macos-hvf.md). Le programme comprend également la signature et l’authentification des adresses de retour A/B avec les clés désactivées, ainsi que les quatre formes BTI sur des pages non protégées.

La sonde exécute aussi deux fois `MRS CTR_EL0`, puis `DC CVAU`, `DSB ISH`, `IC IVAU` et `ISB`, en vérifiant la stabilité de la géométrie du cache et tout l’état. Checked EL0/EL1 admet les instructions originales, les options DSB de base nommées et seulement ISB SY. CTR provient du CPU virtuel choisi et peut varier selon le transport. Les cibles doivent désigner de la RAM ordinaire lisible avec les droits courants, y compris les adresses non alignées et les alias ; les autres sont refusées comme non prises en charge. La maintenance ne produit aucun événement de lecture/écriture de données. La projection assure la cohérence de l’exécution, sans modéliser les caches privés ni le SMP matériel parallèle. `NeverDAArch64CacheTests` vérifie l’état, les fins de pages en lecture seule, les refus, les arrêts, les contextes, les budgets et les mises à jour de code invité via des alias RW/RX traversant deux pages. Les hôtes KVM/WHP indisponibles sont explicitement ignorés.

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

`X64StringInstructions.def` définit aussi `CMPS/SCAS` sur RAM ordinaire en 8/16/32/64 bits avec `REPE/REPNE`. Chaque élément valide toutes les lectures avant observation, actualise les six indicateurs arithmétiques et s’arrête à la première condition de fin. Un défaut de données restaure les indicateurs à l’entrée de ce REP ininterrompu, tout en conservant les pointeurs et le compteur des éléments terminés ; une reprise publique repart de l’état CPU publié. Arrêts et exceptions des observateurs ne modifient pas l’élément courant. Une fin anticipée ne lit jamais l’élément suivant. FS/GS ne concerne que la source CMPS ; SCAS conserve l’accumulateur et le registre source inutilisé. Périphériques et bits hauts ambigus à compteur nul en 32 bits restent exclus. `X64StringComparisonTests.cpp` compare instructions hôtes indépendantes, indicateurs, direction, alias, bouclage, permissions et reprise ; son oracle Linux x64 capture les registres au défaut réel. Le pilote WDK original exécute les deux répétitions conditionnelles aux quatre largeurs via `driver_resource_strings.def`. Voir la [référence Intel](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html). L’oracle natif Linux vérifie les fautes avant et après le premier élément. Il distingue la restauration des flags initiaux sur Intel des flags de la dernière comparaison observés sur AMD EPYC 7763 sous Hyper-V ([observations natives](https://github.com/NeverSight/NeverD/actions/runs/37202522130)) ; un fournisseur CPU inconnu provoque un échec explicite. Les invités checked restaurent les flags initiaux sur tous les backends.

Les CPU WHP vivants partagent une partition native ; sa fermeture finale et sa recréation utilisent le même verrou de registre. `WhpResourceCache.h` réutilise le VP 0 coopératif ; changer de CPU logique détruit d’abord ce VP et ses mappings. Les CPU parallèles conservent des VP et des plages GPA distincts. Chaque transfert de registres, opération XSAVE et annulation vise son propre VP. x64 conserve les fonctions XSAVE par défaut de l’hôte et vérifie la configuration effective avec `WHvGetPartitionProperty`. L’ordonnancement reste coopératif par défaut.

`NeverDX64FPTests` vérifie les 79 positions de corruption et exécute les instructions indépendamment assemblées de `X64ProbeCases.def` sur les transports natifs, avec une échéance unique et RAM invitée inchangée. `NeverDProjectionCacheTests` couvre appelants, ordre ISA, historique des racines, variantes privilège/moniteur, générations, identité des espaces et remplacement échoué. `NeverDRunControlTests` inclut `WhpXsaveTests.cpp` pour les paquets des API anciennes et modernes, chaque TOP, les tailles et l’état inchangé après erreur ; ces tests de protocole en mémoire ne constituent pas une preuve WHP native. Les transports natifs indisponibles sont explicitement ignorés.

Les diagnostics XSAVE distinguent requête de taille, préparation locale et décodage du paquet capturé. Ils conservent le nom API, les tailles retournée et allouée ainsi que des métadonnées limitées d’en-tête et de contrôle, avec des attentes indépendantes dans `WhpHostFailureCases.def`, sans afficher les données des registres invités. `InvalidInputReportsPreparationWithoutHostMutation` vérifie aussi qu’une entrée refusée ne provoque aucun appel hôte ni modification de son paquet. Le codec ISA commun reste l’unique autorité de validation.

Les erreurs hôte WHP lors des requêtes de capacités, de la configuration des partitions/CPU virtuels, du transfert des registres/XSAVE et de l’exécution conservent le HRESULT et le nom d’API déclaré dans `WhpProtocol.def` ; les échecs de requête de capacités conservent le résultat typé d’indisponibilité. `WhpHostFailureCases.def` définit des attentes indépendantes pour les erreurs hôte simultanées à une annulation et les échecs de requête, d’installation et de capture XSAVE modernes ou anciens. Le CI Windows ciblé exige 210 succès natifs : 16 cas de mappage, deux de démarrage, dix FP/contexte, sept CPU partagés, huit entiers et les deux variantes API de `NativeInstallRetainsFPStateBeforeAnyGuestExecution`. Ces dernières comparent FP/SSE complet et métadonnées lues séparément avant toute exécution invitée. Toute inscription manquante, tout test ignoré, désactivé ou non exécuté fait échouer l’audit des preuves natives. Les 26 contrôles supplémentaires couvrent tous les cas de `X64BitStringTests.cpp` aux deux niveaux de privilège. Windows PE64 exige 67 cas de processus WHP et onze cas indépendants avec Windows natif.

`NeverDMemoryLifecycleTests` se construit indépendamment d’Unicorn, y compris dans les configurations natives seules. Les cas de projection et de périphériques propres au logiciel sont explicitement ignorés si Unicorn est désactivé ; les cas de CPU partageant la RAM sur l’hôte correspondant restent enregistrés. `WhpMemoryTests.cpp` isole l’API mémoire native avec 16 cas de `WhpMemoryCases.def` : taille d’une page ou de la projection, allocations partagées ou indépendantes, octets non touchés ou résidents, présence ou absence du premier processeur virtuel. Chaque cas conserve deux propriétaires logiques, alterne plusieurs fois leur partition mappée, détruit le propriétaire inactif et vérifie que le mapping restant fonctionne sans recréation. Une erreur réelle conserve son HRESULT et fait échouer le test ; cette preuve de l’API mémoire ne prouve pas l’exécution d’instructions.

`X64MachineProbe.def` identifie l’instruction de démarrage en échec et toutes les différences de scalaires, TLS, privilège, contrôles x87, voies FP physiques et mots XMM, avec valeurs attendues et observées. `DiagnosticIdentifiesStepFieldAndBothValues` vérifie des messages attendus indépendants. La comparaison reste exacte : le diagnostic distingue une perte de transfert d’un problème d’exécution sans valider une sonde native ayant échoué.

`WhpResourceTests.cpp` couvre la réutilisation, la destruction avant remplacement, la reprise après échec et les courses entre échéance et arrêt. `LogicalCPUSwitchingRestoresPhysicalFPAndTLS` alterne deux machines dans les deux modes de privilège, vérifie leurs états physiques x87/XMM et FS/GS indépendants, puis reprend la survivante après destruction de sa paire. La CI Windows exige les deux cas WHP.

`NEVERD_ENABLE_SEMANTIC_TESTS`, activé par défaut (`ON`), contrôle le groupe de tests de `unittests/semantic` et ses cibles agrégées. Pour construire les tests CPU natifs sans Unicorn, conserver `BUILD_TESTING=ON` et définir `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` ainsi que `NEVERD_EMULATION_BACKEND_UNICORN=OFF`. Les tests natifs KVM/WHP restent disponibles, y compris avec Windows ARM64/MSVC et les en-têtes SDK appropriés. Activer Unicorn sous Windows ARM64 exige toujours une chaîne ARM64 LLVM-MinGW. Cette séparation de construction ne constitue pas une validation native ARM64.

Le CI CPU natif initialise les sources Capstone à la révision fixée et utilise le paquet LLVM précompilé vérifié. Avec `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` et l’adaptateur Unicorn désactivé, la configuration, la compilation et l’édition de liens des tests CPU ne nécessitent aucune source Unicorn. Les signatures et le corpus externe ne sont pas requis. Le CI par défaut conserve le groupe complet de tests sémantiques.

Le profil manuel `native_cpu_only` de `ci.yml` sélectionne Windows x64 avec `native_cpu_backend=whp` (par défaut), ou Ubuntu x64 avec `native_cpu_backend=kvm`. `NativeCPUTests.def` partage les exigences CPU/processus et déclare séparément les cibles et cas propres aux transports. `run_native_cpu_ci.py --require-whp` ou `--require-kvm` vérifie l’hôte, construit toutes les cibles avant CTest et conserve inventaire, JUnit, journaux et décompte des résultats. Tout cas obligatoire absent ou ignoré échoue même si CTest réussit. La CI désactive Unicorn ; `--with-drivers` exige le même corpus aux adresses originales et relocalisées sur le transport choisi. Compilation et sondes d’initialisation ne prouvent ni l’exécution invitée ni la validation ARM64. Le profil Ubuntu utilise les paquets amont signés Clang/LLD 21 ; les déclarations CR8 de Clang 18/19 sont incompatibles avec les en-têtes WDK épinglés. La validation native Linux utilise CMake 4.2.3. `NeverDNativeDriverTests` sélectionne `NO_PRETTY_VALUES` pour conserver dans CTest les noms de cas déclarés, indépendamment du rendu diagnostique des paramètres.

Avant la construction, la CI native exécute `sccache --zero-stats`. Si la sonde échoue, les lanceurs des compilateurs C et C++ sont désactivés, en conservant la configuration des compilateurs et tous les tests obligatoires. Les échecs de configuration ou de compilation restent fatals.

La validation KVM exige l’annulation d’un véritable vCPU sans sortie spontanée et 48 résultats de transfert d’état issus de `KvmStateTransferCases.def`, dont la capture par ioctl et l’échec des requêtes de capacités facultatives. Les autres modes de registres synchronisés s’exécutent si l’hôte les prend en charge, sinon leur omission est explicite. Les noms stables des paramètres ne dépendent ni des numéros ioctl ni du format des tuples. Les tests de protocole complètent l’exécution native sans la remplacer.

`native-host-probe.yml` exécute le programme autonome `probe_native_host.py` sur les runners hébergés Linux et Windows x64/ARM64. `NativeHostProbe.def` déclare l’ordre des preuves de capacités, de création VM/vCPU et de libération. Les rapports conservent les empreintes des sources/binaires, l’ISA native et chaque code d’état. `setup_ready` atteste uniquement la préparation ; aucune instruction invitée ne s’exécute. Les capacités API/périphérique absentes donnent `unavailable` ; les erreurs de compilation, préparation, libération, délai ou format des preuves font échouer le job. La disponibilité ARM64 doit être observée à chaque exécution ; ce test ne valide pas les charges ARM64. Les deux workflows Linux utilisent `prepare_kvm_ci.py` pour donner au seul compte du runner hébergé l’accès au périphérique caractère KVM existant, en conservant son identité et ses permissions. Les machines locales ou autohébergées sont refusées et aucun périphérique absent n’est créé.

`windows-alignment-oracle.yml` utilise `check_windows_alignment.py` et `WindowsAlignmentCases.def` pour recueillir 72 observations originales d’exceptions Windows x64 : neuf formes SSE alignées, chacune dans sept cas d’adresse/droits non alignés et un cas témoin aligné sur une page inaccessible. Les codes, paramètres, PC fautifs, contextes sauvegardés, sorties brutes et empreintes des sources/binaires sont conservés ; les entrées et la RAM doivent rester inchangées. Ces observations établissent uniquement le comportement du système ; elles ne valident pas l’exécution KVM/WHP et n’ajoutent pas de prise en charge SEH.

Avec `native_cpu_only=true`, `native_driver_tests=true` active `NeverDNativeDriverTests` sans Unicorn. Avant la configuration, `build_wdk_driver_fixtures.py` vérifie le SHA-256 intégral des paquets Microsoft officiels WDK/SDK 10.0.26100.6584 et reconstruit 48 images de pilotes normales/CFG/DBG depuis les sources originales. `WDKDriverFixtures.def` déclare les paquets, les arguments de compilation et d’édition de liens et les associations des fixtures. Les fichiers Microsoft non modifiés et leurs licences restent dans les répertoires locaux de compilation/cache ; la CI ne publie que les métadonnées et journaux de compilation. Le manifeste conserve versions des outils, commandes, empreintes des sources/en-têtes et empreintes des images produites.

`NativeDriverTests.def` exige 230 résultats WHP pour les 115 charges de `DriverBuiltinImages.def` et `DriverBackendParityCases.def` : 27 images intégrées, 48 images WDK et 40 scénarios de requêtes, aux adresses initiales et relocalisées. L’inventaire obligatoire complet est `5068 CPU + 230 WHP + 25 SEH + 77 scheduling + 30 wait sets + 11 driver UNPACK + 6 clock reads = 5447`. Les 30 contrôles d’ensembles d’attente comprennent seize cas de modèle portables et quatorze cas de pilotes natifs originaux. `run_native_cpu_ci.py --with-drivers` conserve les identités exactes et preuves JUnit avec Unicorn désactivé. Tout fixture requis absent ou ignoré fait échouer cette validation facultative ; les builds ordinaires gardent les fixtures externes facultatifs. Les images fixes conservent leur rejet de relocalisation attendu. L’exécution native de guests ARM64 reste non vérifiée.

`InterruptionRetainsPhaseCauseDeadlineAndLease` injecte expiration, arrêt et combinaison des deux avant deux instructions initiales distinctes. Le test vérifie la phase exacte, la durée de vie du message possédé, le type et les causes de l’erreur, une échéance inchangée et la libération de la mémoire. Les vrais échecs de transport et les divergences d’état restent distincts. Le budget de validation initiale x64 native est `5 s` ; les échéances invitées et marges de pas unique restent inchangées.

`WhpResourcePolicy.def` accorde à la création des ressources WHP x64 et ARM64 un délai distinct de `30 s` avant la validation ISA. La configuration synchrone de l’hôte est contrôlée par rapport à cette échéance avant publication de la ressource. Les sondes d’instructions et les délais invités ordinaires gardent leurs limites. `WhpResourceTests.cpp` vérifie le type et les causes des interruptions d’initialisation, la destruction après annulation, la priorité des erreurs hôte et les délais d’exécution ordinaires inchangés.

`X64PopFlagsTests.cpp` vérifie les deux privilèges et `driver-strict` : 256 images autorisées et deux états initiaux, neuf encodages, 64 bits d’entrée, alias en lecture seule ou exécutables, fautes entre pages et réparation, arrêt/échec des observateurs, rejet des piles de périphériques et limites des instructions natives suivantes. `X64PopFlagsOracle` exécute indépendamment les instructions originales sur x64 et vérifie CPL3/IOPL0 et la consommation exacte de pile. `driver_resource_flags.def` fait définir, effacer puis restaurer les indicateurs au pilote WDK original avec les deux largeurs. Les états entier/contrôle/x87/SSE restent complets ; ces tests ne prennent pas en charge TF/NT/AC/ID invités et ne prouvent pas l’exécution ARM64 native.

`X64StatusFlagsTests.cpp` couvre `CLC/STC/CMC`, `LAHF/SAHF`, les 256 entrées AH et combinaisons de flags admises, tous les REX, l’état CPU complet, la mémoire inchangée, les arrêts/erreurs d’observation, les contextes et la reprise ADC/stockage native. LOCK invalide est refusé sans effet. Un oracle natif indépendant vérifie 24 préfixes après contrôle CPUID. Les pilotes WDK de ressources exécutent les cinq instructions, ajoutant 22 résultats natifs obligatoires. Les hôtes/ISA indisponibles restent explicitement ignorés. Le profil Unicorn portable exécute les sept mêmes cas ; les tests directs de la dépendance couvrent AH et LOCK en 16/32/64 bits, les registres REX explicites et le refus en mode long sans la fonctionnalité requise.

`X64DoubleShiftTests.cpp` couvre tous les comptes imm8/CL admis, les registres partagés/étendus, les flags définis, les observations RAM entre pages, l’annulation, les défauts de permission/mappage/périphérique, le refus de LOCK/comptes indéfinis, les contextes et la reprise ADC native. Un oracle indépendant vérifie 5 184 exécutions originales ; douze sondes WDK couvrent les registres et RAM. Le contrôle natif ajoute 145 résultats obligatoires.

`X64ScalarShiftTests.cpp` vérifie tous les comptes sur un octet, les deux retenues entrantes, les opérandes nuls, entièrement à un et signés, les formes implicites à un, les alias AH/SPL et du registre compteur, l’état CPU complet, les plages RAM exactes, l’annulation des observateurs, les fautes et la reprise de contexte. Un oracle natif indépendant vérifie 65,536 exécutions. Le pilote de ressources WDK ajoute 72 sondes originales. Le contrôle KVM/WHP exige 769 résultats pour cette famille. Seul un compte masqué nul garantit la préservation de tous les drapeaux. Une rotation complète non nulle de l’anneau de retenue `RCL/RCR` préserve l’opérande et CF mais laisse OF indéfini ; l’oracle exclut uniquement ce bit indéfini.

`X64LoopTests.cpp` couvre 21 encodages originaux, le débordement circulaire des compteurs, les cibles relatives signées au-delà de 4 GiB, la préservation de l’état CPU/RAM complet, l’annulation des observateurs, les instructions réparties entre pages indépendantes, les fautes de lecture de cible et la restauration du contexte. Une instruction incomplète est rejetée avant exécution ; une faute sur sa cible conserve le compteur et le PC de la branche exécutée. Le pilote WDK ajoute 12 sondes originales. KVM/WHP exigent 379 résultats pour cette famille dans les contrats superviseur, utilisateur et pilote. L’oracle des instructions hôtes originales exécute jusqu’à 1,008 cas et en rapporte le nombre. Les branches AMD prises avec `66H` visent la mémoire basse réservée par le système hôte ; ces formes sont donc exécutées dans la matrice invitée avec des cibles basses explicitement mappées. Les largeurs Intel/AMD et la priorité de REX.W sont vérifiées séparément.

`X64BranchTests.cpp` vérifie les 16 conditions Jcc et JMP relatif, neuf séquences de préfixes, les formes courtes/proches, les cibles relatives signées au-delà de 4 GiB, l’état CPU/RAM complet, les arrêts/erreurs des observateurs, le décodage entre pages, les fautes de lecture à la cible et la restauration du contexte. La référence indépendante Intel exécute 9,792 instructions originales ; les formes AMD à cible basse restent dans les tests invités. Les deux modèles testent les octets complets/tronqués et les sondes natives vérifient l’échec de publication. Quatre sondes WDK originales couvrent la politique des pilotes. Chaque porte KVM/WHP ajoute 274 résultats obligatoires. Le modèle logiciel AMD ne prouve pas l’exécution native AMD ou ARM64.

`X64StackTests.cpp` couvre 42 encodages dans neuf familles : largeurs, adresses, état complet, ordre des observateurs, annulation, permissions, frontières de pages, alias physiques, réparation des fautes, rejet des périphériques et restauration du contexte. Un oracle hôte indépendant exécute les instructions originales ; six sondes WDK vérifient le chemin pilote. Chaque porte native KVM/WHP ajoute 1135 résultats obligatoires. Les transports indisponibles restent explicitement ignorés hors de leur porte native obligatoire.

`X64FrameExitTests.cpp` couvre 14 encodages de `LEAVE`, l’ordre effectif des préfixes, l’adressage RBP complet, tous les registres, les alias en lecture seule, les fautes et réparations de cadres traversant une page, les privilèges, les observateurs, les adresses invalides, le refus des périphériques et la reprise du contexte. Un oracle hôte indépendant exécute 42 instructions originales ; cinq sondes WDK vérifient l’exécution des pilotes. Chaque validation native ajoute 379 résultats obligatoires.

`X64FrameEntryTests.cpp` couvre 14 encodages, imbrication, recouvrement, alias physiques, vérifications en écriture seule, fautes interpages et réparation, annulation, privilèges, préfixes refusés et reprise de contexte. Des oracles hôtes indépendants comparent les octets de pile et les registres sur 882 exécutions réussies et 84 fautes sous Linux x64. Deux tests de transport injecté distinguent abandon sur annulation/erreur et publication sur faute architecturale. Six sondes WDK exécutent les instructions originales dans le pilote. Les contrôles natifs ajoutent 508 résultats obligatoires KVM et 507 WHP.

`DriverSIMDSEHTests.cpp` exécute huit défauts SSE originaux avec quatre dispositions prises en charge et un rejet de modification x87, deux contrats natifs, les images WDK normales/CFG et deux adresses. Dix résultats par backend et trois contrôles purs des enregistrements SSE noyau sont obligatoires. `driver_seh_simd.def` définit cas et modes ; les tables de déroulement asynchrone couvrent la fonction fautive. Le noyau Microsoft 10.0.26100.9549 fournit une référence indépendante : 107,744 classifications et 8,192 restaurations par exécution isolée des chemins d’instructions. Cela ne constitue pas une exécution de pilote dans un noyau Windows complet ; KVM/WHP ARM64 natifs restent non vérifiés.

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

`NeverDInstructionFetchTests` exécute des programmes checked x64/ARM64 via Unicorn, KVM et WHP en modes superviseur et utilisateur. `InstructionFetchCases.def` couvre les formes d’opérandes, les branches relatives, les écritures invité/hôte par alias de code, la restauration du contexte, le retrait des permissions, les pages à stockage distinct, la lecture anticipée en fin de page, les encodages invalides ou tronqués et le rejet des appels récursifs. La CI Windows native exige la réussite de tous les cas WHP x64. Les couples hôte/ISA indisponibles sont explicitement ignorés ; l’exécution ARM64 portable ne prouve pas le support ARM64 natif.

`WhpStateTransferTests.cpp` injecte des transferts pour les deux générations XSAVE : groupes modifiés, capture complète, remplissage ignoré, échecs partiels, annulation, priorité des exceptions et remplacement de partition. `ContinuedStepsReuseCapturedRegistersAndFP` compte les installations évitées ; `PartialTransferFailuresPreserveStateAndForceFullRetry` exige une restauration complète. Ce sont des contrôles de protocole ; les suites natives FP, transitions, pilotes et ring3 restent nécessaires.

`CancelledDirectRunPublishesACompleteBoundary` vérifie un état complet lors de l’annulation confirmée d’une exécution directe. `FailedDirectCapturePreservesStateAndForcesFullRetry` exige de préserver l’état de l’appelant si la capture des registres, de XSAVE ou des métadonnées échoue pendant l’annulation, puis de tout réinstaller au nouvel essai. Aucune génération de l’API ne doit publier un préfixe partiel de registres.

`WhpStateTransferCases.def` couvre aussi chaque préfixe partiel de la capture combinée de 32 registres et les conflits des sept champs de métadonnées. Les deux générations XSAVE doivent préserver l’état appelant et imposer une restauration complète au nouvel essai. La suite vérifie une seule lecture de registres par pas et la récupération des métadonnées omises par XSAVE à partir de cette lecture.

`windows-pe64-v1` prend en charge des processus console Windows x64/ARM64 bornés avec PEB/TEB, TLS statique et dynamique, `DllMain`, API Win32 nommées et graphes DLL explicites sans cycle. Les modules invités acceptent les imports de code/données par nom ou ordinal, DIR64, les exports redirigés et les véritables listes du chargeur. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` et `GetProcAddress` utilisent le catalogue configuré. CRT/GUI, SEH utilisateur ARM64 fondé sur les cadres de pile, threads et compatibilité Windows générale restent inachevés ; les preuves natives ARM64 KVM/WHP manquent encore.

Les octets d’entrée et les étendues cumulées des images partagent chacun `memory_limit` ; le runtime compte aussi dans le budget image. La préparation partage 65,536 enregistrements, 64 MiB de lectures de métadonnées, des noms bornés et l’échéance du travail, sans garantie stricte pour les E/S hôtes. Le fixture original EXE→DLL→DLL vérifie rebasage, ordinaux, données partagées, identité API, `MEM_IMAGE`, listes et attach/detach TLS de l’EXE. `NeverDWindowsProcessTests` inclut l’oracle Windows natif, `NeverDPEProgramExportsTests` les métadonnées invalides et budgets, `NeverDProcessPublicTests` la parité C ABI/CLI. Les transports indisponibles sont explicitement ignorés.

`WindowsProcess.ClockServicesUseConsistentUnitsAndPreserveLastError` exécute les appels PE x64/ARM64 originaux à `QueryPerformanceFrequency`, au compteur monotone, à FILETIME, aux ticks cycliques et au délai relatif. `WindowsProcess.UnmodeledDelaysStopWithoutClaimingCompletion` vérifie le refus avant achèvement des attentes alertables, des dates absolues positives et de l’intervalle INT64_MIN. `NativeWindowsOracleRunsTheSameExecutable` exécute aussi le scénario temporel réussi directement sous Windows ; les deux régressions invitées sont obligatoires pour KVM/WHP sans Unicorn. Le programme original pollue explicitement les bits inutilisés du registre `BOOLEAN` et emploie des constantes de type 64 bits, préservant l’époque et INT64_MIN dans l’ABI Windows.

`ARM64 native backend build` compile `NeverDEmulationNative` avec Unicorn désactivé sur `ubuntu-24.04-arm` (KVM) et `windows-11-arm` (WHP), à partir des sources LLVM épinglées. `audit_native_backend_build.py` contrôle chaque source native déclarée, la définition active du backend, la commande de compilation, l’objet ARM64 ELF/COFF et ses empreintes. `probe_native_host.py` consigne la disponibilité de l’initialisation de l’hôte et la libération des ressources ; les fonctions indisponibles sont explicites et les erreurs d’initialisation font échouer le travail. Cela vérifie la compilation et l’initialisation de l’hôte ; l’exécution de l’invité reste non vérifiée. `NeverDCapstoneCompilerOptions.inc` réserve l’option de diagnostic des qualificateurs aux compilations C avec Clang ; GCC et MSVC conservent leurs propres règles d’avertissement. `native_arm64_only=true` sélectionne ces compilations de composants ARM64 et sondes d’initialisation sans le profil complet de validation CPU x64. Les audits normalisent les racines des sources et de compilation avant de comparer les chemins, y compris les alias Windows 8.3. Le travail de composants ARM64 active explicitement le backend KVM ou WHP choisi avant l’audit de compilation.

`WindowsTestExecution.def` sélectionne la comparaison Unicorn ARM64 `WindowsExclusive` pour `RUN_SERIAL`. Cette politique CTest évite la concurrence avec les autres charges invitées tout en conservant le délai invité initial de 60 s et tous les contrôles des résultats, registres, permissions et empreintes natives.

`run_native_cpu_methods.py` valide la propriété booléenne `RUN_SERIAL` de `NativeMethodExecution.def` et la conserve dans les groupes de méthodes et les contrats comparés entre exécutions. Les méthodes s’exécutent une à une ; un processus enfant non terminé empêche la suivante. Les propriétés inconnues et les contrats de fragments modifiés font toujours échouer la validation.

`WindowsProcessLifetime` exécute les callbacks TLS puis `DllMain` des DLL en ordre de dépendance, puis le TLS et l’entrée EXE, sur un CPU avec un budget commun. Chaque module possède un index TLS et un bloc aligné indépendant, copié depuis l’image relocalisée et liée dans une arène de 64 KiB. L’argument réservé TLS vaut zéro ; celui de `DllMain` au démarrage/détachement du processus est opaque et non nul. Une sortie explicite détache les DLL initialisées en ordre inverse de la liste du chargeur puis le TLS EXE, même avant l’initialisation EXE. `DllMain(FALSE)` au démarrage termine avec `0xc0000142` sans détachement. Fautes et budgets épuisés n’inventent aucun nettoyage. Le retour de l’entrée PE avec DLL invitées exige une terminaison de thread non prise en charge et s’arrête explicitement. `SizeOfZeroFill` non nul reste exclu ; les octets initialisés à zéro du modèle TLS réel sont admis. Les DLL sans entrée reçoivent TLS attach, mais aucune notification de détachement du processus.

`WindowsProcessExports` partage la résolution nom/ordinal entre imports statiques et `GetProcAddress`, avec code, données, alias et redirections en chaîne. Seules les redirections initiales utilisées ajoutent des modules du catalogue et des dépendances d’initialisation ; les autres ne chargent aucun fichier. Les noms respectent la casse ; un nom absent renvoie NULL/erreur 127, un ordinal directement recherché absent (y compris un trou) NULL/erreur 182, et un argument de requête NULL l’erreur 87, un succès conserve LastError. Les handles inconnus restent non pris en charge. Les entrées API exactes fournisseur/nom sont réservées une fois depuis le registre borné. La résolution vérifie les en-têtes PE et métadonnées d’export actuels de chaque image, refuse toute modification ou lecture impossible, limite la chaîne à 64 entrées et partage les crédits de métadonnées restants et l’échéance du processus. Une redirection vers un trou renvoie la base de l’image cible et conserve LastError ; vers l’ordinal zéro, elle renvoie l’erreur 87. Cette base est une adresse de données et n’autorise pas l’exécution des en-têtes. Les redirections à l’exécution peuvent charger les modules configurés et terminer leur initialisation avant de renvoyer le résultat. La modification active de la table des exports reste non prise en charge.

`WindowsProcessLoader` charge les noms de base DLL ASCII de `windows.modules` et gère références explicites, dépendances partagées et maintien des modules de démarrage. Répéter une résolution redirigée ne rajoute pas de référence. Chaque rechargement attribue une nouvelle génération résidente au même emplacement du catalogue. TLS et `DllMain` utilisent le même CPU, sous les cadres API suspendus ; restaurer les registres conserve les écritures invitées et utilise le retour actuel. Les pointeurs réservés attach/detach dynamiques valent zéro. Un attach échoué lors d’un chargement explicite renvoie 1114 après nettoyage, sans annuler les chargements imbriqués indépendants réussis. Le déchargement libère images et TLS ; le rechargement restaure les octets originaux. Toute modification externe des listes du chargeur ou pointeurs TLS est refusée. Les budgets de fichiers, images et métadonnées restent cumulatifs, même après échec. Les fournisseurs système utilisent la base de leur PE mappé comme handle. Recherche de fichiers, chemins non ASCII, options `LoadLibraryEx`, cycles et transitions réentrantes du même module en initialisation/déchargement restent non pris en charge.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` partagent le bloc invité courant des paramètres de processus du PEB. Les noms ASCII ignorent la casse ; les valeurs sont en UTF-16. Les modifications valident les entrées, la capacité et les droits d’écriture avant publication. Les instantanés restent indépendants des modifications et libèrent leur mémoire invitée. Le modèle limite le bloc à 64 KiB ; chaînes et expansions sont bornées et vérifient l’échéance. La propriété inconnue des pointeurs, les blocs mal formés, les pages de codes ANSI et le chevauchement des tampons d’expansion restent non pris en charge. `WindowsEnvironmentTests.cpp` compare des fixtures originales x64/ARM64 sur les backends disponibles ; la CI exige un oracle Windows natif indépendant.

`WindowsProcessHeap` centralise allocation, `HeapReAlloc`, libération et taille du tas du processus. Le redimensionnement conserve les octets retenus ; `HEAP_ZERO_MEMORY` initialise les octets ajoutés et `HEAP_REALLOC_IN_PLACE_ONLY` interdit le déplacement. Un redimensionnement échoué conserve le bloc et renvoie NULL avec `ERROR_NOT_ENOUGH_MEMORY` (8), conformément aux observations natives. Les pages indépendantes restituent leur capacité lors des réductions et libérations ; croissance préparée et copies bornées vérifient l’échéance. Tas personnalisés, indicateurs générant des exceptions, propriété inconnue et plages inaccessibles arrêtent explicitement l’exécution. `WindowsHeapTests.cpp` couvre les deux ISA, déplacement imposé, réutilisation du budget et atomicité des échecs ; CI exécute aussi le même EXE original sur Windows natif. Les cas PE par lots utilisent le délai CTest commun de 120 secondes ; chaque invité conserve son budget fini. La fixture du tas permet 20 secondes par processus pour vérifier toutes les données avec WHP.

`WindowsSystemModules` construit des images modèles PE64 bornées pour `ntdll.dll`, `kernelbase.dll` et `kernel32.dll` sur les deux ISA. Les recherches ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` et `GetProcAddress` partagent leurs bases mappées ; PEB/LDR et `MEM_IMAGE` décrivent les mêmes images. Imports statiques, recherches nommées et redirections invitées partagent les portes API et le résolveur. Les fournisseurs restent résidents, sans rappel invité d’initialisation, et ne bloquent pas le retour du point d’entrée après déchargement des DLL invitées ordinaires. Toute modification des en-têtes ou métadonnées d’export arrête la recherche. Noms système non modélisés et ordinaux non nuls arrêtent explicitement l’exécution ; une différence de casse d’un nom modélisé ou un nom vide renvoie 127, une requête NULL renvoie 87. Octets et adresses générés relèvent du modèle ; dispositions propres aux versions Windows, ordinaux natifs et alias entre fournisseurs ne sont pas reconstruits. `WindowsSystemTests.cpp` compare des EXE originaux x64/ARM64 à Windows natif, avec huit observations indépendantes du retour du thread initial.

`WindowsSectionFixture.inc` vérifie deux vues indépendantes, la réutilisation des handles, la lecture après fermeture, l’isolation après écriture, le démappage et le fournisseur résident. Les cas négatifs couvrent espaces, droits, adresses fixes et décalages. `WindowsThreadFixture.inc` distingue affinité et masquage, longueurs exactes et bits hauts parasites. Les exécutables originaux font aussi partie de l’oracle Windows natif ; Wine sans `KnownDlls` ne valide pas le scénario de section.

`WindowsProcessExceptions` implémente `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` et `RaiseException` sur le même CPU et budget de processus. Les gestionnaires ordonnés peuvent modifier les inscriptions, lever des exceptions imbriquées, appeler les API modélisées, charger des DLL et terminer le processus. Les violations de données x64/ARM64 et divisions entières x64 reprennent après validation des modifications de `CONTEXT`, en conservant registres généraux, SIMD et état FP pris en charge. Les exceptions logicielles reprennent via une véritable instruction de retour du fournisseur modélisé. Limites : 128 inscriptions conservées et 16 cadres imbriqués. Dispositions invalides, pointeurs modifiés, champs non pris en charge et dépassements échouent explicitement. SEH ARM64 et déroulement fondés sur la pile, débogage et fautes d’exécution/de garde restent non pris en charge. `WindowsExceptionTests.cpp` compare des EXE/DLL originaux à Windows natif ; les preuves ARM64 KVM/WHP natives restent à obtenir. Les enregistrements des exceptions logicielles portent `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), indépendamment du drapeau de non-continuation fourni par l’appelant ; l’exécutable Windows original vérifie les valeurs exactes des drapeaux des exceptions logicielles et matérielles.

`WindowsProcessContext` conserve l’origine de chaque cadre de distribution. Les fautes x64 prises en charge d’accès aux données et de division exposent RF (`0x10000`) dans `CONTEXT.EFlags` ; `RaiseException`, même avec un code logiciel de violation d’accès, conserve le contexte courant. L’origine survit à VEH/VCH et à la recherche au déroulement SEH. Une continuation valide restaure les indicateurs CPU logiques sans RF ; les modifications invitées de RF sont rejetées avant publication de l’état. Ce profil limité ne modélise ni points d’arrêt d’instruction ni contrôle invité de RF. `WindowsExceptionTests.cpp` vérifie les enregistrements, la restauration et l’absence de modification CPU/RAM en cas de rejet.

Selon des observations natives indépendantes, Windows ring3 traduit les fautes checked x64 `operand_alignment` en `STATUS_ACCESS_VIOLATION` avec les paramètres `[read, UINT64_MAX]`, y compris pour les écritures. La couche CPU fournit la cause ; Windows ne la déduit pas du vecteur 13 et ne redécode pas l’instruction. `WindowsAlignmentProcessTests.cpp` exécute les instructions PE originales dans 72 scénarios de faute et 9 reprises après correction d’adresse (`72 + 9`), en vérifiant PC, RF, XMM et RAM. Les fautes non classées ou incohérentes restent rejetées. Les rapports de processus et de pilotes préservent `cause` et `error_code` hexadécimal, tous deux nullables, en distinguant absence et zéro. Cette distribution concerne le profil utilisateur checked x64. Après chaque faute ou reprise après correction, le programme exporte la page complète de 4096 octets ; l’hôte vérifie les 81 instantanés et les compteurs réels sans modifier le délai de l’invité. Les observations des processus et threads initiaux natifs utilisent `CREATE_DEFAULT_ERROR_MODE` : GoogleTest active le drapeau hérité `SEM_NOALIGNMENTFAULTEXCEPT`, qui peut amener Windows à corriger les fautes mesurées. L’oracle observe ainsi le comportement système par défaut, indépendamment de la politique du banc de test.

`AddVectoredContinueHandler` et `RemoveVectoredContinueHandler` gèrent une liste ordonnée distincte, avec une limite commune de 128 inscriptions conservées avec les gestionnaires d’exception. Après acceptation de la reprise par un gestionnaire d’exception vectorisé, les callbacks de continuation voient le même enregistrement modifiable et le même `CONTEXT`. La validation finale suit ces callbacks, y compris les exceptions imbriquées et les notifications DLL. Un handle ne peut pas être retiré par l’autre famille de gestionnaires. `WindowsContinuationTests.cpp` compare des EXE originaux à Windows natif pour l’ordre, l’arrêt anticipé, les modifications de liste, la réparation du contexte, les appels imbriqués, le chargement et la sortie du processus. Le chemin vectorisé Windows x64 testé autorise la reprise avec `EXCEPTION_NONCONTINUABLE` ; cela ne prouve pas le comportement SEH fondé sur la pile. L’exécution ARM64 native reste non vérifiée.

`RtlCaptureContext` est disponible via `kernel32.dll` et `ntdll.dll` pour x64 et ARM64. `WindowsProcessContext` et `IntegerABI` partagés enregistrent PC/SP de l’appelant sans modifier le CPU ni LastError. Les observations Windows natives établissent les indicateurs x64 `0x10000f`, la conservation des zones home/débogage/vecteurs non écrites et les adresses x87 historiques sur 32 bits ; ARM64 copie LR vers PC et met X0/LR à zéro dans le résultat. Registres, SIMD et contrôles flottants proviennent de l’invité ; sélecteurs x64 et masque de capacités MXCSR suivent le CPU invité configuré. Les destinations invalides, non alignées ou partiellement inaccessibles échouent avant publication. `WindowsContextTests.cpp` couvre imports directs, recherche par fournisseur, rappels VEH, sorties traversant une page et atomicité des échecs. `scripts/check_windows_context.py` exécute le programme original sous Windows x64/ARM64, avec un oracle natif distinct pour un état x87 non vide. Ces observations ARM64 ne prouvent pas l’exécution native KVM/WHP. Restauration du contexte, parcours de pile et tables de fonctions dynamiques restent à implémenter séparément. `WindowsProcessServices.def` déclare les restrictions exactes par module : la recherche dans `kernelbase.dll` renvoie `ERROR_PROC_NOT_FOUND` (127), conformément aux observations natives, sans inventer un export. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` utilise le `X64SEH` partagé dans `os/windows/exception/` (`NeverDEmulationWindowsException`, disponible sans pilotes) pour x64 `__C_specific_handler` et UNWIND_INFO V1. Après VEH, il gère filtres, finally, transfert non local, exceptions imbriquées, collisions de déroulement et cadres EXE/DLL relocalisés, avec conservation des GPR/XMM non volatils. Une continuation par filtre exécute VCH avec le même `CONTEXT`. `WindowsSEHTests.cpp` compare 23 scénarios originaux à Windows natif ; KVM/WHP/Unicorn partagent cette sémantique. Le budget du processus couvre la revalidation des générations, en-têtes, octets de déroulement/portées, régions du gestionnaire de langage et liaisons IAT. Métadonnées modifiées ou images retenues déchargées provoquent un échec explicite. SEH ARM64 par cadres, C++ EH, tables de fonctions dynamiques, RtlUnwind/NtContinue généraux et déroulement traversant des callbacks du chargeur/VEH/VCH restent exclus.

Pour `EXCEPTION_NONCONTINUABLE`, un filtre x64 renvoyant `EXCEPTION_CONTINUE_EXECUTION` déclenche `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, indicateurs `0x81`, enregistrement lié nul) avec un nouveau contexte. VEH est relancé avant la recherche dans la pile logique conservée, avec le même ordre finally, les mêmes identités de cadres EXE/DLL et les mêmes budgets de profondeur et d’exécution. Les 23 scénarios natifs comprennent 21 exécutions réussies et deux terminaisons : accepter la continuation de cette exception secondaire dans VEH/VCH laisse l’exception non traitée, même après restauration du `CONTEXT` initial. Le modèle signale un échec d’exécution. L’adresse d’une exception logicielle égale le PC sauvegardé ; les adresses du répartiteur interne et la disposition des registres relèvent du modèle. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

`WindowsDynamicTests.cpp` compare des DLL/EXE x64/ARM64 originaux aux observations Windows natives indépendantes : références, dépendances partagées, chargements imbriqués, nettoyage après échec, redirections, sortie, DLL sans entrée et TLS neuf au rechargement. Les régressions refusent les métadonnées modifiées et pointeurs de code périmés, conservent les budgets cumulatifs et les résultats API interrompus non terminés. La CI Windows impose oracle natif et cas WHP ; compilation croisée et Unicorn ARM64 ne prouvent pas l’exécution native ARM64.

Une bibliothèque absente dans la chaîne de transfert de `GetProcAddress` renvoie 127 ; un `LoadLibrary` explicite pour un module absent du catalogue renvoie 126. L’oracle natif et chaque backend disponible vérifient les 41 scénarios déclarés. Sous Windows, le retour après déchargement de toutes les DLL est observé 16 fois par variante de DLL. Un échec d’initialisation lors d’un transfert de `GetProcAddress` renvoie également 127 après nettoyage. Les rappels de détachement du processus préservent le contenu de la pile de l’appelant qui termine.

`WindowsExportTests.cpp` utilise des DLL et EXE x64/ARM64 originaux pour vérifier code/données/ordinaux redirigés, alias, requêtes d’initialisation, rebasage, casse, absences, LastError, cycles, cibles non résidentes, pointeurs invalides et modifications après une requête réussie. Le même EXE dispose d’un oracle Windows natif indépendant ; les cas WHP sont obligatoires en CI native. Les tests C ABI/CLI comparent les rapports complets. Les preuves matérielles ARM64 natives restent à obtenir. Des variantes EXE avec et sans table d’exports couvrent les deux graphes, l’ordre PEB, le détachement et les erreurs nom/ordinal/NULL.

`WindowsLifetimeTests.cpp` compare des traces figées aux processus Windows natifs indépendants et à KVM/WHP/Unicorn : sortie normale, retour d’entrée, deux échecs DLL, quatre sorties précoces et DLL sans entrée. Il vérifie aussi fautes des callbacks, budgets communs, champs TLS relocalisés et capacité TLS cumulée. La sonde native de retour conserve le handle du thread initial et vérifie 64 fois son code de sortie et la séquence exacte de notifications thread/processus. Les threads enfants restants sont terminés après observation ; la sortie du processus ne représente pas le retour de l’entrée.

`NeverDUnpackTests`, `NeverDUnpackExecutionTests` et `NeverDUnpackPublicTests` couvrent la récupération des images compressées ; voir [dépaquetage](unpack.md). `UnpackGeneratedTests.cpp` vérifie les règles d'entrée sur x86-64 et ARM64 avec un programme que le test compresse lui-même. `X64ReturnPrefixTests.cpp` vérifie le retour proche sur deux octets sur chaque transport et que tout autre retour préfixé reste rejeté. `WindowsDeferredTests.cpp` vérifie les entrées opaques et l'observation d'un processus arrêté ; `ExecutionSessionTests.cpp` vérifie les surveillances d'exécution. `DirectX64Tests.cpp` vérifie aussi les surveillances partielles, les instructions à cheval sur deux pages, la reprise unique, les services, les instructions invalides et l’état à l’expiration.

`LiveHeapReferencesCannotPublishAnOrdinaryRecoveredImage`, `ExplicitSnapshotsKeepExternalHeapDependenciesVisible` et `ReleasedHeapStateDoesNotBlockRecovery` comparent un démarrage et une entrée compilés indépendamment sur les backends contrôlés et directs disponibles. `HeapReferencesInCapturedTLSCannotBeDiscarded` couvre les dépendances TLS seules. Les tests publics exigent la préservation des fichiers existants lors du refus et des snapshots identiques entre API C et CLI. Les correspondances restent une preuve conservatrice, pas un certificat d’exécution native.

`DirectServiceBindingsRequireAnExplicitSnapshot` couvre les liaisons directes utilisées pour la première fois avant et après le point d’entrée capturé. Les contrôles C API et CLI préservent aussi la sortie existante en cas de refus.

`PrivateHeapDestructionReleasesOnlyOwnedBlocks` vérifie le propriétaire après déplacement, le refus inter-tas, les handles périmés et la réutilisation de la capacité des tas vivants.

`UnpackLibraryTests.cpp` compacte des DLL x64/ARM64 indépendantes et vérifie l’ordre des dépendances, les callbacks TLS ordinaires/générés, le nettoyage après échec, les identités entrée/hôte, l’accès au propre fichier, les exports nommés/ordinaux/données/redirections et l’absence d’auto-imports. Windows natif charge les DLL originales et reconstruites avec un EXE distinct et appelle les exports déclarés ; les cas WHP contrôlés et directs sont obligatoires. `CompletedGeneratedTLSCallsRequireTheAttachABI` refuse les entrées/arguments modifiés ; `GeneratedCallsNeedTheirReturnedStackAtTheContinuation` refuse une mauvaise pile de retour. Ces tests concernent le dépaquetage, sans dévirtualisation.

`ExportObserver` observe aussi les exports exécutables des dépendances invitées résidentes ; les fournisseurs modélisés restent observés à la distribution des services. Les exports de l’entrée sont exclus. Les changements de modules actualisent les arrêts et chaque réparation exige l’identité courante. Les enregistrements respectent la limite d’imports déclarée. Les DLL exigent la réparation d’un helper API et d’un helper de dépendance ; le chargement natif vérifie l’absence d’adresse émulée résiduelle.

`WrappedEntriesRequireExplicitTransferEvidence` couvre un wrapper DLL appelant son entrée restaurée avec une pile plus profonde. Le défaut reste `no_entry` ; choisir l’appel observé avec `transfer` reconstruit une DLL chargeable. Un appel profond seul ne distingue pas entrée et initialiseur.

`WindowsDeferred.EarlierTLSCallbackMayGenerateALaterCallback` exige une section `.gentls` isolée marquée `IMAGE_SCN_CNT_UNINITIALIZED_DATA`, de taille exactement égale aux tampons déclarés, avec taille et pointeur de données brutes nuls. `WindowsDeferredCases.def` définit le stockage et l’assembleur ; la section `.data` ordinaire reste séparée. Les cas de callback et de point d’entrée générés conservent les contrôles de rejet strict et d’exécution différée sur x64/ARM64.

`ExtendedRegistersLoadOrdinaryImportsAgain` exécute les chargements R8-R15 compacts et avec remplissage en x64 vérifié et direct. Les registres bas couvrent un octet précédent ressemblant à REX et les routines d’adresse avec CALL seul ; les appels avec remplissage sautent des octets arbitraires après CALL. `ImportCallHelpersCannotDiscardPersistentEffects` exige la conservation des effets persistants. `PERebuildTests.cpp` refuse les preuves de début/résultat manquantes et les débuts chevauchants, et préserve le retour API exact sur six à huit octets.

`OpaqueExportCallsAreRepairedBeforeTheExplicitStop` restaure un appel pur sans contourner une API inconnue. `ExportObservationIncludesTheOpaqueBoundary` couvre les exports statiques, dynamiques et par ordinal sans modifier l’exécution ni les journaux de services. `OpaqueExportObservationPreservesAnUnreadableReturn` exige que le retour manquant reste absent.

`ExportIdentitySurvivesRebindingAndLateResolution` modifie l’ordre de liaison d’exports opaques et résout un export après l’entrée. Les cas checked/direct x64 exigent la bonne identité API et conservent l’arrêt explicite unsupported-service.

La mémoire virtuelle Windows ajoute `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` et `FlushInstructionCache` pour le processus courant. La couche OS possède les réservations ; `AddressSpace` reste la référence pour les pages validées, les permissions et leur stockage. Les tests couvrent la réécriture de code, les défauts d’accès et la réutilisation du budget mémoire.

`WriteProcessMemory` suit le comportement des pages engagées observé sur x64/ARM64 pour les écritures de 4 Kio au maximum dans le processus courant. Il conserve la protection de chaque région, les préfixes copiés, les nombres d'octets et LastError, y compris `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` et le succès après un préfixe RX. `WindowsMemoryWriteTests.cpp` contrôle les 25 paires de protections ; `check_windows_memory_write.py` vérifie le même exécutable original dans la CI Windows native. Les destinations non engagées restent explicitement non prises en charge.

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

Les régressions couvrent budgets exacts ou insuffisants, mots partiels, deux ordres d’octets, identité de mémoire intacte, bornes des sauvegardes affines, écrasements par prédécesseurs tardifs et invalidation par défaut. C API/CLI vérifient la compatibilité v6 et les domaines invalides. HighC et LLVMC exécutés en O0/O2 contrôlent retour, mémoire, pile et état préservé, sans constituer un certificat d’équivalence native.

Les régressions couvrent phases en registre et en pile, deux boutismes, branches invalides accessibles, prédécesseurs tardifs, gardes internes épuisées et limites de découverte adjacentes. Les tests de visites couvrent corrélations arithmétiques, boucles imbriquées, modes, passages séquentiels, budget total et priorité historique. Le CLI exécute les deux chemins C et ABI source à O0/O2 ; C/Python v8 vérifient dispositions, champs invalides et extensions futures ignorées. Les régressions vérifient aussi que de grands sélecteurs finis sans rapport laissent du budget aux gardes natives et que le dernier raffinement autorisé revient à un producteur déjà proposé.

`NeverDLLVMCPhiTests` exécute les mises à jour de boucle indépendantes et interdépendantes à O0/O2, avec zéro itération, des bornes de boucle et des valeurs initiales aléatoires sur toute la largeur. Les assertions de lisibilité exigent aucune copie locale pour les mises à jour indépendantes et uniquement la copie nécessaire aux échanges composés. Les cas existants de branchement, switch, déplacement de branche et permutation continuent de vérifier l’arête choisie et les affectations simultanées.

Les cinq tests indépendants de `LLVMCInternalExitRegions` couvrent polarités, zéro tour, échanges PHI parallèles, les quatre imbrications tête/interne et l’ordre des observations. Destinations communes et continuations précoces vérifient un repli exécutable ; une région refusée après une boucle valide ne doit publier aucune structure partielle. Module entier et fonction sélectionnée donnent le même résultat sans modifier LLVM. LLVM original et C émis sont comparés aux oracles non signés à O0/O2 sur 294 912 appels, avec pièges de comportement indéfini pour C.

`NeverDLLVMCPhiTests` vérifie aussi les sorties de boucle communes : paires PHI distinctes des successeurs, ordre des appels d’observation, mémoire de sortie et IR appelant inchangé. Le C du module entier et d’une fonction sélectionnée est comparé à des références indépendantes en O0/O2. Les comparaisons différentes et les prédécesseurs supplémentaires vérifient le traitement conservateur.

`NeverDLLVMCPhiTests` vérifie ce regroupement avec plusieurs arêtes de retour, des oracles O0/O2 indépendants, l’ordre des observateurs, les instantanés précédant des appels modifiant la mémoire, le débordement modulaire étroit et l’extension de signe. Les sorties du module et d’une fonction conservent l’IR initial. Les tests couvrent les arêtes contradictoires, racines partagées, annotations poison, opérandes indéfinis, décalages variables, intrinsèques contraints, exceptions et refus complet faute de budget. Les rotations ne sont regroupées que si toutes les opérations entrantes concordent.

`NeverDLLVMCPhiTests` exécute aussi les régions scalaires structurées à O0/O2 : boucles imbriquées aux blocs réordonnés, losanges, zéro itération, débordement modulaire étroit, observateurs en tête, échanges PHI, valeurs externes vivantes, pas partagés et limites des décalages entonnoir. Les tests vérifient l’IR inchangé, la fusion vers trois variables locales et le repli exécutable des graphes à plusieurs sorties, irréductibles ou trop grands. Ces exemples synthétiques sont indépendants ; le rendu source ne certifie pas la récupération native.

`NeverDLLVMCValueTests` compare le C des boucles scalaires typées au LLVM compilé directement et indépendamment en O0/O2, avec des traps de comportement indéfini pour le C généré. Les valeurs limites et les entrées déterministes sur toute la largeur couvrent multiplication étroite, débordement avant décalage, multiplication et décalage élargis, troncature en booléen, comparaison et extension signées, priorité, expressions conditionnelles, arithmétique booléenne, repli des opérations non prises en charge et matérialisation des expressions profondes. Les tests vérifient aussi la préservation de l’IR appelant et la suppression des conversions redondantes.

`NeverDLLVMCPhiTests` et `NeverDLLVMCValueTests` couvrent les compteurs sur un octet avec incrément/décrément et débordement, les valeurs de sortie fusionnées, les usages intégrés après la boucle, les valeurs externes vivantes et les instantanés PHI. Des comparaisons indépendantes O0/O2 avec traps de comportement indéfini vérifient addition composée, refus de soustraction inversée, multiplication étroite et masques booléens. Les régions imbriquées exigent des compteurs locaux aux boucles, un résultat distinct et un LLVM source inchangé. Une régression exécutable fait coïncider les noms des fonctions externes avec ceux du résultat et du compteur initialement produits, puis vérifie les appels et leurs effets observables.

`NeverDLLVMCValueTests` vérifie les deux positions de la branche neutre pour les mises à jour arithmétiques et bit à bit, les bases inchangées, les dépendances en ligne à l’ancienne valeur, les instantanés de condition, les tests étroits, les branches non neutres et les sélections partagées. Le C généré est exécuté à O0/O2 face au LLVM compilé indépendamment, avec pièges de comportement indéfini. `NeverDLLVMCPhiTests` vérifie aussi les instantanés parallèles et les initialisations conditionnelles qui doivent conserver une portée commune. L’IR appelant reste inchangé.

`NeverDUnicornDecodeTests` vérifie les bits EVEX réservés des formes registre sur les modèles CPU AVX-512/APX, ainsi que la priorité des fautes mémoire ROUND, la conservation de l’état et la reprise. Une sonde Linux x64 indépendante confirme les fautes d’alignement du codage classique et les fautes de page des formes scalaires/VEX. Ces tests du moteur n’étendent pas les instructions admises en mode checked et ne prouvent pas une exécution APX native.

## Mesures CPU ARM64

Utilisez une compilation CPU Release. HVF explicite exige macOS ARM64 natif ; activez Unicorn pour la comparaison logicielle. Chaque résultat est vérifié. L’initialisation est mesurée séparément ; les changements de CPU incluent les appels API et contrôles intermédiaires, les autres charges excluent préparation et vérification.

```bash
cmake --build build-cpu --target neverd-cpu-bench --parallel 4
build-cpu/bin/neverd-cpu-bench --backend hvf --samples 7 --warmup 1
build-cpu/bin/neverd-cpu-bench --backend unicorn --samples 7 --warmup 1
```

Conservez l’exécutable de référence avant de recompiler. Python 3.11+ alterne l’ordre des mesures. Gardez configuration, étiquettes des sources, empreintes binaires et échantillons complets ; ne lancez pas de compilation ou de tests en parallèle.

```bash
python3 scripts/benchmark_cpu.py \
  --baseline /path/to/before --baseline-label BEFORE_COMMIT \
  --candidate /path/to/after --candidate-label AFTER_COMMIT \
  --pairs 15 --output /path/to/new-comparison.json
```

Le comptage des entrées est un diagnostic séparé incluant les sondes initiales et un surcoût ; ses durées sont exclues des mesures de performance. Ces charges ne représentent ni un OS complet ni une comparaison entre ISA.

[Reproduction](../testing.md#reproduce-checked-arm64-cpu-measurements) · [HVF](macos-hvf.md)

`NeverDLLVMPrivateFrameTests` couvre les écritures chevauchantes, toutes les entrées et arêtes de boucle, les adresses conservées, les sorties aliasées, la mémoire non initialisée/ordonnée/inconnue, les métadonnées et le refus atomique aux budgets exact et insuffisant d’une unité. Des oracles indépendants O0/O2 avec pièges de comportement indéfini comparent retours complets, objets externes et octets du cadre. Les compilations x86-64, AArch64, AArch64 gros-boutiste et ARM32 ne prouvent pas leur récupération native. Relancer `NeverDByteMemoryForwardingTests` après toute modification des helpers communs.

```sh
cmake --build build-release --target NeverDLLVMPrivateFrameTests NeverDByteMemoryForwardingTests --parallel 4
build-release/bin/NeverDLLVMPrivateFrameTests
build-release/bin/NeverDByteMemoryForwardingTests
```

`NeverDByteCellScalarizationTests` couvre les mots chevauchants, deux entrées et deux retours de boucle, les accès larges et de largeur non puissance de deux, les deux ordres d’octets, les choix de valeurs stockées et obligations poison, les écrasements partiels de poison, le rejet des usages et les budgets exacts/insuffisants entre objets. Le pipeline Thin/Deep normal doit éliminer les tableaux résiduels. Des oracles O0/O2 indépendants comparent les 24 octets de sortie, les gardes voisins et le retour pour 8 192 entrées et trois versions, soit 49 152 appels avec pièges de comportement indéfini. Les compilations x86-64, AArch64, AArch64 gros-boutiste et ARM32 ne constituent pas une couverture d’exécution native. Exécuter cette cible avec celles du transfert par octets et des cadres privés lors d’un changement des contrats mémoire partagés.

```sh
cmake --build build-release --target NeverDByteCellScalarizationTests --parallel 4
build-release/bin/NeverDByteCellScalarizationTests
```

`AndroidMutexTests.cpp` utilise des programmes indépendants O0/O2, ordinaires/APS2/RELR, pour les trois types de mutex, plusieurs attentes, la reprise de contention, la libération récursive finale, errno, les événements, la mémoire invalidée, les interblocages et le budget cumulé. Les cas tournent sur Unicorn et les KVM/WHP/HVF disponibles ; les autres sont explicitement ignorés. Cela ne prouve pas l’équivalence avec un appareil Android ou un SMP parallèle.

`HighControlFlowSemantics.DeepStableContainersPreserveEveryReturnPath` vérifie 48 niveaux de blocs, boucles, switch et exceptions avec un interpréteur indépendant, sur les chemins empruntés et contournés. Une limite de durée généreuse détecte les parcours récursifs répétés. Cette couverture concerne le HighIR structuré ; la restauration des méthodes de l’image entière exige toujours ses propres contrôles complets d’inventaire et de dépendances.

`SwiftOnceSources.EarlyReturnsKeepExactObjCOnceThunkProofs` couvre les thunks ARM64/x64 avec des appels retain combinés ou séparés. `EarlyOnceCopyReturnsRequireTheSameCompleteTail` refuse les écritures modifiées, les retain absents ou réordonnés, les lectures ordonnées, les résultats modifiés et les entrées externes. `IgnoredNestedReturnCopiesDoNotObserveOnceContext` vérifie les deux sorties réalisables d'un callback void et le flot source après projection.

`HighControlFlowSemantics.ReturnTailCopyKeepsTheOuterLabelOwner` compare les chemins d'entrée et de contournement avec un interpréteur indépendant lorsqu'une adresse réapparaît dans un bloc imbriqué. `ReturnTailCopyIncludesTheFirstChildOfItsLabel` préserve le cas valide du parent et du premier enfant, ainsi que son affectation. Ces contrôles ciblés ne remplacent pas les comparaisons complètes des méthodes et dépendances natives.

`JumpTailCopyKeepsTheOuterLabelOwner` vérifie la même règle d'appartenance pour les fins de saut.

`EarlyStringGetterReturnsKeepOnlyInertOnceAnchors` vérifie les ancres d'instruction vides entre le retour anticipé et l'appel once, et refuse les appels ou écritures intermédiaires.

`SwiftOnceSources.ObjCThunkRootsShareTheNestedCallbackProof` vérifie la preuve commune de la racine et le refus lorsqu’une feuille commence à observer son contexte.

`SourceABI.SwiftPointActionRequiresTwoDoublesAndContext` / `ObjCCallHints.CoreGraphicsPointActionsKeepSwiftFloatingCarriers` vérifie les deux architectures et rejette les fournisseurs modifiés, les imports faibles, le stockage conflictuel et les porteurs ABI périmés. `HighCSourceCalls.SwiftCoreGraphicsPointActionsKeepCoordinatesContextAndOrder` exécute le C généré à O0/O2 avec des vérificateurs indépendants de porteurs Swift et contrôle les bits des coordonnées, y compris zéro signé, sous-normaux et NaN, l’identité du récepteur, l’ordre des appels et les valeurs de garde. Ces vérifications prouvent l’ABI d’appel, pas la restauration complète des méthodes supérieures.

`NativeSourceHints.CGContextCGRectMethodKeepsOrdinaryAndSwiftContextInputs` vérifie l’arbre complet et les porteurs exacts, y compris les membres privés et les signatures refusées. `SwiftFieldReceiver.CGRectMethodSelfKeepsItsLogicalParameterIdentity` et `CGRectMethodRejectsChangedEntryAndReceiverParameter` testent le pipeline et la relecture de publication, refusant tout changement d’indice self, d’entrée ou de type d’argument. Les traces du compilateur couvrent quatre cibles macOS/Mac Catalyst ; cette déclaration d’entrée reste limitée à arm64. `HighCSourceCalls.SwiftCGRectMethodKeepsContextReceiverAndAllCoordinateBits` exécute le C produit à O0/O2 avec un oracle indépendant de porteurs scalaires Swift, vérifiant les bits des quatre coordonnées, les pointeurs context/self distincts, un seul appel et les gardes de stockage.

`NeverDLowInstructionBoundaryTests` exécute les tests de provenance LowIR sans construire tous les fixtures de levée. `BackwardSharedReturnEpilogueKeepsReturnAndCallerFrame` vérifie la libération alignée par ADD et par LDP post-indexé, y compris la restauration du registre de lien dans l’appelant ; le RET X30 original et l’entrée partagée restent représentés indépendamment. `BackwardSharedReturnEpilogueRejectsChangedReturnAndOwnership` refuse un autre registre de retour, BR X30, une libération absente ou non alignée, les restaurations étroites, entrées intérieures, corrections, mappages modifiables ou ambigus, entrées relogeables et autres formats. Décoder une fin partagée ne prouve pas une ABI native : des sauvegardes ou une allocation manquantes dans l’appelant font toujours échouer la preuve de cadre existante.

`NeverDOwnInteriorCallTests` couvre les appels x86 et x86-64 directs vers une étiquette située dans la plage de déroulement de la fonction elle-même, sous une entrée `.pdata` Microsoft x64, une FDE DWARF System V x86-64 et une FDE DWARF i386. Un appel fait uniquement pour l'adresse de retour qu'il empile est élevé en un empilement suivi d'un saut, et le C émis pour les cas x86-64 linéaire et en boucle s'exécute en `-O0` et `-O2` sous AddressSanitizer et les pièges de comportement indéfini. Une cible dont les retours dépilent l'adresse de retour de cet appel reste un appel ordinaire ; un retour après un changement de pile ou sous le pointeur de pile d'entrée est refusé. Une plage reconstituée à partir d'une chaîne d'enregistrement i386 ne délimite pas le corps, si bien que son appel reste un appel.

`NeverDSysVCallContractTests` couvre les contrats d'appel x86-64 System V sur les formes de `QDomNode::save` et `QDomNode::isDocument` de QtXml. Un appelé direct dont le résumé lit un registre d'argument reçoit la valeur de l'appelant, y compris un `this` entrant transmis sans modification ; un appel virtuel prend l'objet qu'un bloc dominant a chargé dans `RDI` ; une méthode qui retourne sans écrire `RAX` sur un chemin et ne fait que transmettre des résultats d'appelés sur les autres est void ; et un octet écrit dans `AL` avant une chaîne de comparaisons est la valeur retournée sur chaque chemin. Les programmes émis s'exécutent en `-O0` et `-O2` sous AddressSanitizer avec des pièges de comportement indéfini.

`ObjCCallHints.CIImageAffineValueKeepsProviderAndPhysicalCopyCarrier` vérifie le fournisseur CoreImage, la fabrique CIImage, le record logique complet de 48 octets et le pointeur x2 ; fournisseurs absents ou incorrects, x86_64 et déclarations contradictoires sont refusés. `ObjCImageValueCopy.OriginalFrameAndCompleteBodyAuthorizePublication` distingue l’appel original des affectations du résultat à la même adresse machine. `RejectsChangedCopyCallBodyAndCurrentImage` refuse 24 modifications de preuves, arguments, écritures, cadres, métadonnées, imports, appels dupliqués et IR sauvegardé, y compris des modifications cohérentes dans MedIR et HighIR. `GeneratedCExecutesAgainstIndependentPhysicalCopyABI` exécute le C généré inchangé à O0/O2 sur ARM64 face à une fonction recevant le pointeur x2 observé indépendamment dans le compilateur ; il vérifie les six motifs de bits flottants, l’identité du sélecteur et du receveur, une seule évaluation, l’objet retourné, les écritures légales sur la copie, les entrées inchangées et les gardes mémoire. Les autres hôtes ignorent ce test d’exécution de l’ABI physique.

`ObjCCallHints.CurrentMethodEncodingMustAgreeWithCachedDeclaration` refuse une ABI en cache incompatible avec l’encodage actuel non vide ou le sélecteur de la méthode ; les clients fournissant uniquement une déclaration gardent leur contrat existant.

`DarwinIndirectRecordCalls.MatrixFrameEffectsRequireExactCurrentContract` couvre les contrats matriciels/affines courants et 22 mutations rejetées du contrat. `ObjCAffineImageValueCopy.CurrentProducerInitializesThePublishedCopy` prouve le passage du résultat SDK à la copie CoreImage et à la reconstruction indépendante pour publication. `RejectsWrongProducerFrameAndSavedIR` vérifie douze mutations par producteur : écritures d’entrée manquantes, résultat hors cadre, fournisseur ou support ABI incorrect et réutilisation d’une entrée Concat consommée. `GeneratedCMatchesOriginalMachineAndSDKResults` exécute le C généré inchangé et les mots ARM64 originaux avec CoreGraphics natif à O0/O2 sur Apple ARM64 : 1000 cas par producteur comparent les 48 octets du résultat, les deux entrées, les identités sélecteur/récepteur, un appel, les objets retournés, les écritures privées et les valeurs de garde. Les autres hôtes ignorent ce test SDK natif.

`MatrixFrameEffectsRequireExactCurrentContract` vérifie aussi le consommateur CGRect et ses 22 mutations rejetées. `ObjCAffineImageValueCopy.CGRectInputUsesTheSameCurrentFrameOwner` valide rotation → emprunt CGRect → réinitialisation par rotation → publication CoreImage. `CGRectBorrowRejectsExpiredInputsAndChangedABI` rejette huit changements d’initialisation, limites, imports et supports ABI. `GeneratedCMatchesOriginalMachineAndSDKResults` exécute aussi cette séquence complète à O0/O2 sur 1000 cas face aux instructions originales et au SDK natif, avec contrôle de l’angle sauvegardé, des 48 octets finaux, des objets et des gardes.

`FrameMetadataAccessorUsesCurrentCatalogAndABI` vérifie la déclaration commune, les deux registres de réponse et la publication actuelle du witness de frame. `FrameMetadataAccessorRejectsChangedImportAndBytes` refuse les imports faibles, les changements de fournisseur/nom/addend, les requêtes contenant des adresses privées, les spills partiels, les mauvais rechargements et les appels originaux modifiés. L’oracle ARM64 original/C généré appelle aussi le véritable accesseur de métadonnées URL de Foundation : les deux variantes exécutent chacune 2048 cas sous O0/O2 et vérifient le choix dynamique du witness, tous les octets de sortie, les entrées conservées, les nombres d’appels et les gardes. Il ne prouve pas l’allocation dynamique de pile ni les effets mémoire des witnesses.

`AArch64ExclusiveTests.cpp` couvre largeurs, paires, acquire/release, chevauchements de registres, alias, défauts, instantanés, annulation ou échec des observateurs et concurrence entre CPU. `RAMReservationTests.cpp` couvre écritures identiques, ABA, réutilisation, annulation et interférences des chaînes et de `ENTER` sur KVM/Unicorn. Les programmes Windows ARM64 originaux exécutent des boucles exclusives. `scripts/check_aarch64_exclusives.py` observe les instructions originales et exceptions d’alignement dans la CI Windows ARM64. Ces observations natives ne prouvent pas l’exécution des backends ARM64 KVM/WHP ; les profils indisponibles restent explicitement ignorés. Les mêmes cas couvrent Unicorn logiciel, les interférences entre contrats, les écritures identiques et ABA, les alias exécutables au cours d’une même exécution, ainsi que `DC ZVA` et l’annulation par les observateurs.
 `windows-alignment-oracle.yml` exécute aussi la sonde ARM64 : 1 320 observations couvrent chaque décalage non aligné, quatre séquences de chargement/stockage et la mémoire accessible en écriture, en lecture seule, inaccessible ou traversant une limite de page. La sonde conserve la largeur complète des registres et les éventuelles écritures partielles précédant une faute. `WindowsExclusiveProcessTests.cpp` compare 1 320 observations Windows ARM64 originales aux empreintes natives de `WindowsExclusiveNative.def`. Seul le placement du code et des données est normalisé ; les registres, métadonnées d’exception et effets RAM sont conservés.

`AArch64AtomicTests.cpp` couvre 168 encodages LSE assemblés indépendamment, les alias de registres, les comparaisons signées, droits, annulations, réservations physiques et transferts NZCV. `scripts/check_aarch64_atomics.py` collecte 1 100 observations Windows ARM64 originales avec résultats, contexte d’exception et empreinte RAM complets ; les tests du parseur refusent les preuves absentes ou incohérentes. L’exécution KVM/WHP native exige une validation distincte. La régression du processus checked est `WindowsAtomicProcessTests.cpp` ; `WindowsAtomicResults.def` conserve les condensats des enregistrements complets.

Les cibles natives structurellement constantes utilisent directement l’ordonnanceur existant avec contrôle de faisabilité. Une cible symbolique unique ne conserve le prédicat entrant qu’après une énumération exhaustive. Les régressions vérifient 128 transferts constants avec le budget de requêtes d’une exécution linéaire et 32 transferts calculés avec deux requêtes d’énumération par transfert, en préservant les bits hauts libres des adresses et les domaines des branches. Les états complets modifiés, l’absence d’alignement, une limite de cibles nulle et les budgets de requêtes ou d’instructions insuffisants sont refusés. Les contrôles existants de refus des cibles multiples et des énumérations incomplètes restent requis.

Une branche native ne conserve le domaine entrant sur une arête qu’après une preuve UNSAT complète excluant l’autre. Les tests vérifient 32 transferts conditionnels dans les deux orientations avec 512 portes du solveur, des budgets de requêtes exacts ou réduits d’une unité et des portes épuisées. Un alignement modifié ou absent, des comparaisons inversées et un état final modifié doivent être refusés ; les tests existants de contrôle indéfini arbitraire et de deux arêtes réalisables restent requis.

Les caches de traduction en bits et le stockage du parcours ne suivent que les nœuds et variables atteints. `NeverDSolverTests` vérifie les identifiants élevés et clairsemés, la croissance du contexte entre assertions incrémentales, la réutilisation des bits en cache, l’extraction des modèles et le changement des hypothèses. Les expressions larges sans lien restent non encodées ; les violations de largeur atteintes, les racines mal formées et les budgets de portes épuisés restent refusés.

`SourceFrameAnalysis.CallStorage*` vérifie les appels exacts, définitions entrantes, initialisation, remplissage, fuites, bornes et cycles sans autoriser la publication du source. `ObjCFrameBlockBorrows.*` vérifie les emprunts synchrones bornés par le descripteur et 19 mutations des imports, en-têtes, ABI et instructions. Le remplissage reste non prouvé ; les champs de propriété non initialisés sont rejetés. Construction du bloc, lectures des captures et fermeture du callback restent des contrôles de publication distincts.

Les tests de publication block/copie couvrent aussi deux plages disjointes de 48 octets, le chevauchement du descripteur, les corps de callbacks modifiés, les instructions et IR périmées, les occurrences d’appel détachées et l’ordre exact de projection. `MixedWidthFrameCopiesMeetEveryInitializedByte` et `FrameCoverageCannotHideMissingBytesOrPointerJoins` vérifient les écritures de 8/16 octets dans les deux ordres de jonction, les octets manquants, l’invalidation après emprunt modifiable et les identités de pointeur conservées après écrasement partiel.

Les tests de relation de boucle couvrent les temporaires fixes et variables pour un nombre arbitraire d’itérations, les offsets distincts, les plans appariés, les plages partielles ou non alignées, les deux ordres d’octets et les rangs temporaires. La composition native réserve le nouveau stockage au candidat. Préfixes absents, octets non déclarés ou non définis, mauvaises projections, affectations omises, comportement modifié et ensembles définis incompatibles refusent la certification. Les budgets exacts d’exécution, de requêtes et d’observations passent ; une unité de moins échoue. L’inférence ne finance pas la preuve suivante. Les durées restent liées au condensat, sans établir un ABI natif ordinaire.

Les tests de relation de boucle native couvrent la collecte conditionnelle différée et les frontières de refus non auditées pour un nombre arbitraire d’itérations. Les plans manuels et inférés revérifient tout le domaine d’entrée et d’induction ; branches invalides accessibles, mises à jour natives modifiées et budgets de requêtes ou d’instructions épuisés refusent les certificats. Les tests lient les octets modifiés des frontières inaccessibles, conservent les défauts stricts et le refus des plans mal formés, vérifient les deux témoins et les options combinées, ainsi que les refus des API statiques et des instructions superposées. Le schéma sémantique 17 lie cette admission ; ABI natif ordinaire et composition source restent des obligations distinctes.

`ObjCSuperGetterSources` couvre les getters CGRect à quatre porteurs, dix mutations de publication rejetées et les appelants Boolean/CGRect partageant un corps machine. L’oracle d’exécution vérifie à O0 et O2 les bits exacts du retour (zéro négatif, infini et charge utile NaN compris), l’identité du récepteur/de la classe et le chargement du sélecteur après l’appel de métadonnées. Apple ARM64 exécute le thunk original et le C généré ; les autres hôtes exécutent le C généré avec leur ABI native de structures.

`LowIRLoopInference` couvre les projections de compteurs de 8, 24 et 32 bits avec bits supérieurs initiaux arbitraires, les deux sens, les registres, les cadres, les temporaires de fonction et les deux ordres des octets. Les preuves complètes réussissent ; résultats modifiés, stagnation, débordement étroit et sorties par égalité sautées sont refusés. Les budgets exacts des opérations, requêtes, chemins, candidats de rang et élargissements réussissent, tandis qu’une unité de moins échoue. Les limites de la preuve finale sont vérifiées séparément pour les opérations, requêtes et observations.

`ObjCCallHints.SDKRecordData*` vérifie les deux enregistrements externes, chaque décalage double, les deux architectures Darwin et les alias du fournisseur, ainsi que les imports modifiés ou faibles, bibliothèques absentes, corrections contradictoires, stockage inscriptible et plages incomplètes. `python3 -m unittest scripts.tests.test_generate_darwin_record_data_declarations scripts.tests.test_generate_darwin_data_declarations` vérifie les conflits de profils, agencements alternatifs, tailles/alignements invalides, TLS et exports propres aux architectures. Reproduire le catalogue avec `generate_darwin_record_data_declarations.py`, le SDK et libclang fixés, le chemin de sortie et `--check` ; cette vérification des déclarations ne démontre ni l’initialisation des résultats natifs indirects ni la récupération des méthodes.

`LowIRLoopInference.ProjectedBounds*` couvre les sorties par égalité avec bits supérieurs arbitraires des compteurs et bornes, les champs de 8/24/32 bits, les trois types de stockage et les deux ordres des octets. Les preuves refusent les bornes modifiées, la stagnation, les sorties sautées, le débordement et les changements des octets supérieurs observés. Les budgets d’inférence et de preuve restent séparés, avec tests exacts et réduits d’une unité.

`LowIRLoopInference.LateCounter*` couvre l’initialisation constante qui ne révèle les champs de 8/24/32 bits qu’après généralisation : registres, cadres, temporaires de fonction, deux ordres des octets et bits supérieurs de borne arbitraires. Les sorties par égalité passent la preuve complète ; stagnation, sorties sautées, bornes changeantes et mutations des octets supérieurs observés sont refusées. Les budgets exacts et réduits d’une unité sont vérifiés séparément pour l’inférence et la preuve finale.

`LowIRLoopInference.ProjectedComparisonBits*` vérifie les égalités étroites mémorisées aux en-têtes de boucle avec bits supérieurs de borne arbitraires : 8/24/32 bits, trois stockages, deux ordres des octets et initialisation constante ou avec bits supérieurs. Les preuves refusent les modifications du cache, des octets supérieurs observés et des bornes, ainsi que la stagnation et les sorties sautées. Les budgets exacts et réduits d’une unité sont vérifiés séparément pour l’inférence et la preuve finale.

`LowIRLoopInference.OrderedComparisonBits*` couvre les deux codages booléens des sorties par ordre non signé : compteurs de 8/24/32 bits, trois stockages et deux ordres des octets. Les preuves refusent les mises à jour non terminantes, comparaisons ou bornes modifiées et mutations des octets supérieurs observés. Les budgets exacts et réduits d’une unité sont séparés. Les fixtures qui changent leur codage réassocient l’empreinte des opérations originales avant preuve.

`LowIRLoopInference.MutablePrefixBounds*` couvre les bornes dérivées de 8/24/32 bits avec bits supérieurs variables, trois stockages de compteur, les deux ordres des octets et les sorties directes/mémorisées. Il vérifie non-terminaison, bornes mobiles par égalité, mutations observables des bits supérieurs/caches et budgets exacts ou réduits d’une unité. Une borne mobile comparée par ordre non signé peut terminer au rebouclage ; une régression en donne la preuve complète.

`LowIRLoopInference.SharedTemplatesFitIndependentOperationBudgets` prouve des compteurs partiels imbriqués avec au plus 1 024 opérations d’inférence et 640 opérations de preuve indépendante. Les expressions pures identiques et les lectures immuables du préfixe sont partagées uniquement dans une reconstruction de point de coupure, en conservant l’identité des opérandes, les largeurs de sortie et les espaces de localisation. Les tests observent les mots complets des compteurs et des bornes, rejettent un calcul de préfixe modifié et refusent les budgets réduits d’une opération. Les cas existants de registres, de trame, de temporaires de fonction, d’ordre des octets et de non-terminaison restent requis.

`LowIRLoopInference.CompletedEntailments*` vérifie l’isolation des sessions entre boucles de frame terminantes et non terminantes dans les deux ordres d’octets, l’épuisement du solveur/des nœuds et les budgets de preuve indépendants. Les régressions à bornes variables testent le budget logique exact et réduit d’une unité malgré les accès au cache ; les preuves natives à contextes répétés vérifient la prise en compte du domaine.

`LowIRLoopInference.IncrementalEntailmentsKeepRollingBudgets` vérifie la réutilisation de l’encodeur dans un même domaine de contraintes. Un changement de domaine supprime l’encodeur ; si la capacité cumulée de portes est épuisée, une seule nouvelle tentative avec un encodeur neuf est comptée comme une requête supplémentaire. Les mots entiers des compteurs et des bornes restent observés dans les deux ordres d’octets, avec des budgets de requêtes exacts et inférieurs d’une unité, le refus des dépassements de portes, de largeur ou de recherche et un budget indépendant pour la preuve finale.

`LowIRLoopInference.RebuiltCounterLanes*` couvre les mises à jour projetées exactement reconnues lorsque les autres bits du compteur sont reconstruits depuis le préfixe ou changent indépendamment. Les cas de registres, de cadre et de temporaires de fonction couvrent les deux ordres d’octets, des compteurs de 1/3/4 octets et des sorties directes ou mises en cache, avec observation complète des compteurs, bornes et étiquettes. La suppression d’une étiquette haute observée, les mises à jour non terminantes, l’absence de garde de sortie et les budgets d’inférence ou de preuve trop courts d’une unité restent refusés. La reconnaissance structurelle ne propose que des élargissements et des rangs ; les preuves complètes des transitions et la preuve finale restent obligatoires. Des budgets fixes d’opérations et de requêtes couvrent aussi les compteurs projetés symboliques, sans transformer les coïncidences du préfixe constant en relations supplémentaires.

`LowIRLoopInference.ProjectedCounterCopies*` couvre les boucles imbriquées qui copient une tranche du compteur via un mot portant une étiquette indépendante avant de l’incrémenter. Les 144 cas d’état complet couvrent registres, cadres, temporaires de fonction, les deux ordres d’octets, des tranches de 1/3/4 octets, des sorties directes ou mémorisées et des copies de mots entiers comme témoins. Les égalités projetées doivent tenir sur les arrivées sauvegardées et chaque transition entrante ; les bits hauts restent indépendants. L’implication dans le domaine de transition peut reconnaître une récurrence additive entre paramètres distincts. Les étiquettes hautes supprimées, mises à jour invalides, gardes absentes et budgets d’inférence ou de preuve finale réduits d’une unité restent refusés.

`LowIRLoopInference.TransferredCounters*` couvre un compteur changeant d’emplacement entre points de coupure, alors que d’autres boucles peuvent éviter chaque point. Les transferts symboliques exacts d’un pas unitaire proposent des gardes du compteur source et un rang de secours avec un autre emplacement à un point ; toutes les gardes et tous les rangs exigent des preuves complètes des transitions. Les 192 cas couvrent registres, cadres, temporaires de fonction, les deux ordres d’octets, des compteurs de 1/3/4/8 octets, des étiquettes indépendantes et des témoins sans déplacement. Résultats ou étiquettes modifiés, mauvais rangs, mises à jour non terminantes, gardes absentes et budgets insuffisants d’inférence ou de preuve finale sont refusés. Le parcours utilise le quota existant de nœuds symboliques ; les sélecteurs de graphe optionnels peuvent garder un budget nul. Les cas incluent le décompte jusqu’à zéro et l’incrémentation jusqu’à une borne d’entrée. La découverte d’un compteur diffère l’élimination des bornes et des gardes de champ jusqu’à la reconstruction de tous les modèles de coupure ; l’élargissement habituel et la preuve indépendante continuent.

`LowIRLoopRefinement.GuardedCuts*` et `BinaryLowIRLoopRefinement.GuardedCuts*` couvrent PC répétés, registres/cadre/drapeaux, ordres des octets, domaines non sélectionnés finis ou cycliques, recouvrement, mauvais côté, généralisation, témoins de valeurs indéfinies, métadonnées, empreintes et budgets. Des tests natifs indépendants prouvent deux contextes R10 au même PC et l’antériorité du contrôle des frontières non auditées. La certification ABI reste séparée.

`BinaryLowIRLoopInference.NativeSelectors*` couvre deux contextes de registre, des contextes distingués uniquement par le cadre, trois domaines conjoints, des modèles inséparables, des mutations des origines et du corps natif, ainsi que les budgets indépendants exacts et réduits d’une unité. Le nombre d’itérations est arbitraire, sans constante d’entrée ajoutée.

`NativeSelectorsGeneralize*` vérifie la récupération automatique et les preuves natives complètes de phases alternées dans les registres et le cadre, y compris les masques d’octet aux bits hauts symboliques. `NativeSelectorState*` couvre les rangs/corps incorrects, les corruptions hors masque, les affectations invalides et les budgets de travail/métadonnées exacts ou réduits d’une unité. Des plans explicites valides composent aussi le constructeur réel avec des vérifications natives complètes avant et après : coupes avec et sans chevauchement, provenance du préfixe original conservée, scans de repli facturés et débordements temporaires refusés. Cette couverture reste distincte de l’inférence automatique.

`DarwinIndirectRecordCalls` vérifie le contrat MakeScale actuel et ses 22 mutations d’import/ABI, puis consomme un résultat privé complet de 48 octets avec la preuve partagée de copie par valeur. Les plages mal alignées, décalées, chevauchantes ou hors cadre sont refusées. Supprimer l’effet d’écriture certaine entraîne aussi un refus, même si l’ABI de retour complète est conservée.

`SourceFrameAnalysis.IncomingResultAddressNeedsCompleteEntryIdentity` rejette dix mutations d’entrée, de porteur ou d’écriture et l’absence d’ABI d’entrée. `NativeSourceHints.IndirectResultTailCallRetainsExplicitOutputAddress` relifte une queue directe et vérifie le paramètre de sortie explicite, six écritures et le contrôle de publication.

`NativeSourceHints.FourDoubleCallerDemandNeedsEveryUnchangedCarrier` contrôle les quatre voies basses, les écritures hautes indépendantes et neuf mutations de déclaration ou de contrôle. `FourDoubleReturnRequiresEveryComputedLowLane` rejette douze cas de résultat incomplet ou de contrat périmé. `DarwinNativeRecordReturns.FourComputedDoublesExecuteAtO0AndO2` compare les 32 octets à un oracle arithmétique indépendant sur 2048 cas par niveau d’optimisation.

`DarwinIndirectRecordCalls.AffineInvertSnapshotsItsCompleteAliasedInput` exécute 2560 cas à O0 et O2, avec entrées/sorties identiques, chevauchantes ou séparées. Il contrôle les bits d’entrée, un appel, les 48 octets du résultat et tout le stockage gardé. Cet oracle valide la copie physique et la capture, pas l’exécution de la machine originale ou du SDK natif. Le test des contrats matriciels/affines courants conserve 22 mutations rejetées par contrat.

`DarwinIndirectRecordCalls.AffineTranslatePreservesScalarBitsAndSnapshotsAliasedInput` exécute 2560 cas à chacun des niveaux O0 et O2. Il vérifie les motifs binaires des deux scalaires, les six champs d’entrée, un appel, tous les octets de sortie et la mémoire protégée pour des positions identiques, chevauchantes ou distinctes. Quatre mutations de l’ABI scalaire et les 22 mutations communes d’importation/ABI sont refusées. Le substitut bit à bit vérifie les arguments physiques et la copie préalable ; ce n’est ni un oracle mathématique de translation ni une exécution du code machine original.

`NativeFloatingReturnProof.HFAResultFieldsNeedTheExactCompleteDefinedCall` couvre neuf sélections de champs et dix-sept mutations refusées de l’appel, du registre, de la largeur, du décalage ou de SSA. `HFAFieldExtractionNeedsADominatingCall` refuse un producteur sur un chemin frère. `NativeSourceHints.HFAFieldTypeRequiresCurrentCallAndFrameProofForPublication` lifte un appelant ARM64 de cinq instructions, relifte le résultat scalaire déduit et vérifie la publication ; un fournisseur incorrect ou l’absence de restauration LR/SP est refusé. Ces tests portent sur les types et projections source, pas sur l’exécution du corps machine original.

La préemption explicite CPU0, le temps virtuel et les limites sont décrits dans [l’ordonnancement des pilotes](driver-scheduling.md).

`SwiftOnceSources.FoldedObjCGetterTailsExecuteOnceAndRetainsAtO0AndO2` replie de vraies queues de retour ARM64/x64, avec retain combiné ou séparé et prédicats séparés ou intégrés, puis exécute le C produit avec des substituts du runtime à O0/O2. Il vérifie les bits du résultat, une initialisation unique, l’ordre des appels et une valeur en cache modifiée. `FoldedObjCGetterTailsRevalidateCurrentStorageAndCalls` refuse les mutations de largeur, ordre, intrinsèques, stockage, résultat, appels et imports actuels malgré un plan enregistré. Ces contrôles utilisent du code et des substituts contrôlés ; ils n’exécutent ni la machine WMF d’origine ni le runtime Swift natif.

`SourceFrameAnalysis.CompleteOutput*` couvre les préfixes complets ou courts, les jointures de tous les retours, les écritures SDK terminales, les octets manquants, les fuites de pointeurs et les restrictions de porteurs. Les cas appelants refusent les certificats absents ou courts, le mauvais alignement, les limites du cadre et le chevauchement des registres sauvegardés, les alias, l’invalidation par une écriture ultérieure et les valeurs opaques vivantes. `NativeSourceHints.CompleteNativeOutput*` rejoue des producteurs et consommateurs ARM64 assemblés, exige toute la plage d’entrée SDK et refuse les preuves périmées de code, CFG, ABI, audit, fournisseur et occurrence d’appel. Ces vérifications portent sur l’initialisation des octets et l’acceptation du code source, sans exécuter le corps machine original ni certifier un retour logique natif.

`MedCallingConvValueFlow.FPInputsFollowOnlyAuthenticatedCallPrefixes` / `NativeSourceHints.PreservedFPPrefixesRetainAllEntryInputsAfterSDKCalls`: Les régressions du préfixe vérifient trois lectures valides et dix-neuf cas refusés de porteurs, préfixes, rattachement d’appel et valeurs inutilisées. Un appelant ARM64 assemblé préserve les quatre voies double entrantes pendant un appel SDK ; un nouveau lifting vérifie l’ABI complète et l’acceptation du code source. La méthode WMF acceptée et inchangée `0x36350` réussit aussi 2048 cas à O0 et autant à O2 face à une expression native CoreGraphics indépendante de retournement, translation, normalisation, concaténation et application, avec contrôle des 32 octets, du récepteur/sélecteur et des protections d’entrée, y compris les dimensions nulles, négatives, infinies et NaN. Le corps machine WMF original n’est pas exécuté.

`SourceABI.SwiftEntryCapturesAndReturnsTheErrorRegisterOnEveryPath` vérifie la capture ARM64/x64, les deux chemins de retour et le refus des preuves de transport absentes ou modifiées. `NativeSwiftCallsKeepBothResultsAndPostCallErrorBranches` conserve le résultat, le registre d’erreur actualisé et la condition suivante, avec collisions de noms privés et les deux ordres d’émission. Le C généré s’exécute à O0/O2 sur la cible Darwin native, comparé à des oracles indépendants de succès/échec. `SwiftFunctionSymbols.RegularExpressionInitializerRetainsContextAndError` refuse les alias, symboles complets modifiés, entrées hors code et formats d’image non pris en charge. Ces tests n’exécutent pas le constructeur WMF original et ne prouvent pas la récupération complète des méthodes appelantes.

`NativeSourceHints.SwiftErrorDeclaration*` reconstruit les entrées ARM64/x64 assemblées après remplacement d’une ABI scalaire observée par la déclaration du compilateur. L’absence ou l’incomplétude de l’audit courant interdit ce remplacement ; les contrats de source explicites dans les options, MedIR ou HighIR restent prioritaires. Les tests d’entrée rejettent aussi une marque de sortie d’erreur MedIR absente ou une largeur d’opérande incorrecte avant la conversion vers HighIR.

La capture d’entrée reste une racine de vivacité lorsque tous les chemins écrasent le registre d’erreur, y compris lors du nettoyage final de la source après la liaison once. `NativeSourceHints.SwiftErrorCallsRequireCurrentDirectNativeProjection` rejette les fonctions appelées absentes, nulles, modifiées ou non prouvées, ainsi que les cibles modifiées, appels indirects, résultats incomplets, opérandes absents et effets incompatibles.


`SwiftFunctionSymbols.RepeatedDeclarationsKeepEveryRecordField` vérifie les enregistrements dupliqués identiques et rejette les noms, tailles, provenances de limites ou origines modifiés. `NativeSourceHints.SwiftErrorCallResults*` teste l’inférence automatique des appelants ARM64/x64 et rejette les cibles actuelles absentes, les opérations machine modifiées, les audits périmés, les ABI incomplets et les extractions de résultats absentes, réduites ou sans lien. L’exécution du code d’entrée couvre aussi des déclarations de débogage facultatives contradictoires en conservant la convention liée et les rôles d’erreur et de contexte.

Les générateurs de witness Swift vérifient `CurrentValueSubject: Publisher` et `Range<Bound: Comparable>: RangeExpression` sur ARM64/x86-64 pour macOS et Mac Catalyst. `scripts.tests.test_generate_swift_witness_contracts` rejette les modifications des entrées génériques, types ou membres de réponse des métadonnées, prototypes, fournisseurs d’exportations et les flux incomplets. `ObjCSourceBindings.SwiftWitnessUndefRequiresExactDescriptorContract` et `SwiftWitnessUndefRejectsUnprovedInputAndABI` vérifient les deux descripteurs sur les deux architectures, avec 33 mutations par descripteur et architecture concernant l’identité runtime/import, le stockage faible ou contradictoire, l’ABI et les effets. Ces catalogues n’accordent aucun contrat de disposition ou d’emprunt de pile.

`scripts.tests.test_generate_swift_data_declarations` vérifie la requête complète du descripteur `String.Index` et rejette les changements de cellules symboliques, octets ou longueurs de recette, flux de métadonnées/cache, ABI runtime et les définitions dupliquées ou manquantes. `ObjCSourceBindings.SwiftRangeIndexDescriptorKeepsItsCompleteRecipe` vérifie le descripteur non initial à l’offset 3 sur les deux architectures ; `SwiftRangeIndexDescriptorRejectsStaleIdentityAndRecipe` rejette 20 mutations par architecture et revalide les indications d’adresse publiées ainsi que l’émission des helpers.

`ObjCSourceBindings.PrivateFramePointerTailRequiresExactStoreOnEveryPath` couvre les cycles de variables objet après fusion des PHI et une valeur de pile privée dans le même cycle. Le cycle d’objets préserve le stockage exact du pointeur ; une valeur de pile, un écrasement partiel ou une fuite inconnue rejette la preuve.

`ObjCCallHints.FoundationGenericNSRangeKeepsSixPointersAndTwoWords` vérifie les deux architectures, fournisseurs et tous les arguments et résultats ; il rejette les imports faibles, décalages, fournisseurs étrangers, symboles périmés et effets d’emprunt inventés.

Le lecteur String de `scripts.tests.test_generate_swift_witness_contracts` vérifie le flux complet, rejette 28 modifications du stockage, de l’ABI et du flux ainsi que sept déclarations ambiguës et borne l’entrée. Les tests de liaison couvrent quatre descripteurs sur les deux architectures avec 33 mutations par paire. L’identité n’accorde aucun agencement de pile ni emprunt.

`PreparedFiniteKeys.*` vérifie la destruction du contexte et le renommage, l’ordre et les limites des projections, l’invalidation explicite après déplacement, les résultats malformés ou incomplets, les domaines vides ou non uniques et les limites exactes de capacité. Les régressions existantes du cache et des décalages couvrent aussi ce chemin.

`SourceABI.SwiftPointTransformKeepsTwoFloatingInputsAndResults` rejette les changements de registres, de disposition, de contexte et de résultat indirect sur les deux architectures. `SourceABI.SwiftPointForwardingPreservesBothIEEECarriers` exécute le code de transfert avec -O0/-O2 et vérifie les zéros signés, sous-normaux, infinis et charges NaN des deux champs. Les tests de déclaration acceptent les formes du compilateur et les arguments nommés, et rejettent les signatures modifiées et les identités ambiguës.

La fixture MainActor vérifie le flux complet des métadonnées fixes et de la table statique, et rejette les changements de stockage, d’ABI, d’extraction ou d’identité, les effets supplémentaires, les déclarations absentes ou dupliquées et les budgets dépassés. Les deux catalogues exigent les trois exports SDK pour chaque cible. Les tests de liaison couvrent les quatre descripteurs sur arm64/x64 avec 33 mutations par descripteur et architecture. Le test d’exécution sur l’hôte compare la table publique pour cinq motifs binaires de l’argument d’instanciation.

`BitVectorEncodingClone.WatchMigrationAndGrowthOutliveTheSource` agrandit les tables et listes après copie et destruction de la source, modifie indépendamment une copie sœur, interrompt la propagation après une visite et vérifie les modèles complets repris contre les clauses originales et des relations booléennes indépendantes.

`ObjCCallHints.SwiftPublishedAccessorsKeepOpaqueValueAndAllKeyPaths` vérifie les deux accesseurs sur ARM64/x86-64 et les deux fournisseurs Combine canoniques, en rejetant neuf mutations d’ABI et huit d’identité d’importation par combinaison. La validation SDK indépendante exécute le C généré des deux configurations source à O0/O2 sur ARM64 et compare les 24 octets, les gardes d’entrée/sortie et deux identités owner sur 128 appels. Huit configurations de compilation croisée couvrent les deux architectures sur macOS/Mac Catalyst. L’oracle conserve les références consommées par le setter et n’accorde aucun raccourci de propriété ou d’emprunt au produit.

`ObjCCallHints.SwiftMainActorSharedKeepsObjectAndMetatypeContext` vérifie les transporteurs complets du résultat et de swiftself sur ARM64/x86-64, en rejetant dix mutations d’ABI et dix mutations d’identité d’import par architecture. La validation indépendante du SDK exécute le C généré sans modification pour les deux configurations d’architecture source à O0/O2 sur un hôte ARM64 : 128 appels préservent l’identité du singleton et du métatype ainsi que l’équilibre des références. Huit configurations de compilation croisée couvrent macOS et Mac Catalyst sur les deux architectures ; l’exécution native x86-64 constitue une couverture distincte.

`BitVectorEncodingClone.RootQueuePreservesDecisionsAcrossGrowthAndBudgets` vérifie le mélange de variables racine et indécidées, les racines hors décision, la copie d’une copie, la destruction des sources, l’ajout de variables, les deux polarités par défaut, l’interruption bornée et la reprise, les conflits et redémarrages. Les modèles complets et tous les compteurs doivent correspondre à un encodage neuf.

`ContextFiniteProofs.*` vérifie l’isolation des contextes et propriétaires, leur remplacement, le déplacement des jetons, les prédicats exacts et projections ordonnées, l’ajout de nœuds, les résultats complets ou incomplets, les limites de stockage et l’éviction LRU. Les tests de trame exigent la requête finale d’unicité avant stockage et maintiennent la limite de nœuds même en cas de succès du cache.

`LinuxPriorityTests.cpp` vérifie les états explicites, l’isolation des threads, les observations absentes, le JSON mal formé, l’admission des profils et les effets des refus. Des programmes bruts x64/AArch64 indépendants à O0/O2 vérifient le bornage nice, la réduction des arguments à 32 bits, les permissions CAP_SYS_NICE/RLIMIT_NICE et le codage getpriority du noyau. Exécuter `LinuxPriority.*` et `Backends/LinuxPriorityProcess.*` dans `NeverDLinuxProcessTests`, puis les suites complètes des processus Linux, Android natif et API publique de processus pour les changements partagés noyau/JSON. Les transports natifs facultatifs absents restent des tests explicitement ignorés.

`LinuxKernelAvailability.*` valide les entrées d’absence explicite et l’admission des profils. `Backends/LinuxKernelProcess.*` utilise des programmes bruts indépendants x64/AArch64 à O0/O2 pour vérifier ENOSYS avant les arguments et le rejet persistant des appels non spécifiés ou sans rapport. Le test Android compare SVC brut à `syscall` de Bionic, en distinguant retour brut et effets sur errno. Les changements de disponibilité nécessitent ces tests ciblés, les suites complètes des processus Linux et de l’API publique, ainsi que les suites Android syscall, entrée native et signaux.

`CompletedQueryCache.*` vérifie les domaines complets sur un octet, chaque emplacement compact, la croissance, l’isolation du contexte et du propriétaire, les entrées invalides ou incomplètes et les limites exactes de stockage. Les branches natives conservent leurs coûts logiques fixes et les budgets exacts ou réduits d’une unité malgré le travail évité par les réponses complètes.


`BinaryLowIRRefinement.NativeTargetDomainsKeepIndependentProjections` / `FrameOffsets.FrameAndJointTargetProjectionsKeepIndependentSearches` vérifient les chaînes répétées de cibles natives avec changements de branche et écritures de trame symboliques, les coûts logiques fixes, les budgets exacts ou insuffisants d’une requête, les limites de cibles invalides, l’épuisement des portes et les observations finales incorrectes. Les projections alternées de trame et de cibles corrélées préservent aussi les tuples complets, l’ordre des observations et le refus des résultats incomplets lors du remplacement du prédicat.
`FrameOffsets.Cached*` vérifie les translations avec bits hauts libres de la racine, le repliement non signé, les changements de forme des sommes, les deux modes de cache, la séparation des prédicats, la capacité nulle, les refus de budget de requêtes/nœuds et la distinction entre domaines vides et non uniques. La première requête conserve une preuve complète ; les translations suivantes peuvent utiliser une preuve déjà achevée sans budget de requêtes restant.

## Contrats des noyaux Android GKI publiés

`AndroidTestExecution.def` donne à `FiniteRegistryRejectsBeforeSuccessAndCanBeReused` un délai CTest total de 120 secondes et `RUN_SERIAL`. Chacune des deux charges conserve sa limite propre de 30 secondes ; la sérialisation évite la contention entre tests de capacité. Les six variantes O0/O2 et de relocation suivent cette politique.

`LinuxPIDFD.*` vérifie avant chargement les branches, enums invalides, conflit GKI/absence et catalogues mal formés, excessifs ou contradictoires. `Backends/LinuxPIDFDProcess.*` exécute des appelants O0/O2 x64/AArch64 indépendants pour les huit branches: flags, allocation commune, limites, fermeture/réutilisation, catalogue fermé, erreurs de non-chef et ordre scalaire/vectoriel. Les cibles absentes et non-chefs précèdent l’épuisement FD; les flags de thread et le catalogue vide avec self implicite sont couverts. Les vecteurs opposent longueur négative précoce et métadonnées suivantes inaccessibles, ainsi qu’une étendue originale dépassant la limite utilisateur alors que l’étendue plafonnée tient, pour pidfds et les deux flux capturés. Les cas Android `ReleasedGKIProcessDescriptorsShareRawAndBionicOwnership`, `ReleasedGKIVectorImportRetainsRawAndBionicErrors` et `ReleasedGKICatalogueRetainsRawAndBionicLookupErrors` conservent erreurs brutes, errno Bionic, catalogue et épuisement sur six profils de relocation. Exécuter ces cas puis les suites Linux processus, Android native et API publique complètes. Les tests exécutent le modèle sans démarrer les huit noyaux GKI. Voir les [contrats GKI publiés](../android-gki-kernels.md).

`ProcessCPUClocksRetainIdentityAndIdleSeparation`, `ProcessCPUClocksKeepMissingObservationBoundaries`, `LinuxClock.ProcessCPUObservationsShareAliasesAndRemainFixedWhileIdle`: Vérifie les huit révisions : identité brute/Bionic, PROF/VIRT/SCHED, 32 bits bas, cible avant faute de pointeur, observations absentes, alias, secondes CPU non négatives et séparation CPU/repos mural. `AndroidTimeTests.cpp` vérifie sorties et sentinelles ; le syscall coopératif vérifie le TID courant non chef.

`ZeroTimeoutPollRetainsReadinessAndOrderedCopies` vérifie les appels bruts O0/O2 des huit GKI : descripteurs vivants, négatifs ou fermés, doublons, réduction des arguments, ordre délai/masque, timespec nulle en lecture seule, import complet avant sélection et conservation des premiers `revents` lors d’une faute ultérieure. `ZeroTimeoutPollKeepsUnobservedBoundaries` maintient les limites inconnues du noyau, des ressources, masques, attentes et disponibilités. `ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` vérifie sous Android la table commune et errno dans six profils de compactage.

## Attributs de répertoire groupés bornés

bulk-attributes vérifie groupes complets, ensemble noms/types, gardes des octets inutilisés, low32 FD, mots bitmap, erreurs natives, dup, open indépendants, EOF et rewind zéro. Modes littéral/inconnu uniquement virtuels. Les modèles couvrent aussi stat complet, invalidation, noms NFD/255 octets, alias entrée/sortie, erreurs de transport/budget, déplacements/SWAP/suppression/réutilisation et droits explicites. Inventaire requis :63 cas par plateforme,189 ARM64 et126 Intel. Seul ARM64 HVF correspondant est vérifié localement. native5s, guest/Python5,000,000us/quantum1024 et public10s sont inchangés.

`MaterializedRuntimePreservesOwnedObjectsOnNativeWindows` vérifie exécution modélisée originale, restauration, permissions et exécution native Windows des deux images : réallocation/libération du tas, pointeurs intérieurs encodés, réarmement FLS, verrous récursifs, LastError et pages virtuelles réservées, engagées et protégées. `MaterializationRequiresKnownSupportedState` refuse version absente et TLS dynamique. `RuntimeRestorationHasTheSameCAPIAndCLIContract` compare octets exacts et rapports. Les contrôles de construction Linux et observations Wine ne remplacent pas les preuves de durée de vie sous Windows natif.

`NeverDUnpackDriverTests` couvre entrée, imports noyau, ressources conservées, ABI, exports, ordonnancement, requêtes et déchargement, C API/CLI et somme PE. Les cas KVM/WHP obligatoires et ImageHlp sous Windows ne prouvent pas un chargement noyau natif. [UNPACK](unpack.md).

## Tests de l’état opaque

`X86PreservedState.*` vérifie formes scalaires fraîches, alias exacts, réinitialisation stricte et refus des octets/séquences/versions périmés. `OriginalBinaryUndefinedIndependence.*Opaque*` couvre branches, appels internes, cibles indirectes exhaustives, profils exacts et budgets de métadonnées exacts/moins un calculés par décodage indépendant. `BinaryLowIR*.*Opaque*` couvre témoins et choix indéfinis arbitraires, sources inductives multiples, refus tardifs de rang/budget, préservation scalaire depuis l’entrée réelle et modification d’octets d’une source ultérieure à LowIR identique mais résumé différent. `NativeUndefinedIndependence.*Opaque*` et `NativeStackControl.*FreshMemoryCall*` vérifient intérieurs de groupes, frontières avant coupure, reçus périmés et cible évaluée avant mutation de pile. Reconstruire les consommateurs concernés, dont `NeverDInterpreterLLVMRefinementTests` ; rapporter séparément sanitizers, fautes compilées et tests ordinaires.
