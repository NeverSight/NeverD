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
compte dont l’existence l’active et le slot auquel chaque cluster l’a activée.
Une gate sans ligne pour un cluster n’y a pas été activée. `simd-0321` est active
sur tous les clusters ; `simd-0449` et le syscall SHA-512 le sont sur testnet et
devnet et pas sur mainnet, ce qui est exactement pourquoi un programme qui marche
sur devnet échoue sur mainnet.

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
clusters d’un coup. `sol_alloc_free_` n’a besoin d’aucune gate : le runtime
continue de l’honorer et refuse d’accepter un nouveau programme qui l’appelle, ce
qui est une différence entre les deux registries et rien d’autre.

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
| Adresses base58 en données en lecture seule | correspondance dans `SBFKnownAddresses.def`, ou une constante que le code matérialise |
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

Un appel vers une fonction que cette analyse n'a pas décrite est supposé écrire
partout où il peut atteindre. L'appelé s'exécute dans son propre frame : un
appel dont les registres d'argument n'adressent prouvablement pas la mémoire de
travail laisse le modèle intact ; tout le reste l'abandonne.
`sol_invoke_signed_rust` et `sol_invoke_signed_c` écrivent des données de compte
et non la mémoire de l'appelant, si bien que deux invocations assemblées dans un
même bloc restent toutes deux lisibles.

Le modèle est une analyse « must » avant sur le CFG intra-procédural : un octet
ne survit dans un bloc que si tous les chemins qui y mènent ont écrit la même
valeur. Les arcs d'appel ne sont pas suivis, car un appelé n'hérite de rien du
frame de son appelant. Les programmes de plus de `kMaxScratchFlowBlocks` blocs
conservent la récupération par bloc et ne perdent que les faits qui franchissent
une frontière de bloc.

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
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;
```

`width` est en bits ; un retour host non nul devient un status SBF explicite.
Registres, PC de retour, r6-r9 préservés, frame pointer, adresses VM, fautes de
division, PQR large et shifts wrapping sont représentés. Seuls les helpers
utilisés sont émis, donc `clang -Wall -Wextra -Werror` accepte la sortie.

## Contrat host Rust généré

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

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

## Référence de conformité actuelle (2026-08-10)

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
| Manifest ELF officiel | 20/20 artefacts de `sbpf/tests/elfs` |
| Matrice ISA | les 256 encodings pour v0-v4, soit 1,280 cellules, plus les limites du verifier |
| Exécution différentielle | oracle raw-byte face à LLVM ORC, C11 et Rust stable, avec traces memory/fault/syscall |
| Agrégat intégré | 145/145 cas dans 14 binaires de test |
| ASan + UBSan | 141/141 cas core dans 13 binaires sans rapport |

L’audit fixe Anza `sbpf` à
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` et Agave à
`cae40aa610fdbdb313209bc1eec737079eb59688`. Pour le mettre à jour, révisez
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` et
`SBFUpstreamSources.def`, puis exécutez :

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

La comparaison montre que `sol-azy` plante sur l’ELF strict actuel et conserve
un nœud CFG legacy indéfini ; `solana-data-reverser` vise les account data,
`SolDragon` marque l’analyse WIP et `bn-ebpf-solana` requiert Binary Ninja.
Les `sbpf` et Agave officiels restent donc l’autorité sémantique.
