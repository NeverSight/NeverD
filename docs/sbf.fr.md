**Langues**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Décompilation Solana SBF

[← Index de la documentation](README.fr.md)

NeverD charge les artefacts déployables Solana comme programmes SBF de premier
rang et expose tout le parcours par le CLI et `libneverd` :

```text
SBF ELF
  → loader ELF et verifier sensibles à la version
  → LowIR sans perte + CFG
  → MedIR normalisé + faits sur les registres
  → fonctions, syscalls, observations CPI/account et régions récupérées
       ├─ LLVM IR vérifié
       ├─ C11 portable
       └─ Rust stable sûr
```

L’implémentation suit la VM Anza `sbpf` actuelle, pas le eBPF Linux générique.
Les metadata de version, opcode, syscall, relocation et protocole résident dans
les bases `.def` sous `include/neverd/sbf/`; loaders et backends consomment les
tables typées générées sans dupliquer encodages ni noms.

## Entrée et versions VM prises en charge

L’entrée est un programme Solana ELF64 little-endian (`.so`).

| SBF | Layout ELF | Machine ID | Comportement ISA notable | État |
|-----|------------|------------|--------------------------|------|
| v0 | sections/relocations legacy | `EM_BPF`, `EM_SBPF` | frames fixes à trous virtuels, LDDW, anciens opcodes mémoire | legacy |
| v1 | sections/relocations legacy | `EM_BPF`, `EM_SBPF` | frames de pile ajustées manuellement | legacy |
| v2 | sections/relocations legacy | `EM_BPF`, `EM_SBPF` | arithmétique PQR, encodages mémoire déplacés, soustraction immédiate inversée, CALLX par registre source | legacy, non monotone |
| v3 | program headers stricts, sans relocation dynamique | `EM_BPF` | syscalls/calls statiques, JMP32, CALLX par destination, bytecode à `0x100000000`, rodata à zéro | format du toolchain déployé actuel |
| v4 | program headers stricts, sans relocation dynamique | `EM_BPF` | ISA v3 plus contrat de memory mapping aligné | upstream `sbpf` actuel ; disponibilité cluster variable |

Un numéro de version n’est pas en soi une spécification : `SBFVersionFeatures.def`
détient donc les changements de comportement et la table des versions les
compose. Chaque enregistrement porte la proposition SIMD qui a accepté le
changement ainsi que le prédicat exposé par `anza-xyz/sbpf` pour la même
question, car plusieurs propositions atterrissent dans une version et une seule
proposition change plusieurs choses sans rapport : SIMD-0173 déplace les classes
d’instructions mémoire et retire `lddw`, tandis que SIMD-0174 ajoute
indépendamment la classe PQR dans la même version. Consigner la proposition sur
la fonctionnalité plutôt que sur la version est ce qui rend une version
récupérée traçable jusqu’au document qui l’a décidée, et c’est pourquoi les deux
règles `callx` sont des fonctionnalités distinctes : SIMD-0173 lit le registre
source et SIMD-0377 le registre destination.

Les changements v2 ne s’appliquent volontairement pas à v3. Les feature checks
sont explicites, jamais des suppositions `version >= N`. Strict, par défaut,
rejette headers, plages ou alignements mal formés, sections legacy writable non
supportées, continuations, registres, écritures frame-pointer ou branches
invalides et opcodes inactifs, avec slot et adresse virtuelle.

## Le runtime dont parle une description

La version de l’ISA vient du fichier. Presque rien d’autre n’en vient. Quels
syscalls se résolvent dépend de la chaîne et du slot ; à quels octets se trouve
un champ de compte dépend du loader qui possède le programme ; si l’entrypoint
reçoit un second argument dépend d’un interrupteur que la chaîne actionne ; et
savoir si un programme peut être déployé est une autre question que savoir s’il
s’exécute. Un unique commutateur de version ne peut rien exprimer de tout cela :
ce sont donc des axes séparés avec des tables séparées.

`SBFRuntimeFeatures.def` consigne les clusters, les usages et les gates qui
changent ce que NeverD rapporte, chacune avec son identifiant de runtime, le
compte feature dont l’état enregistre l’activation et le slot auquel chaque
cluster l’a activée. Un compte pending peut exister sans activer sa gate.
Une gate sans ligne pour un cluster n’y a pas été activée. `simd-0321` est active
sur tous les clusters ; `simd-0449` et le syscall SHA-512 le sont sur testnet et
devnet et pas sur mainnet, ce qui est exactement pourquoi un programme qui marche
sur devnet échoue sur mainnet.

Dans la révision Agave épinglée, la gate
`syscall_parameter_address_restrictions` (`simd-0459`) renforce le contrat
d’adresse VM et d’alignement des paramètres de syscall et de CPI ; l’état RPC
finalisé enregistre son activation aux slots 429,840,000 sur mainnet,
407,468,256 sur testnet et 462,240,000 sur devnet. La gate
`account_data_direct_mapping` remplace la copie des données de compte dans le
buffer d’entrée par des régions mémoire directement adossées lorsque l’espace
d’adressage ajusté est utilisé ; elle n’est pas activée sur mainnet et s’active
aux slots 408,332,256 sur testnet et 463,968,000 sur devnet. Aucune de ces gates
ne crée un nouvel ABI de compte ni ne modifie les offsets logiques ABIv0/ABIv1 :
le loader propriétaire choisit toujours la sérialisation, et NeverD les
enregistre comme métadonnées de topologie du runtime.

Les bits de feature restent append-only. Le snapshot observable dépassant
désormais 32 bits, `RuntimeFeatureMask` est l’unique type `uint64_t` de stockage
et de host ABI. `RuntimeFeatureDisposition` distingue un `RuntimeBranch` vivant
La largeur de l’ABI v2 est figée et ne s’étend pas in-place ; au-delà de 64 bits, il faut v3 ou une représentation multiword, jamais modifier la largeur de v2.
d’un `FoldedBranch` dont le côté actif est inconditionnel dans la révision
épinglée, mais dont l’ancien côté compte encore aux slots historiques.
Activations RPC finalisées (`—` signifie non activé) :

| gate | domain / disposition | mainnet | testnet | devnet |
|------|----------------------|---------|---------|--------|
| `disable_deploy_of_alloc_free_syscall` | `ProgramAdmission` / `FoldedBranch` | 209,088,008 | 195,356,264 | 224,208,000 |
| `enable_bpf_loader_set_authority_checked_ix` | `LoaderManagement` / `RuntimeBranch` | 251,424,000 | 247,628,260 | 255,744,000 |
| `remove_bpf_loader_incorrect_program_id` | `LoaderManagement` / `FoldedBranch` | 237,168,000 | 224,300,256 | 247,104,000 |
| `simplify_alt_bn128_syscall_error_codes` | `SyscallSemantics` / `FoldedBranch` | 274,320,000 | 278,300,256 | 308,448,000 |
| `abort_on_invalid_curve` | `SyscallSemantics` / `RuntimeBranch` | 311,904,000 | 300,764,256 | 342,576,000 |
| `deplete_cu_meter_on_vm_failure` | `VMFaultPolicy` / `RuntimeBranch` | 327,888,000 | 319,340,257 | 364,176,000 |
| `fix_alt_bn128_multiplication_input_length` | `SyscallSemantics` / `FoldedBranch` | 361,152,000 | 346,988,256 | 397,440,000 |
| `raise_cpi_nesting_limit_to_8` | `CPIExecution` / `RuntimeBranch` | — | — | — |
| `increase_cpi_account_info_limit` | `CPIExecution` / `FoldedBranch` | 403,056,000 | 385,868,256 | 435,456,000 |
| `poseidon_enforce_padding` | `SyscallSemantics` / `FoldedBranch` | 406,080,000 | 385,868,256 | 438,048,000 |
| `fix_alt_bn128_pairing_length_check` | `SyscallSemantics` / `FoldedBranch` | 406,944,000 | 385,868,256 | 438,480,000 |
| `alt_bn128_little_endian` | `SyscallSemantics` / `RuntimeBranch` | 425,088,000 | 406,604,256 | 456,192,000 |
| `enable_alt_bn128_g2_syscalls` | `SyscallSemantics` / `RuntimeBranch` | 425,520,000 | 406,604,256 | 457,056,000 |
| `loader_v3_minimum_extend_program_size` | `LoaderManagement` / `RuntimeBranch` | 432,864,000 | 416,540,256 | 470,880,000 |

Ce périmètre ne prétend volontairement pas couvrir tout le `FeatureSnapshot`
d’Agave. NeverD n’inclut les gates loader, verifier, VM, entry/input, syscall et
d’infrastructure CPI que lorsqu’elles modifient directement le décodage ou le
host contract émis. Scheduling de transaction, fees, consensus, vérification
precompile au niveau transaction et sémantique métier d’une `CPI target built-in`
relèvent de l’`external runtime` ; ajouter leurs bits sans implémenter ces
built-ins annoncerait une capacité inexistante.

`SBFLoaders.def` consigne la propriété et la sérialisation. Déployer et exécuter
ont cessé d’être la même réponse il y a des années : `loader-v1` et `loader-v2`
refusent toute instruction de gestion qu’on leur envoie et continuent d’exécuter
les programmes qu’ils possèdent déjà, et c’est pourquoi leur sérialisation doit
rester lisible.

| Loader | Sérialisation | Déploie | Exécute |
|--------|---------------|---------|---------|
| loader-v1 | `abi-v0` | non | oui |
| loader-v2 | `abi-v1` | non | oui |
| loader-v3 | `abi-v1` | oui | oui |
| loader-v4 | `abi-v1` | non | non (built-in retiré) |

`SBFAccountLayout.def` place chaque champ de compte sous chaque sérialisation.
Les deux ne diffèrent pas seulement par le padding : elles ordonnent les champs
différemment, si bien qu’à l’offset trois la forme non alignée porte le premier
octet de l’adresse du compte et la forme alignée son drapeau exécutable, sans que
la valeur annonce laquelle a été lue. Un compte répété occupe en outre un octet
en `abi-v0` et huit en `abi-v1`, ce qui désaligne un parcours des entrées et non
un seul champ.

Savoir si un appel se résout est trois questions et non une :
`SBFSyscallLifecycle.def` détient le degré de stabilité de la signature publiée
et `SBFSyscallRegistration.def` le reste — dans quel registry un syscall figure,
quelle gate le gouverne et dans quel sens cette gate pointe. Le sens compte parce
qu’une gate peut retirer aussi facilement qu’ajouter : activer
`disable_fees_sysvar` est ce qui a supprimé le syscall du sysvar de fees, et lire
une gate qui retire comme une gate qui ajoute inverse la réponse pour tous les
clusters d’un coup. `sol_alloc_free_` reste enregistré pour l’exécution des deux
côtés de la limite. Le deployment l’enregistrait avant
`disable_deploy_of_alloc_free_syscall`, puis le refuse à partir du slot
d’activation propre au cluster. La révision Agave épinglée a replié le côté
deployment actif dans la construction du registry ; NeverD conserve la gate pour
qu’un profil historique obtienne la réponse antérieure à l’activation.

Sur un runtime qui a activé `simd-0321`, l’entrypoint reçoit aussi l’adresse des
données de l’instruction dans `r2`. NeverD la modélise comme une sorte de valeur
à part plutôt que comme une constante, car l’endroit où elle tombe dépend des
comptes : inventer une adresse permettrait de rapporter un load qui la traverse
comme un champ de compte nommé. Avant activation le registre arrive à zéro, et un
programme qui le lit lit un zéro. Les entry points LLVM, C et Rust générés
prennent donc le tampon d’input et les données de l’instruction, car un callable
auquel on ne peut pas donner le second ne peut pas reproduire un programme qui le
lit.

Le toolchain Solana actuel utilise `cargo build-sbf`. Les programmes v3+ sont
orientés Rust et l’ancien toolchain C ne cible pas v3, sans limiter NeverD :
toute entrée acceptée peut devenir C ou Rust.

- [Programmes Solana](https://solana.com/docs/core/programs)
- [Exécution](https://solana.com/docs/core/programs/program-execution)
- [Référence syscall](https://solana.com/docs/core/programs/syscall-reference)
- [VM Anza sbpf](https://github.com/anza-xyz/sbpf)
- [Changelog Agave](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

## CLI

```bash
neverd info program.so
neverd headers --json program.so

neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

neverd lift -o program.ll program.so
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so

# Indiquer de quel runtime parle la réponse. Rien de tout cela ne se trouve
# dans le fichier du programme.
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`, `--sbf-slot`, `--sbf-loader` et `--sbf-purpose` sélectionnent le
profil de runtime. Les valeurs par défaut décrivent mainnet-beta tel qu’il est,
sous `loader-v3`, pour un programme déjà déployé. Interroger le déploiement à la
place rapporte les syscalls qui garderaient un programme hors de la chaîne alors
même que la chaîne continuerait de l’exécuter.

`--sbf-version=auto|v0|v1|v2|v3|v4` ne change la sémantique qu’après validation
du layout détecté. Il sert aux fixtures endommagées ou de recherche, pas à
réinterpréter un fichier non fiable sous un autre standard d’empaquetage.

## Analyse et récupération

LowIR conserve encodage de huit octets, champs bruts, continuations LDDW, calls
résolus, hashes syscall, blocs, arêtes, atteignabilité et diagnostics. MedIR
normalise les encodages versionnés en opérations 32/64 bits typées, extensions
explicites, arithmétique gardée, largeurs mémoire et types d’appel. Le dataflow
des registres suit constantes et adresses stack/rodata.

HighIR récupère fonctions entry/internal, arêtes directes, noms syscall officiels,
chaînes, boucles naturelles, conditionnelles réductibles et observations Solana
conservatrices. `sol_invoke_signed_rust`/`sol_invoke_signed_c` sont des CPI ; la
mémoire basée sur le registre d’entrée est un accès account/input. Aucun type
Anchor ni layout account n’est inventé sans IDL.

C et Rust partagent une passe de structuration neutre. Elle émet `if`/`if-else`
et `while`/`loop` lorsque la représentation réductible est unique ; calls internes,
CALLX et flots irréductibles gardent le dispatcher PC exact.

La base syscall couvre logs, mémoire, PDA, SHA-256/Keccak/Blake3, Poseidon,
secp256k1, courbes/alt-bn128, exponentiation modulaire, CPI, return data,
sibling instructions, compute units et sysvars dont epoch rewards. Les
relocations `R_BPF_64_64`, `R_BPF_64_RELATIVE`, `R_BPF_64_32` sont centralisées.
Les text relocations, deux moitiés LDDW et clé CALL Murmur3 officielle, sont
appliquées avant décodage. Si `R_BPF_64_32` a déjà été appliqué et supprimé, la
clé registry est recalculée depuis symboles et slots pour récupérer les calls.

## Récupération du programme Solana

Au-dessus du modèle machine SBF, NeverD rapporte ce qu'un programme signifie en
tant que programme Solana. Chaque fait enregistré porte la preuve qui l'a produit,
et ce que les octets ne tranchent pas reste non renseigné plutôt que deviné.

| Récupéré | Preuve |
|----------|--------|
| Adresses base58 en données en lecture seule | correspondance dans `SBFKnownAddresses.def` et `SBFAnchorNamespaces.def`, ou une constante que le code matérialise |
| L'adresse propre déclarée | un `sol_memcmp_` d'exactement une longueur de clé contre une constante en lecture seule |
| Dispatch d'instructions Anchor | une comparaison 64 bits dont la constante égale un discriminator SHA-256 avec namespace |
| Cibles de CPI | l'enregistrement instruction atteignable depuis l'argument de l'invoke |
| L'opération que sélectionne un appel | un sélecteur répertorié dans `SBFProgramInstructions.def`, ou un discriminator Anchor en tête |
| Seeds d'une adresse dérivée | le tableau de descripteurs de seeds atteignable depuis l'argument de dérivation |
| Lectures et écritures de champs de compte | un load ou store dont l'adresse tombe de façon prouvée dans l'input sérialisé |

Le loader passe un seul argument, le tampon d'input sérialisé à la base de la
région d'input ; la propagation de constantes depuis cet état d'entrée donne donc
des champs de compte nommés plutôt que des offsets bruts. `SBFAccountLayout.def`
contient la sérialisation officielle ; ses champs fixes sont vérifiés comme
pavant exactement leur étendue.

Anchor dérive un discriminator en hachant `<namespace>:<name>` en SHA-256 et en
gardant les huit premiers octets, ce qui est à sens unique. NeverD se contente
donc de confirmer des candidats : `SBFAnchorNames.def` est un dictionnaire de
noms récurrents, et `--sbf-idl` fournit l'IDL du programme, prioritaire. Une
comparaison 64 bits n'est appelée discriminator qu'une fois qu'au moins l'une
d'elles se résout en un nom.

`SBFKnownAddresses.def` recense les adresses de protocole et de programmes
canoniques. Chaque entrée doit se décoder en exactement 32 octets, ce que la
suite de tests impose. La récupération a aussi besoin de l'ABI des syscalls :
SBPFv3 mappe les données en lecture seule à l'adresse zéro, si bien qu'un
argument de longueur et une adresse de données basse sont le même nombre.
`SBFSyscalls.def` enregistre donc quels registres d'argument portent une adresse
VM, et seuls ceux-là sont suivis.

Les deux syscalls d'invocation décrivent la même instruction avec deux
structures différentes, et `SBFCPIABI.def` conserve les deux dispositions,
indexées par le syscall qui les choisit. Lire l'une avec les offsets de l'autre
n'échoue pas : cela rapporte silencieusement le premier compte comme programme
appelé. `SBFProgramInstructions.def` nomme ensuite l'opération demandée à un
programme canonique à partir du sélecteur que son interface publie : un index de
variante bincode pour les programmes system, stake, lookup-table et
upgradeable-loader, un octet de tête pour les programmes de token, y compris la
plage d'extensions de Token-2022 par-dessus la numérotation partagée avec le
programme de token d'origine. Un sélecteur non répertorié est rapporté comme un
nombre.

### Mémoire de travail et fenêtres de syscall

Un programme ne remet presque jamais une constante au runtime. Il assemble un
tableau de seeds, une instruction sérialisée et la charge utile de celle-ci dans
son propre frame ou sur son tas, puis passe un pointeur. Ne lire que l'image
chargée montrerait le pointeur et rien de ce qu'il adresse ; la récupération
maintient donc un modèle au niveau de l'octet de la mémoire que seul ce programme
peut écrire, borné par `kMaxModeledScratchBytes`.

La récupération de scratch est à la demande : le fixed point scratch de Solana
CPI/PDA n’est construit que lorsqu’un vrai `scratch consumer` existe ; les programmes
sans ce consumer ignorent le `whole-CFG fixed point`. `SBFAnalysisLimits.def` décrit
l’`analysis policy` de l’hôte, pas les `protocol limits` : `MaxModeledScratchBytes`
représente 1,024 octets par `program point`, et `ScratchFlowRetainedByteBudget` est
une `logical retained estimate` de 8,388,608 octets. Au dépassement du budget, la
récupération élargit explicitement vers `ScratchRecoveryPrecision::BlockLocal`.
Seuls les `cross-block must-facts` sont abandonnés ; le `block-local replay` reste
`sound` et peut encore récupérer les `same-block stores`. Le printer émet toujours la ligne `recovery scratch-precision=block-local`,
et widening ne renvoie jamais de `half-converged must-facts`.

Deux faits décident de ce qui survit à un appel. `SBFSyscalls.def` dit quels
registres d'argument portent une adresse VM ; `SBFSyscallMemory.def` dit ce que
le runtime en fait, comme lecture ou écriture avec une étendue `Fixed`,
`Counted` ou `Opaque`. Un syscall sans fenêtre d'écriture ne peut modifier aucun
octet de l'appelant : tout ce qui était prouvé avant `sol_log_` l'est encore
après. Une écriture bornée par un argument de longueur n'invalide que cette
fenêtre. Une écriture `Opaque` invalide son adresse de base et tout ce qui la
surplombe, car un tampon ne s'étend jamais en dessous de son début ni au-delà
d'une frontière de région VM. Le résumé d'effets de `SBFSyscalls.def` et la table
de fenêtres sont validés l'un contre l'autre dans les deux sens, si bien
qu'aucun ne peut dériver seul.

`sol_memcpy_`, `sol_memmove_` et `sol_memset_` sont suivis et pas seulement
invalidés : avec une destination, une longueur et une source prouvées, les
octets de destination deviennent connus. C'est ce qui récupère l'opération
qu'invoque un programme Anchor, puisque sa charge utile est copiée en place
plutôt que mappée.

Seul un syscall de runtime résolu peut préserver scratch, et uniquement selon
ses fenêtres d'écriture auditées. Tout appel interne, indirect ou autrement non
résolu efface les octets modélisés, même si aucun argument actuel ne pointe vers
scratch, car un pointeur échappé auparavant ou un alias global peut encore
permettre à l'appelé de les modifier. `sol_invoke_signed_rust` et
`sol_invoke_signed_c` écrivent des données de compte et non la mémoire de
l'appelant, si bien que deux invocations assemblées dans un même bloc restent
toutes deux lisibles.

Le modèle est une analyse « must » avant sur le CFG intra-procédural : un octet
ne survit dans un bloc que si tous les chemins qui y mènent ont écrit la même
valeur. Les arcs d'appel ne sont pas suivis, car un appelé n'hérite de rien du
frame de son appelant. Sa worklist de dépendances n'a aucune échappatoire de
précision liée au nombre de blocs ; un gate Release optionnel exerce la limite
complète de 10 Mio et `1,310,720` instructions.

`SBFLints.def` catalogue des observations sur l'ensemble du programme : absence
de vérification signer ou owner, cible d'invocation non constante, syscall
déprécié ou derrière un feature gate, et version SBPF que SIMD-0500 cessera
d'accepter au déploiement. Chacune porte une sévérité et une confiance, et aucun
lint ne change la sémantique décodée. Rien dans cette couche ne contacte le
réseau.

## Contrat runtime LLVM généré

LLVM ne traite jamais une adresse VM comme pointeur host. Les déclarations
load/store/syscall vérifiées renvoient un status `i32`; load/syscall écrivent
leur `i64` par pointeur de sortie. Tout status non nul branche vers un bloc de
faute SBF. Le module passe `llvm::verifyModule` avant de sortir du backend.

## Contrat host C généré

```c
#include <stdint.h>

typedef enum neverd_sbf_status {
  NEVERD_SBF_OK = 0,
  NEVERD_SBF_INVALID_INSTRUCTION = 1,
  NEVERD_SBF_MEMORY_ACCESS = 2,
  NEVERD_SBF_DIVIDE_BY_ZERO = 3,
  NEVERD_SBF_DIVIDE_OVERFLOW = 4,
  NEVERD_SBF_CALL_DEPTH = 5,
  NEVERD_SBF_UNKNOWN_SYSCALL = 6,
  NEVERD_SBF_UNKNOWN_FUNCTION = 7,
  NEVERD_SBF_EXECUTION_OVERRUN = 8,
} neverd_sbf_status;
/* v2 is fixed-width: values 0..8 reuse the legacy constants above. */
typedef uint32_t neverd_sbf_status_v2;
enum {
  NEVERD_SBF_INVALID_REGISTER = 9,
  NEVERD_SBF_INVALID_BRANCH = 10,
};
typedef uint64_t neverd_sbf_runtime_feature_mask;
typedef struct neverd_sbf_runtime_features {
  neverd_sbf_runtime_feature_mask bits;
} neverd_sbf_runtime_features;

/* Generated feature constants have the form NEVERD_SBF_RUNTIME_FEATURE_<Name>. */
typedef struct neverd_sbf_syscall_invocation {
  uint32_t hash;
  uint64_t arguments[5];
  neverd_sbf_runtime_features runtime_features;
} neverd_sbf_syscall_invocation;

/* v1 is the exact legacy four-field ABI. */
/* All callback fields return int, including the v2 callback. */
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  /* Legacy syscall callback: hash, five arguments, output value. */
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;

/* The v1 entrypoint reads only the four fields above. */
neverd_sbf_status neverd_sbf_program(
    neverd_sbf_environment *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);

/* v2 is a distinct ABI: the old layout is embedded and never extended in place. */
typedef struct neverd_sbf_environment_v2 {
  neverd_sbf_environment base;
  /* NULL callback falls back to base.syscall. */
  int (*syscall_with_features)(
      void *, const neverd_sbf_syscall_invocation *, uint64_t *result);
  /* NULL selects the program snapshot; a pointer to zero is an explicit empty snapshot. */
  const neverd_sbf_runtime_features *runtime_features;
} neverd_sbf_environment_v2;

neverd_sbf_status_v2 neverd_sbf_program_v2(
    neverd_sbf_environment_v2 *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);
```

`width` est exprimé en bits. Chaque callback C généré renvoie `int`, y compris
`syscall_with_features`. Avec l’entrypoint v1 `neverd_sbf_program`, zéro signifie
le succès ; tout retour non nul de `load` ou `store` est normalisé en
`NEVERD_SBF_MEMORY_ACCESS`, et tout retour non nul de `syscall` en
`NEVERD_SBF_UNKNOWN_SYSCALL` ; les contrats sont `v1-load-store-nonzero` et
`v1-syscall-nonzero` ; v1 ne transmet jamais un status exact du callback.
Les fautes internes `InvalidRegister` et `InvalidBranch` sont aussi normalisées en
`NEVERD_SBF_INVALID_INSTRUCTION` (`internal-invalid-instruction`).
L’entrypoint v2 `neverd_sbf_program_v2` est la voie des statuts exacts : une valeur
de callback reconnue de `neverd_sbf_status_v2`, y compris 9 ou 10, est conservée
comme faute traitée (`v2-exact-status`). L’entrypoint v2 conserve également les fautes internes
`InvalidRegister` et `InvalidBranch` comme 9 et 10. Une valeur de callback inconnue
utilise le fallback propre à l’opération produit par le générateur
(`operation-specific-fallback`). Si `syscall_with_features` est nul, il retombe
sur `base.syscall`, et ce callback renvoie lui aussi `int`
(`feature-aware-null-base-syscall`).
Le struct et l’entrypoint v1 restent compatibles avec les hosts legacy. Utilisez
l’entrypoint v2 séparé pour recevoir `syscall_with_features` et le snapshot de
runtime features résolu. Le code généré représente registres, PC de retour,
r6-r9 préservés, frame pointers, adresses VM, fautes de division, opérations PQR
larges et shifts wrapping. Seuls les helpers réellement utilisés sont émis ; la
sortie minimale passe donc `clang -Wall -Wextra -Werror`.

## Contrat host Rust généré

```rust
// The v1 source contract remains Result-based.
pub enum SbfError {
    InvalidInstruction, MemoryAccess, DivideByZero, DivideOverflow,
    CallDepth, UnknownSyscall, UnknownFunction, ExecutionOverrun,
}

#[repr(u32)]
#[non_exhaustive]
pub enum SbfErrorV2 {
    InvalidInstruction = 0, MemoryAccess = 1, DivideByZero = 2,
    DivideOverflow = 3, CallDepth = 4, UnknownSyscall = 5,
    UnknownFunction = 6, ExecutionOverrun = 7, InvalidRegister = 8,
    InvalidBranch = 9,
}

pub struct SbfRuntimeFeatures { bits: u64 }
impl SbfRuntimeFeatures {
    pub const fn from_bits(bits: u64) -> Self { Self { bits } }
    pub const fn bits(self) -> u64 { self.bits }
    pub const fn contains(self, feature: Self) -> bool {
        (self.bits & feature.bits) != 0
    }
}

pub struct SbfSyscallInvocation {
    pub hash: u32,
    pub args: [u64; 5],
    pub runtime_features: SbfRuntimeFeatures,
}

pub enum SbfSyscallOutcomeV2 {
    Unregistered,
    Returned(u64),
    Fault(SbfErrorV2),
}

pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}

pub trait SbfEnvironmentV2 {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfErrorV2>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfErrorV2>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfErrorV2> {
        let _ = (hash, args);
        Err(SbfErrorV2::UnknownSyscall)
    }
    fn syscall_outcome(&mut self, hash: u32, args: [u64; 5])
        -> SbfSyscallOutcomeV2 {
        match self.syscall(hash, args) {
            Ok(value) => SbfSyscallOutcomeV2::Returned(value),
            Err(SbfErrorV2::UnknownSyscall) => SbfSyscallOutcomeV2::Unregistered,
            Err(error) => SbfSyscallOutcomeV2::Fault(error),
        }
    }
    // Some(SbfRuntimeFeatures::from_bits(0)) is an explicit empty snapshot.
    fn runtime_features(&self) -> Option<SbfRuntimeFeatures> { None }
    fn syscall_with_features(
        &mut self, invocation: SbfSyscallInvocation
    ) -> SbfSyscallOutcomeV2 {
        self.syscall_outcome(invocation.hash, invocation.args)
    }
}

pub fn neverd_sbf_program<E: SbfEnvironment>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfError> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated program body")
}
pub fn neverd_sbf_program_v2<E: SbfEnvironmentV2>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfErrorV2> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated v2 program body")
}
```

L’ancien entrypoint `neverd_sbf_program` et `SbfEnvironment` constituent le
`v1-result-abi` ; leurs méthodes host utilisent `Result`. Un
`Some(SbfRuntimeFeatures::from_bits(0))` est le marqueur
`explicit-empty-snapshot`, distinct de `None`. `syscall_outcome` est le
`result-host-bridge` entre la méthode host fondée sur Result et
`SbfSyscallOutcomeV2`. Comme `SbfErrorV2` porte `#[non_exhaustive]`, les appelants
doivent employer un `non-exhaustive-wildcard` (`_`) dans un match.

La sortie est du Rust stable sûr, sans pointeurs bruts. L’entrée est générique
sur le trait et utilise des tableaux sûrs de taille fixe pour registres et
frames. Les tests compilent avec `rustc --edition=2021 -D warnings`.

## API C

Après chargement SBF, les opérations session habituelles restent disponibles :
fonctions, désassemblage, dumps IR, CFG/call graph JSON, sections, symboles,
relocations, chaînes et headers. Rust se choisit par la valeur enum ajoutée sans
casser l’ABI.

```c
neverd_session_t session = neverd_session_create();
neverd_sbf_set_strict(session, 1);
neverd_sbf_set_version(session, "auto");
/* De quel runtime parle la réponse. Les valeurs par défaut décrivent
   mainnet-beta tel qu’il est, sous loader-v3, pour un programme déjà déployé. */
neverd_sbf_set_cluster(session, "devnet");
neverd_sbf_set_slot(session, 474768000);
neverd_sbf_set_loader(session, "loader-v3");
neverd_sbf_set_purpose(session, "deployment");
/* Optional: name Anchor handlers from the program's own IDL. */
neverd_sbf_set_idl(session, idl_json);
const char *rust = neverd_decompile_all_ex(
    session, "program.so", NEVERD_OUTPUT_RUST, 0, 0);
/* consume rust, then: */
neverd_free_string(rust);
neverd_session_destroy(session);
```

## Vérification et limites

`unittests/sbf/` couvre invariants metadata, loaders v0-v4, verifier strict,
CFG/récupération, LLVM vérifié, compilation C/Rust sans warning, interpréteur
brut indépendant de MedIR et API C. Une fixture conditionnelle+boucle s’exécute
dans les deux langages face à l’oracle brut ; le corpus ELF `sbpf` officiel sert
aussi localement sans intégrer de binaires tiers.

- Réécriture SBF et roundtrip objet sont explicitement refusés.
- Récupération Anchor IDL/types et RPC/accounts live sont hors du loader.
- Syscalls et mémoire VM du source généré passent par un host contract ; ce
  n’est pas un runtime Solana autonome.
- Relaxed sert à l’inspection ; aucune sémantique devinée n’est attribuée.

## Référence de conformité actuelle (2026-08-24)

Après les relocations, un `ProgramImage` unique, immuable et adressé par la VM
est la source de vérité commune au decoder, à l’interpreter, à la récupération
des strings et aux backends LLVM/C/Rust. Aucune copie text ou rodata distincte
ne peut diverger de la sémantique du loader.

Les ensembles fermés résident dans `SBFVersions.def`, `SBFOpcodes.def`,
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFVersionFeatures.def`, `SBFProtocolLimits.def`,
`SBFSyscalls.def`, `SBFSyscallMemory.def`, `SBFCPIABI.def`,
`SBFProgramInstructions.def` et
`SBFUpstreamSources.def`. Les diagnostics et noms de blocs LLVM à usage unique
restent locaux, conformément à la pratique réelle de LLVM.

`SBFProtocolLimits.def` consigne l'ancienne valeur de 65 536 instructions et
la limite actuelle de 10 MiB pour les account data ; NeverD dérive de cette
dernière sa borne de décodage conservatrice.

En strict v3/v4, les program headers bornés constituent le contrat runtime ;
les tables de sections et de symboles ne sont qu’un enrichissement debug
facultatif et n’invalident pas une image valide si elles manquent ou sont
endommagées. Legacy v0-v2 fusionne `.text`, `.rodata`, `.data.rel.ro` et
`.eh_frame` ; `R_BPF_64_64`, `R_BPF_64_RELATIVE` et `R_BPF_64_32` sont
appliquées exactement une fois avant que l’image devienne immuable.

| Preuve | Résultat audité |
|--------|-----------------|
| Manifest ELF officiel | 23/23 artefacts de `sbpf/tests/elfs` |
| Oracle officiel | `NeverDSBFExternalOracleTests` confronte 1,411 cas opcode/limite au verifier épinglé |
| Exécution différentielle | oracle raw-byte face à LLVM ORC, C11 et Rust stable, avec traces memory/fault/syscall |
| Agrégat intégré | `check-neverd-sbf` exécute toutes les suites enregistrées ; aucun total rapidement variable n’est figé |
| ASan + UBSan | les cibles ciblées tournent en fail-fast sans rapport ; aucun total rapidement variable n’est figé |

L’audit fixe Anza `sbpf` à
`2510663bb8d894e8e3094be351e4bb4b604f1f84` et Agave à
`ef210d67f2fabeee1730498188fa78854260c679`. Pour le mettre à jour, révisez
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` et
`SBFUpstreamSources.def`, puis exécutez :

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

La comparaison montre que `sol-azy` plante sur l’ELF strict actuel et conserve
un nœud CFG legacy indéfini ; `solana-data-reverser` vise les account data,
`SolDragon` marque l’analyse WIP et `bn-ebpf-solana` requiert Binary Ninja.
Les `sbpf` et Agave officiels restent donc l’autorité sémantique.

## Contrat de preuve audité le 2026-08-24

`SBFUpstreamSources.def` épingle l’audit sur Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679` et le SDK Solana
`122f32e571ce39face4beffaccea733e37c207fd`. Le manifest officiel réussit
23/23 ; `NeverDSBFExternalOracleTests` confronte 1,411 cas opcode/boundary au
verifier officiel indépendant via `SBFOfficialOracleProtocol.def` et
`SBFOfficialVerifierCases.def` et `SBFOfficialExecutionConstants.def`. Les ELF malformés viennent de
`SBFOfficialELFMutations.def` et d’un corpus tabulé ; aucun total rapidement
variable n’est figé.
Séparément, le `différentiel ELF strict de 41 cas` exécute toute la matrice
strict-v3 via le processus officiel `verify-elf-batch` et NeverD ; ces 41 cas
ne font pas partie du total 1,411.

La matrice officielle d’exécution additionnelle (`additional execution matrix`)
est séparée : elle contient exactement 508 cas actifs `(Version,Opcode)` et
58 cas limites, soit 566 cas d’exécution exacts. Elle ne remplace pas et ne
compte pas dans les 1,411 `verifier probes`, ni dans le différentiel ELF strict
ELF stricte de 41 cas.

`NeverDSBFAgaveConformanceTests` authentifie aussi la révision Firedancer
test-vectors `68bb4af40235562e8852fa23d5727e49c2a0b862` et confronte les 1,955
fixtures `sol_compat_elf_loader_v1` (1,399 acceptées, 556 rejetées). Pour chaque
ELF accepté, il compare aussi `entry_pc`, `text_off`, `text_cnt`, `rodata_hash`
et `calldests_hash`. Cette gate ne teste volontairement que le loader, sans
exécuter le verifier d’instructions ultérieur, afin de ne pas confondre les
deux étapes d’Agave.

Le profil chain par défaut reste fidèle à Agave : les lignes
`SBF_RUNTIME_VERSION` calculent par cluster/slot historique l’ISA maximal et le
font passer de V0 à V1, V2 puis V3 à l’activation des feature accounts
officielles ; le maximum actuel reste V3. Il utilise
`RuntimeVersionPolicy::ChainProfile`. Seul `--sbf-version=v4` explicite choisit
`RuntimeVersionPolicy::UpstreamToolchain` pour une analyse offline conforme au
`sbpf` épinglé, sans prétendre que v4 est activé on-chain. La limite actuelle
de 10 MiB vaut exactement `10'485'760` octets ; 65,536 n’est conservé que comme
provenance/test historique et n’est pas appliqué à l’exécution.

Les registres `.def` typés font autorité pour features, syscalls, faults et
source ABI : `SBFSyscallRegistration.def`, `SBFValidationRules.def`, `SBFFaultCodes.def`,
`SBFSourceStatuses.def`, `SBFArgumentRegisters.def` et `SBFEdgeKinds.def`.
`SBFFaultCodes.def` stabilise les valeurs de fault d’exécution, tandis que
`SBFSourceStatuses.def` possède séparément l’ABI du source généré. Le
loader est `raw-first` : correction des relative CALLs, puis raw relocations
une fois dans l’ordre ELF ordinal ; l’ordre d’erreur stable est text identity,
CALL, relocation, entrypoint, read-only layout. Le mapping file/VM est
gap-aware et n’invente jamais d’octets dans les trous.

CFG et dataflow sont par fonction : un call edge n’est pas un predecessor
local, une shared tail reste ambiguë et tous les latches d’une boucle forment
une seule région multi-latch. Worklist et ownership sont testés avec 10,000
fonctions, blocs en ordre inverse et conditional latches, sans chiffre de temps
propre à une machine.

Le call graph SBF public suit `callgraph-budget=fail-closed` : les limites
typées d’input, provenance, node, edge, element et
`CallGraphOutputByteBudget` rendent le JSON exact ou vide. À épuisement, il
renvoie `{"nodes":[],"edges":[]}`, renseigne `neverd_last_error()` et ne publie
jamais de relation partielle.

Chaque ligne d’activation porte cluster, feature account et slot ; un
`RPC activation audit` peut la comparer à un nœud live tout en gardant l’analyse
ordinaire offline. La comparaison inclut Blueshift, `qedsvm` (preuves Lean de
chemins choisis, mais ELF loader limité actuellement à V0),
`leanprover-solanalib`, `sol-azy`, `bn-ebpf-solana` et Ghidra/SolDragon.
`ezBPF` se déclare deprecated au commit
`88829078a6d7682a2baed0d696d500401c263750` et renvoie vers Blueshift ; c’est un
prédécesseur archivé avec une seule table byte-to-enum, pas un decoder sensible
aux versions pour moved-memory, JMP32 et la matrice v0-v4 actuelle. Dans
ce contrôle, les pins de comparaison sont Blueshift
`704e40f7aa82446555b19d9ffbc0a6e18a35480f`, `qedsvm`
`99bd5ede85374adc7fc5c835c2432ecf4e123fd1` et `leanprover-solanalib`
`6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec` ; les quatre outils locaux sont
épinglés à `sol-azy` `362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`,
`solana-data-reverser` `bf90923adec984a61ca0437e9d341360ac1b11ee`, `SolDragon`
`002b98677a5e595a773af6607b77210f5ea71db7` et `bn-ebpf-solana`
`c3fe0de45d37eb68dcb08f2498c6e1f986056572`.
ce snapshot, NeverD possède la preuve reproductible la plus forte que nous
ayons trouvée parmi les décompilateurs SBF généraux publics audités ; c’est une
affirmation comparative bornée, pas un « numéro un mondial » absolu.

L’audit public ajoute `r2ghidra-solana`, épinglé à
`eca0b8e2d307e00991e289b8f9b0f45743819f1b`, avec une UX Ghidra C-like et
`C-like-pdg` pour comptes, Anchor, strings et syscalls ; son CI réussit au HEAD
épinglé, mais la suite Solana spécifique est commentée et le smoke CI ne
décompile que `/bin/ls`. Le reproducer direct confirme que le
`relative_call_sbpfv0.so` officiel de V0 produit un C raisonnable, tandis que le
`relative_call.so` officiel de V3 échoue dans `pdg` ; le résultat est reproductible.
`radare2-solana`, épinglé à
`292d845681be377cadc9959a74c2cadeb6e7f412`, étend SIMD-0173/0174, réservés à V2,
en `>=V2` jusqu’à V3/V4, alors que le `program.rs` officiel les marque V2-only.
`SBPF-3-1`, épinglé à `0e602c93007faa96bccb8e1e12040954ff108b6f`, n’a que 2/2
tests cargo triviaux sans CI ; la détection de version est un placeholder
none/V0, le decoder d’opcode high-nibble est faux et le saut utilise imm au lieu
de off. Les deux ELF relative_call V0/V3 produisent le même pseudocode erroné.
L’avantage de NeverD est l’évidence officielle et reproductible du loader,
verifier, runtime et process-oracle V0–V4, sans nier l’UX ni la sortie C de ces
outils.

`SBFComparisonTools.def` est l’unique autorité pour les noms affichés et les
révisions complètes des outils comparés. Le dernier balayage public borné ajoute
les constats suivants :

- `blastrock/Solana-eBPF-for-Ghidra`, épinglé à
  `c3ad719004726fe924dbed901eca2744ad82c85d`, fournit une vraie UX Ghidra P-code,
  mais un unique modèle SLEIGH sans version fixe CALLX sur `dst` et mélange les
  opcodes legacy/current. Il n’a ni vrais tests ni CI, et sa source par défaut
  omet une classe de constantes de relocation pourtant référencée.
- `SolEmu-Ghidra`, épinglé à `6520af2ff104d5adbec24632ba3afa3bef0da529`,
  hérite de ce decoder identique et ajoute une UX d’émulateur autour de
  comportements CPI, cryptographiques et ZK explicitement simulés ou placeholder ;
  il n’a pas davantage de vrais tests ou CI. `Ghidra_sBPF`, épinglé à
  `907bd4476432ca83bb2352686ad1ccafdb38504c`, permet de choisir v1-v3 à la main,
  mais cumule les encodages réservés à V2 dans V3, sans sélection automatique
  V0/V4, tests ni CI.
- `solana-ebpf-ida-processor`, épinglé à
  `aacd215907266190ed9c6c1b408ca9309f92ecdd`, est une UI IDA utile de
  désassemblage/relocation, pas un source lifter ; sa table mélangée lit toujours
  CALLX depuis `imm` et n’a ni tests ni CI. `solana-bpf-reverse`, épinglé à
  `39479a3bddb8cb866ee499266a76a1b54069b222`, produit des rapports heuristiques
  et des squelettes Rust TODO depuis des layouts hard-coded ; l’exécution donne
  9 pass, 2 fail et 1 skip, sans CI.
- `solens`, épinglé à `22defa1c8f4118dacd42f5c291f1ac31609fc0e5`, est un
  désassembleur terminal V2-only avec 0 test et sans CI. `sbpf-decompiler`,
  épinglé à `37b8bc0edc7ce347abee466f5f974e900c1948df`, n’implémente actuellement
  que trois lignes `Hello, world!`, avec 0 test et sans CI.
- `sbpf-eye`, épinglé à `5277a52aeb58e50b6ff8f9020414334765369b49`, se décrit
  comme une TUI lightweight WIP d’instructions/CFG : 3 tests passent, mais il n’a
  ni IR sémantique, ni source emitter, ni CI. `svm_bytecode_analyzer`, épinglé à
  `12aa236db8964e6be661e38131c2dc81588cf19c`, est un analyseur disassembler/CFG,
  pas un lifter ; son décodage des bytes register/offset est faux et son run
  donne 17 pass et 1 fail, sans CI.
- `giraffexiu/Solana-eBPF-for-Ghidra`, épinglé à
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07`, est un snapshot d’un commit de la
  même lignée Ghidra, sans nouvelle sémantique de version, tests ni CI. `CertSBF`,
  épinglé à `bb93a97cf0c64d119d08ec851e8e820315beb59e`, est une précieuse
  formalisation Isabelle/HOL de l’ancien rBPF, pas un décompilateur source V0-V4
  actuel de programme entier.

Ces constats renforcent seulement la preuve comparative dans le snapshot public
borné ; ils ne concluent rien d’absolu sur les outils futurs ou privés.

L’audit RPC final du 2026-08-24 correspond exactement : 38 feature accounts et
89 activation rows ; mainnet au slot 441305159, testnet à 433055669 et devnet à
487238699. Le compte vide en attente, détenu par le système
(`VirtualAddressSpaceAdjustments` sur mainnet), n’était pas activé. Aucune URL
RPC n’est figée dans la documentation.

La Linux Release CI lit les pins exacts avec `--print-pinned-revision`,
`--print-test-vectors-revision` et `--print-toolchain`, authentifie l’oracle et
le corpus sparse, puis exporte `NEVERD_SBPF_ORACLE` et
`NEVERD_AGAVE_CONFORMANCE_ROOT` ; les deux tests externes y sont obligatoires.
Un run local normal sans environnement oracle/corpus explicite découvre les
cas mais peut les ignorer.
