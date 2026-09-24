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

`KernelRemoveLocksTests.cpp` vérifie l’indépendance des identités de verrou et de périphérique, les Tags NULL ou répétés, les tailles retail/DBG exactes, le vidage immédiat et différé, les obligations après un acquire échoué, l’atomicité des erreurs, la capacité et le retrait du stockage. `KernelRemoveLockBridgeTests.cpp` et `DriverWDMRemoveLockTests.cpp` couvrent l’initialisation avant attachement, le stockage opaque dans l’extension, les limites IRQL, la libération après retrait du paquet, la fin du fournisseur après le vidage du verrou, la disponibilité dès la dernière libération avant le retour du callback, les workers en attente et le nettoyage d’un échec AddDevice. Le fixture original `driver_wdm_remove_lock.c`, compilé avec le véritable WDK, existe en variantes retail/DBG et normale/avec CFG actif via les chemins facultatifs `NEVERD_WDM_REMOVE_LOCK_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` et `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`. Les tests publics C API/CLI utilisent le schéma PnP existant et conservent des observations distinctes de réception du bus, de fin du bus et de démontage final. Les artefacts absents sont explicitement ignorés ; les preuves d’exécution restent limitées à Linux et n’établissent ni Driver Verifier complet ni le vidage général de requêtes simultanées.

`DriverResourceScenarioTests.cpp` vérifie les faits JSON/natifs explicites, largeurs entières, nombres, chevauchements physiques/de registres, alignement, ID, banques vides et sérialisation. `KernelMMIOTests.cpp`, `KernelMMIOFailureTests.cpp`, `KernelResourceBridgeTests.cpp` et `UnicornMMIOTests.cpp` couvrent propriété des banques/mappages, alias, générations, durée des listes compactes, chronologie du fournisseur, persistance au redémarrage, accès après surprise/alimentation, transactions CPU/API exactes et atomicité des échecs. Le fixture WDK original `driver_wdm_resources.c` utilise `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`. `DriverWDMResourceTests.cpp` exécute les vrais accesseurs scalaires et REP, le rebasage normal/CFG actif, les alias de sous-plages, les mappages de fin de page, STOP/redémarrage et les accès invalides. C API/CLI refusent les faits invalides avant chargement et exécutent le même scénario de 14 requêtes avec sorties IOCTL persistantes et nombres exacts de map/unmap. Le fichier partagé [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) exige le protocole registres/IOCTL de ce fixture. Les artefacts absents sont explicitement ignorés et les preuves se limitent à Linux ; aucune mémoire physique hôte ni backend général de périphérique n’est exercé.

`DriverDMAScenarioTests.cpp` valide capacités explicites, domaines logiques, limites d’octets/nombre/temps, directions strictes et séparation configuration/observations. `KernelPhysicalMemoryTests.cpp` et `BackendBackingTests.cpp` vérifient les frontières d’allocations partageant une page, les références de maintien, les permissions CPU inchangées, l’exclusion MMIO/réentrance et l’atomicité des échecs de validation sur toute une plage. `KernelRequestMDLTests.cpp` vérifie les alias de descripteurs construits contre les mêmes identités physiques. `KernelDMATests.cpp`, `KernelDMABridgeTests.cpp` et `SchedulerDMATests.cpp` exercent les octets RAM réels, appels de table liés à l’adaptateur, propriété immédiate/FIFO, durées de vie séparées des callbacks/mappages, fragments de pages, directions erronées, contrôle préalable de libération, domaines PDO indépendants et échecs d’époque/alimentation. Le pilote WDK original `driver_wdm_dma.c` utilise `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`. `DriverWDMDMATests.cpp` et les tests C API/CLI exécutent les vrais pointeurs d’adaptateur, le stockage commun/SG et les événements DMA/interruption configurés séparément. Le fichier partagé [driver-dma-scenario.json](../examples/driver-dma-scenario.json) exige le protocole de cette fixture. Les artefacts absents entraînent un saut explicite ; les preuves Linux ne démontrent ni DMA hôte réel, ni PCI, ni moteur général de périphérique. `pluginsdk/python/tests/test_driver_dma_integration.py` exerce la liaison JSON existante avec gestion de propriété via `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_FIXTURE` et `NEVERD_TEST_WDM_DMA_CFG_FIXTURE`, notamment les octets, l’ordre des callbacks et les échecs rapportés.

`KernelSEHTests.cpp` vérifie les plans purs de déroulement, l’ordre des portées, la restauration des GPR non volatils, les piles bornées et les métadonnées explicitement non prises en charge. `KernelExceptionTests.cpp` vérifie l’arité API exacte, les statuts sur 32 bits bas, les exceptions typées, les limites IRQL et l’absence de modification de l’état modèle/CPU. Le véritable WDK `/GS-` `driver_wdm_seh.c` utilise les options `NEVERD_WDM_SEH_FIXTURE` / `NEVERD_WDM_SEH_CFG_FIXTURE`. `DriverWDMSEHTests.cpp` exécute des images normales, CFG actif et rebasées avec levées directes ou depuis une fonction auxiliaire, handlers imbriqués, nouvelles levées, exceptions non capturées et refus explicites des filtres/finally/défauts CPU. L’API C/CLI exécute [driver-seh-scenario.json](../examples/driver-seh-scenario.json) et vérifie les résultats API null avec les véritables messages du handler invité. `pluginsdk/python/tests/test_driver_seh_integration.py` utilise `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_SEH_FIXTURE` et `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`. Les images externes absentes sont explicitement ignorées ; les preuves restent limitées à Linux et n’établissent pas la prise en charge des tampons utilisateur ni du SEH général.

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
