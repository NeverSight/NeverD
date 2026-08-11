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

Les changements v2 ne s’appliquent volontairement pas à v3. Les feature checks
sont explicites, jamais des suppositions `version >= N`. Strict, par défaut,
rejette headers, plages ou alignements mal formés, sections legacy writable non
supportées, continuations, registres, écritures frame-pointer ou branches
invalides et opcodes inactifs, avec slot et adresse virtuelle.

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
```

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
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFProtocolLimits.def`,
`SBFSyscalls.def` et
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
| Agrégat intégré | 104/104 cas dans 13 binaires de test |
| ASan + UBSan | 101/101 cas core dans 12 binaires sans rapport |

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
