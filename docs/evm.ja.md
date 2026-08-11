**言語**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM 逆コンパイル

[← ドキュメント索引](README.ja.md)

NeverD は従来形式の Ethereum Virtual Machine バイトコードを読み込み、専用の
256-bit LowIR、stack-SSA MedIR、復元 HighIR を構築し、LLVM IR、C23、Solidity を
出力します。strict 解析が既定であり、未割り当て opcode や選択した hardfork で
無効な opcode は、その正確な PC でエラーになります。

Solidity と C は意味論的な再構成です。opcode 順序、256-bit 演算、stack 検査、
検証済み制御フローを保持しますが、元のソース、識別子、型の再現は主張しません。

## クイックスタート

```bash
# i256/i512 を使う検証済み LLVM IR。
./build/bin/neverd lift contract.evm -o contract.ll

# EVM の各解析段階を確認。
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# C23 または Solidity を出力。
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# 歴史的 opcode 集合を選択し、未知 opcode を調査用 fault node として保持。
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`、`cfg`、C API の Low/Med/High/LLVM query も EVM 入力を受け付けます。
EVM の binary rewrite は明示的に拒否され、`patch` は native binary 専用です。

## 対応入力

| 入力 | 認識と正規化 |
|------|--------------|
| raw bytes | `.raw`、`.evmraw`、または明示的 EVM 拡張子を持つ binary content |
| hex text | 任意の ASCII 空白と省略可能な `0x`。`.evm`、`.hex`、`.bin`、`.bytecode` および検証済み拡張子なし hex |
| compiler artifact | root または `evm` 配下に `deployedBytecode`、`runtimeBytecode`、`bytecode` を持つ `.json`。`contracts → file → contract → evm` の solc standard JSON も対応 |

runtime/deployed bytecode を creation bytecode より優先します。creation code しかない
場合は、有界かつ定数の `CODECOPY`/`RETURN` constructor wrapper を認識して runtime
slice を抽出します。`0x` だけの artifact field は空とみなし、空の runtime field が
利用可能な creation fallback を隠すことはありません。末尾の Solidity CBOR map は、
encoded length、map marker、既知の `solc`/`ipfs`/Swarm key がすべて妥当な場合だけ
除去します。

不正 hex、奇数桁、未解決 linker placeholder、曖昧な multi-contract artifact、
不正 metadata bounds、正規化後の空 code は実用的なエラーになります。C++ API の
`BytecodeLoadOptions::ArtifactContract` は `Contract` または
`path/File.sol:Contract` を選択できます。同名 contract が複数 source file にある
場合は未修飾名を拒否し、artifact 順序による誤選択を防ぎます。

EVM は backend plugin ではなく core loader registry に登録されます。そのため CLI、
C API、disassembler、CFG builder、Low/Med/High/LLVM query は同じ正規化 image と
EVM options を受け取り、entry point ごとに認識や解析がずれません。

## Hardfork と opcode

Frontier から Fusaka までの 150 個の割り当て済み legacy opcode を網羅し、
`PUSH0`、transient storage、`MCOPY`、blob opcode、`CLZ` を含みます。既定の
`latest` は Fusaka です。

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

`dao`、`tangerine_whistle` のような underscore 形式、`merge`、`prague`、`osaka`
も受け付けます。現在 `latest` と `osaka` は canonical `fusaka` revision です。

`latest` は NeverD が実装した最新の確定 mainnet revision であり、Ethereum 開発
branch の先端ではありません。[Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)
は 2026 Q4 予定で、Review 段階の
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) と
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) は
`--evm-hardfork=amsterdam`（または `bogota`）でのみ有効になり、確定までは
`latest` に入りません。EIP-8024 は有効な immediate だけを消費し、無効な候補は
次の instruction のままです。

EOF は [Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2) で
除外され、execution-spec-tests でも
[Osaka から削除され未予定](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md)
と記録されています。NeverD は撤回済み proposal を確定 mainnet behavior としません。

strict mode は unknown byte と fork-inactive byte を拒否します。`--evm-relaxed` は
LowIR/diagnostic に保持しますが、実行が到達すれば backend は fault します。未知 byte
を NOP として黙って扱うことはありません。

## LLVM 形式の metadata architecture

手書き EVM metadata は LLVM の multiply-included `.def` pattern に従います。

- `EVMOpcodes.def` が 150 個の確定 opcode と 4 個の opt-in 開発 opcode の唯一の
  source of truth です。encoding、実際の pop/push 変化、immediate kind、class、
  activation fork、primary effect、独立した
  EVM memory/state/call-value access、termination を 1 record に収め、暗黙 default を
  許しません。
- `EVMMemoryAccesses.def`、`EVMStateAccesses.def`、
  `EVMCallValueAccesses.def` は閉じた typed domain です。`CALL` は external call と
  memory read/write、`EXTCODECOPY` は context read と memory write を同時に持てます。
  state access は `None/Read/Write/Unknown` lattice です。payability は独立しており、
  `CALLVALUE` read は通常 `payable` を導きます。一方、解析器は canonical
  `ISZERO(CALLVALUE)` guard と非 zero branch の `REVERT` を証明した場合だけ、その
  compiler-generated read を除外します。
- `EVMImmediateKinds.def` は fixed-width PUSH data と EIP-8024 の conditional
  single/pair encoding を定義し、`EVMDecodeStatuses.def` は LowIR と disassembly が
  公開する stable vocabulary を所有します。`EVMUpstreamOpcodePolicy.def` は
  go-ethereum の naming alias と意図的な historical/withdrawn exclusion を記録し、
  `scripts/audit_evm_opcode_metadata.py` は byte drift と未 review の新しい upstream
  constant を拒否します。
- `EVMHardforks.def`、`EVMEffects.def`、`EVMExitStatuses.def`、
  `OutputLanguages.def` が ordered enum、parser、display name、CLI choice、C ABI value
  を生成します。`EVMConstants.h` は protocol width/limit/default name を所有します。
- `EVMCalls.def` は別プログラムを呼び出す 4 命令と、callee アドレスの出所を表す
  lattice を記述します。レコードごとの 1 つのフラグ、すなわち callee と引数
  window の間に value operand があるかどうかが以降の operand 位置をすべて導出し、
  その導出が宣言された pop 数からずれないよう opcode データベースに対して検証され
  ます。
- `EVMPrecompiles.def` はプロトコル自身が応答するアドレスの辞書で、各エントリは
  それを予約した fork を持ちます。gas は意図的に含めません。precompile のコストは
  入力の関数であり、アドレスや操作を変えないまま何度も再価格付けされてきたためで
  す。
- `EVMRecoveredFacts.def` は復元事実の語彙の綴りを所有します。出力に現れる名前が
  1 箇所に集まり、新しい列挙子が漏れうる `switch` に散らばりません。
  `EVMKnownSignatures.def` は signature が持つ 3 つの役割について同じことを行いま
  す。
- `Semantics.h` は target-independent scalar ALU evaluator です。constant folding と
  interpreter が同じ checked `APInt` を使い、LLVM/C/Solidity lowering は target
  contract と unsupported case を明示するため独立に fail-loud です。

decoder が raw-byte boundary です。assigned identity と fork activation を分離する
ため、relaxed decode は inactive opcode の name/fork/immediate width を保持しつつ
semantic query を conservative fault にします。inactive immediate が後続 boundary を
ずらしません。解析、実行、emitter は generated `Opcode` と metadata query を使い、
raw encoding は trace/host callback の ABI boundary に限ります。`SWAP16` の 17 stack
input と最大 host op の 7 argument は別々に compile-time 導出されます。

`OpcodeInfo` は half-valid に default construct できず、name は dangling しない
`llvm::StringLiteral` です。compile-time validator は duplicate encoding、unknown
property、ALU contract、effect/state mismatch、PUSH/DUP/SWAP/LOG family、terminator、
host ABI result count を検査します。unknown stack family は lowering ができるまで
拒否し、relaxed unknown metadata は専用 factory だけが生成します。

`.def` は LLVM の
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def)
と同様の hand-authored database です。`.inc` は TableGen output など実際の生成物や
literal fragment 用です。豊富な declarative record は `.td` から
[TableGen](https://llvm.org/docs/TableGen/ProgRef.html) が `.inc` を生成します。
NeverD には現時点で EVM TableGen step がないため、generator のない generated-looking
`.inc` は採用しません。周辺 C++ は LLVM [coding standards](https://llvm.org/docs/CodingStandards.html)
と LLVM ADT/string types、exhaustive fail-loud switch に従います。

opcode の追加は完全な `EVM_OPCODE` record、共有 scalar semantics、明示 backend
lowering、focused test の順です。hardfork は ordered `EVM_HARDFORK` record と alias
を追加します。typed API、lookup、validation、classification、CLI は重複 table なしで
拡張され、semantic case の不足は即座に失敗します。

## 解析モデル

- **EVM LowIR**: PC、encoding、typed immediate status、decode 済み stack-depth operand
  （PUSH の right-zero padding と EIP-8024 の conditional-consumption rule を含む）、
  basic block、predecessor/successor edge、validated `JUMPDEST` target、reachability、
  stack-height domain を保持します。CFG recovery は deterministic whole-program fixed
  point です。各 stack slot に bounded finite set of 256-bit values を伝播し、具体的な
  height ごとに abstract stack を 1 つ保持します。internal-call/return block をまたぐ
  constant、stack shuffle、`PC`/`CODESIZE`、scalar ALU operation により、1 つまたは
  複数の concrete jump target を解決できます。本当に不明な target は推測せず、
  explicit indirect edge のままです。

  `AnalyzeOptions::MaxAbstractValuesPerSlot` は各 finite value set を制限し、超過時は
  slot を `Unknown` に widen します。`MaxStackHeightVariants` は block の
  path-dependent height 数を制限し、CFG を切り詰める代わりに explicit
  analysis-limit error を返します。どちらも zero を拒否します。non-relational stack
  merge 後の Cartesian operation で作られた finite value は over-approximation として
  mark されます。invalid candidate は診断しますが、slot correlation が失われただけで
  strict analysis が bytecode を拒否することはありません。precise invalid target は
  exact jump PC で引き続き失敗します。relaxed mode の stack fault は診断され、fault
  した abstract path だけを終了します。存在しない post-fault fallthrough は作りません。
- **EVM MedIR**: 各 stack value を 256-bit SSA value として表し、すべての merge phi を
  配線してから deterministic sparse constant worklist を実行します。private lattice は
  `Uninitialized`、1 つの exact `Constant`、`Overdefined` です。同じ constant は block
  と anchored phi cycle を越えて伝播し、conflict する cycle や runtime-dependent cycle
  が constant を捏造することはありません。worklist は def-use ID を検査し、interpreter
  と同じ `Semantics.h` ALU evaluator を使用します。MedIR は primary semantic effect に
  加え、orthogonal な `none/read/write/readwrite` EVM-memory access、source-level state
  access、call-value access も保持します。この境界では polymorphic LowIR stack を
  top-align して保守的に扱い、一部の incoming height に存在しない slot は explicit
  unknown value となり、deterministic diagnostic が精度低下を記録します。
- **EVM HighIR**: Solidity dispatcher selector、推定 calldata/return word、mutability、
  constant storage slot、LOG/event と revert の fact、function/CFG region を復元します。
  checked producer index と iterative memoized value walk は instruction distance ではなく
  typed MedIR operand から fact を復元します。selector comparison は block と phi を
  またぎ、`EQ` のどちらの operand order も扱い、derived 32-bit mask を保持できます。
  argument offset、storage key、event topic0、non-payable/receive guard、exact 32-byte
  return size は semantic input を使います。iterative walk は MedIR graph により構造的に
  bounded で、malformed、mixed、cyclic expression は unknown とします。同一 selector の
  conflicting target は診断して省略します。payability は state-access lattice と独立で、
  reachable unresolved dynamic jump は保守的な `nonpayable` recovery を強制します。
  MedIR に memory SSA が入るまでは custom-error payload recovery と外向き call の
  payload recovery だけが残る bounded instruction-window heuristic です。復元した name と type は明示的に heuristic です。

  HighIR は interface の外向き半分も記録します。すなわち `CALL`、`CALLCODE`、
  `DELEGATECALL`、`STATICCALL` のそれぞれについて、callee の出所、解析対象の fork
  が予約していればその予約アドレス、呼び出しが callee の calldata 先頭に置く
  selector、そして定数であれば転送 value を記録します。`CREATE` と `CREATE2` は
  まだアドレスを持たないコードを実行するため、復元すべき callee が存在せず対象外
  です。

  復元した外向き signature が、そのプログラム自身が応答する標準に数えられること
  はありません。`transfer(address,uint256)` を送ることはトークンを使うという意味
  であってトークンであるという意味ではなく、両者を混同すればあらゆる router と
  vault が ERC-20 として報告されてしまいます。delegate する call はさらに proxy
  fact としても報告されます。callee のコードがこのプログラム自身の storage に対し
  て動くのは、この family でそれだけだからです。

  precompile の検索は、存在する最新の fork ではなく解析対象の fork で gate されま
  す。後の fork が導入する precompile のアドレスを呼び出してもコードのないアカウ
  ントに届き、成功して何も返しません。名前を付ければ、プログラムが明らかに行って
  いない操作を報告することになります。
- **LLVM**: verifier-clean `i32 @evm_execute(ptr)` state machine、checked 1024-word
  `i256` stack、`i512` intermediate、guarded signed division、saturated shifts、正確な
  `BYTE`/`SIGNEXTEND`/`CLZ`、validated dynamic-jump switch。

deterministic interpreter が semantic oracle です。LLVM/C を実行して比較し、Solidity
は Anvil に deploy して storage/trace を比較します。pre-Fusaka raw corpus も Anvil の
native EVM で実行し、scalar ALU、calldata copy、overlapping `MCOPY`、memory expansion、
Keccak、return data を独立検証します。

account operand は[実行仕様](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py)
どおり 160-bit address に mask し、environment/map width は実行前に検証します。
`BLOCKHASH` は直前 256 block window を適用します。EIP-211 return-data buffer は frame
output と分離され、`RETURN`/`REVERT` だけが `ExecutionResult::ReturnData` を設定します。
CREATE/CREATE2 failure は zero と revert buffer、success は address と空 buffer です。

## 生成 C の contract

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

環境依存 operation は次の host ABI を呼びます。`a0` は元の stack top、未使用 argument
は zero、戻り値は最初の pushed value です。trace は各 instruction の前に走ります。

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment, uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);
void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

```bash
clang -std=c2x -ffreestanding -c contract.c
```

frontend は 512-bit `_BitInt` が必要です。Apple Darwin Clang は現在上限不足なので、
macOS では対応する non-Darwin target または NeverD の LLVM output を使用します。

## 生成 Solidity の contract

出力は、監査向けの selector-specific function/storage/event/error と、正確な PC/stack
state machine を併記します。constant storage は
`recovered_storage_slot_3 = uint256(0x3)` のような absolute slot constant であり、
偽の sequential state layout は作りません。

contract は意図的に `abstract` です。`_evmHost` を override して environment effect
を実装し、`_evmTrace` は既定で `EVMTrace` を emit します。

```bash
solc --bin contract.sol
```

## C API

```c
neverd_session_t session = neverd_session_create();
neverd_evm_set_hardfork(session, "cancun");
neverd_evm_set_strict(session, 1);
if (!neverd_session_load(session, "contract.evm") ||
    !neverd_session_analyze(session)) {
  /* inspect neverd_last_error(session) */
}
const char *solidity = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_SOLIDITY, 0, 0);
const char *c = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_C, 0, 0);
neverd_free_string(solidity);
neverd_free_string(c);
neverd_session_destroy(session);
```

`neverd_decompile_all` は互換性のため C を出力します。新しい entry point は
`neverd_session_bitness`、`neverd_evm_set_strict`、`neverd_evm_set_hardfork`、
`neverd_decompile_all_ex` です。native に Solidity、EVM に legacy LLVM-to-C flag、
EVM に native object roundtrip を要求すると、黙って無視せず明示的に拒否します。

## 明示的な制限

- legacy bytecode のみ。EOF container は未対応です。
- Amsterdam/Bogota は明示的な開発 target です。予定 opcode が確定するまで
  `latest` は確定済み Fusaka のままです。
- RPC、chain-state discovery、gas/refund、precompile execution は行いません。
- creation extraction は一般的な static wrapper に限り、constructor emulator ではありません。
- dynamic jump は bounded constant analysis で証明できない限り indirect edge のままです。
- ABI type、source name、mapping、event、custom error は best-effort recovery です。
- memory/storage/calldata/call/log/hash/context を独立実行するには host hook が必要です。
