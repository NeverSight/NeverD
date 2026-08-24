**言語**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM 逆コンパイル

[← ドキュメント索引](README.ja.md)

NeverD は従来形式の Ethereum Virtual Machine バイトコードを読み込み、専用の
256-bit LowIR、stack-SSA MedIR、復元 HighIR を構築し、LLVM IR、C23、Solidity を
出力します。strict 解析が既定ですが、legacy EVM は image 全体を事前検証しません。
確実に `Reachable` な execution lane が未割り当てまたは選択 hardfork で inactive な
opcode に実際に到達したときだけ、その正確な PC で拒否します。dead byte と単なる
`MayReachable` CFG candidate は strict error になりません。

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
slice を抽出します。この constructor の走査は、解析対象の hardfork のもとで実際の
decoder と同じ single-instruction decoder を使うため、ある fork では data であり別の
fork では opcode である byte が境界を動かすことはありません。存在する
`deployedBytecode` または `runtimeBytecode` field は authoritative です。明示的な
`0x` は空の自然停止 runtime として受理され、creation bytecode への fallback を意図的に
防ぎます。field が欠けている場合だけ次の candidate を探し、明示 prefix のない欠落または
空白だけの hex は拒否します。明示 raw 入力も空を受理します。

### コンパイラの trailer

`EVMMetadataFields.def` は二つの trailer 形式を表にします。Solidity は CBOR map を
書き、その末尾 2 byte は map だけの長さを数えます。`vyper` はその map で終わる CBOR
array を書き、その末尾 2 byte は自分自身を含む footer 全体を数えます。一方の framing
を他方として読んでも派手に失敗はしません。2 byte ずれた位置に着地し、実際のコードを
2 byte 削るだけです。そのため両方を試し、どちらにも一致しない入力はそのまま残します。

trailer は 2 回読みます。与えられたままの入力に対して 1 回、deploy wrapper を外した
後に残る runtime code に対して 1 回です。Vyper は trailer を initcode 側へ移し、
runtime code には残しません。そのため unwrap 後だけを見る読み手は、自ら名乗っている
contract を build 不明と報告してしまいます。sequence footer は runtime code の長さ、
data section の長さ、immutable の長さも示し、constructor を実行せずに返されるコードを
限界づけます。

### 命令ではない container

`EVMBytecodeContainers.def` は、いかなるデコードよりも前に入力を分類します。
EIP-3541 が `0xEF` を deploy 不可にして以来、先頭の `0xEF` はその byte 列が命令では
ないことの表明です。

| Container | Marker | 扱い |
|-----------|--------|------|
| legacy | — | 命令としてデコード |
| delegation (`eip-7702`) | `0xef0100` かつちょうど 23 byte | 委譲先アカウントを報告し、解析は停止 |
| eof (`eip-3540`) | `0xef00` | 拒否。有効化した fork は存在しない |

delegation indicator の 20 byte はアドレスであってコードではありません。デコードすれば
アドレスを opcode として読み、アカウントの control-flow graph を作ってしまうため、
`info` は委譲先を報告し、解析は理由を添えて拒否します。この拒否は二つの場合を区別
します。Pectra より前では marker がまだ割り当てられておらず、Pectra 以降では委譲先の
runtime code が単に存在しません。長さが異なる marker は container の変種ではなく
malformed な入力なので、decoder が読めなかった byte を名指しできるよう命令のままに
します。

不正 hex、奇数桁、未解決 linker placeholder、曖昧な multi-contract artifact、
不正 metadata bounds、欠落または空白だけの hex は実用的なエラーになります。明示的な
空 raw 入力または `0x` runtime は有効な空 program です。C++ API の
`BytecodeLoadOptions::ArtifactContract` は `Contract` または
`path/File.sol:Contract` を選択できます。同名 contract が複数 source file にある
場合は未修飾名を拒否し、artifact 順序による誤選択を防ぎます。

EVM は backend plugin ではなく core loader registry に登録されます。そのため CLI、
C API、disassembler、CFG builder、Low/Med/High/LLVM query は同じ正規化 image と
EVM options を受け取り、entry point ごとに認識や解析がずれません。

## Hardfork と opcode

Frontier から Fusaka までの割り当て済み legacy opcode をすべて網羅し、
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
除外されました。EOFv1/EIP-7692 は未予定で、container proposal
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540) は Stagnant です。旧
`execution-spec-tests` repository は archive され、保守中の tests は
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests) に移りました。
NeverD は experimental EOF container を確定 mainnet behavior としません。

strict mode は、確実に `Reachable` な state lane が実行到達を証明した unknown または
fork-inactive byte だけを拒否します。`--evm-relaxed` は typed fault prefix と diagnostic
として保持し、backend は実行到達時に fault します。unknown byte を NOP として扱いません。

## LLVM 形式の metadata architecture

手書き EVM metadata は LLVM の multiply-included `.def` pattern に従います。

- `EVMOpcodes.def` が確定済み legacy opcode と opt-in 開発 opcode すべての唯一の
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
  go-ethereum の naming alias と意図的な historical/unscheduled-EOF exclusion を記録し、
  `scripts/audit_evm_opcode_metadata.py` は byte drift と未 review の新しい upstream
  constant を拒否します。
- `EVMHardforks.def`、`EVMEffects.def`、`EVMExitStatuses.def`、
  `OutputLanguages.def` が ordered enum、parser、display name、CLI choice、C ABI value
  を生成します。`EVMAnalysisLimits.def`、`EVMInterpreterLimits.def`、
  `EVMABIParserLimits.def`、`EVMABITableLimits.def` は analysis、interpreter、parser、
  public table の各上限を宣言します。`EVMConstants.h` は shared protocol width と stable
  internal name を所有し、`EVMAnalysisLimits.def` から analysis default と diagnostic
  option name を生成します。interpreter/ABI header は自身の table から制限を生成します。
- `EVMCalls.def` は別プログラムを呼び出す 4 命令と、callee アドレスの出所を表す
  lattice を記述します。レコードごとの 1 つのフラグ、すなわち callee と引数
  window の間に value operand があるかどうかが以降の operand 位置をすべて導出し、
  その導出が宣言された pop 数からずれないよう opcode データベースに対して検証され
  ます。
- `EVMPrecompiles.def` はプロトコル自身が応答するアドレスの辞書で、各エントリは
  それを予約した fork と、それを予定に載せた提案を持ちます。`0x100` の
  `P256VERIFY` は `eip-7951` に帰属します。これは Fusaka とともに mainnet へ
  予約した Final 提案であり、そのインターフェースの出どころである rollup 側の提案
  は一度も予定に載せていません。gas は意図的に含めません。precompile のコストは
  入力の関数であり、アドレスや操作を変えないまま何度も再価格付けされてきたためで
  す。
- `EVMMetadataFields.def` と `EVMBytecodeContainers.def` は、デコード前の入力が
  何であるかを記述します。すなわち compiler trailer の二つの framing と、byte 列が
  そもそも命令ではない container です。
- `EVMRecoveredFacts.def` は復元事実の語彙の綴りを所有します。出力に現れる名前が
  1 箇所に集まり、新しい列挙子が漏れうる `switch` に散らばりません。
  `EVMKnownSignatures.def` は canonical function spelling と selector を一度だけ保存し、
  standard ごとの `KnownFunctionVariantInfo` に return list と
  independent/non-independent evidence role を分離します。ERC-20/ERC-721 の shared
  spelling は 1 つの callable candidate のままですが、どちらの standard も単独では証明せず、
  最初の variant の return type も借用しません。event と custom error は別の typed record
  を保ちます。
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

  back-edge で変化した loop-carried slot は、fixed point の収束のため semantic に `Top`
  へ over-approximate されます。この loop recurrence abstraction は resource budget と
  独立です。instruction、block、state、value node、abstract stack、lane、edge、worklist
  update、instruction×lane transfer は named budget で課金され、
  `MaxAbstractValuesPerSlot`、`MaxStackHeightVariants`、
  `MaxAbstractInstructionTransfers` を含みます。zero または exhaustion は insertion 前の
  hard error です。追加の emergency widening や silent truncation は
  起こりません。exact invalid target は jump PC で失敗します。

  `EVMLowFaultKinds.def::InvalidJumpDestination` は `end-of-code JUMPI` で path-sensitive
  です。condition が確実に true で target が invalid なら successful tail はなく definite
  fault を記録し、確実に false なら成功します。unknown condition は成功し得る false path
  だけを残し、lane 全体を definite fault と誤認しません。
- **EVM MedIR**: 各 stack value を 256-bit SSA value として表し、すべての merge phi を
  配線してから deterministic sparse constant worklist を実行します。private lattice は
  `Uninitialized`、1 つの exact `Constant`、`Overdefined` です。同じ constant は block
  と anchored phi cycle を越えて伝播し、conflict する cycle や runtime-dependent cycle
  が constant を捏造することはありません。worklist は def-use ID を検査し、value、
  state lane、stack entry、operation、operation-lane reference、phi incoming、worklist
  update を別々の budget で制限します。interpreter と同じ `Semantics.h` ALU evaluator を使用します。MedIR は primary semantic effect に
  加え、orthogonal な `none/read/write/readwrite` EVM-memory access、source-level state
  access、call-value access も保持します。各 LowIR whole-stack lane に独立した SSA
  execution lane を対応させ、phi は source lane を明示します。互換性のない stack を
  maximum height で top-align しません。
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
  byte-granular で flow-sensitive な memory dataflow が block をまたぐ constant-offset
  write を追跡し、overlap/kill で byte を合成し、dynamic/unknown write で知識を無効化
  します。現在証明済みの payload recovery は selector と既知の Panic byte です。既知の
  custom-error declaration について Solidity emitter は canonical parameter type を保持
  しますが、各 runtime argument value の復元を主張しません。

  selector discovery は root lane からだけ始まり、dispatcher の unmatched edge をたどります。
  handler 内の selector-like test は public function に昇格しません。receive/fallback も
  root-constrained で、確実に reachable な successful terminal が必要です。revert、fault、
  non-payable empty-calldata handler、単なる possible path は entry point を証明しません。
  canonical function candidate は矛盾する calldata use で棄却され、shared selector は
  independent standard evidence を加算しません。設定数の独立 compatible selector、または
  exact event topic/arity、storage slot、proxy の強い evidence が得られた後だけ standard と
  per-standard variant を選択します。その static return list も、確実に reachable なすべての
  successful terminal が exact ABI byte count で一致するときだけ出力します。unresolved
  transfer、conflicting shape、mismatch は fail closed となり、revert/fault は successful
  return ではありません。name、type、event、standard label は evidence-backed candidate です。

  HighIR は function、lane/operation visit、region block reference、memory read request、
  tracked byte、memory state cell、memory worklist update を別々の hostile-input budget で
  制限します。memory fixed point は確実に reachable で実行される lane だけを使い、前駆間を
  byte consensus で meet します。budget exhaustion は fact を切り捨てず hard error です。

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

interpreter は opcode 固有の side effect より前に、typed required stack height、pop count、
retained-plus-pushed height を preflight します。そのため underflow/overflow で命令が半分だけ
実行されません。`EVMForkSemantics.def` は byte `0x44` を Paris 前の `DIFFICULTY`、Paris
以降の `PREVRANDAO` に切り替えます。`REVERT`、semantic fault、step limit、
allocation/length resource exhaustion は storage、transient storage、log、selfdestruct effect
を entry snapshot に戻し、frame-local diagnostic と明示 revert byte は保持します。allocation
failure は error string の追加 allocation をせず `ExecutionFaultKind::ResourceExhausted` と
なります。entry snapshot 自体を作れなければ `HasPersistentStateSnapshot` は false で、その
結果は commit できません。

### Public IR と resource boundary

public `execute` は最初に
`Code`/`Fork`/`Instructions`/`JumpDestinations` が canonical LowIR を構成することを検証します。
fork、instruction record、encoding、jump-destination table の改ざんは、interpreter が
instruction table を index する前に `llvm::Error` になります。public `lowerToMedIR` も、
設定された option、resource bound、structural invariant をこの順に検証してから、
`canonical decode replay` により埋め込まれた fork/strictness で `Low.Code` を decode し、
LowIR の各 field を比較します。その後でのみ `lowerCanonicalLowToMedIR` を呼び、index を構築し、
caller-controlled record に比例する出力を allocation できます。public `recoverHighIR` も外部
LowIR/MedIR を replay 検証します。private `lowerCanonicalLowToMedIR` と
`recoverCanonicalHighIR` は `analyze` が所有する IR 専用で、重複した非再帰 replay だけを
省略しますが、HighIR option/resource budget は常に適用されます。

dispatcher proof は `MedStateLane` ごとに sort 済み `Any/Exact/Excluded` selector domain を
保持します。join は Exact set を union、Excluded exclusion set を intersect し、cofinite
exclusion から Exact set を差し引きます。domain が widen すると lane を再訪します。equality
は selector が許可されている場合だけ true-edge candidate を記録し、false edge では除外します。
raw `XOR(selector, constant)` は、canonical successor がすべて同じ entry を指すとき、
zero/false edge を match として記録します。この fallthrough form は `JUMPDEST` target を
必要としません。nonzero/true mismatch edge は selector を除外し、`ISZERO` は同じ expression
を equality に変換します。selector word、zero-calldata word、calldata size、call value guard
は edge ごとに refine され、unknown conditional は possible branch を探索せず proof を停止します。

function を認識した後、function-scope traversal はその candidate の
`exact singleton selector` を保持して続行します。shared dispatcher へ jump で戻った場合、
`SelectorEquality`、raw `XOR`、`SelectorWord` は、すでに match した selector と一致する
`definite edge` だけをたどります。一方、Unknown または無関係な predicate では、すべての
`definite edges` を保守的に保持します。他の entry block を除外する heuristic は使用しません。
これにより正当な `shared body/tail-call` が失われません。

外部 CALL/CREATE の結果は別です。host outcome 自体が非決定的なので、解析は 2 本の正確な
CFG edge をともに探索します。これにより ERC-1167 fallback recovery を保ちつつ、読めない
selector condition を証拠にはしません。本当に Unknown な dispatcher は引き続き fail closed です。

`EVMAnalysisLimits.def` は `MaxLowDiagnostics` と `MaxLowDiagnosticBytes` により、linear decoder
と CFG builder に単一の aggregate LowIR diagnostic budget を与えます。両経路とも正確な count
と最終 byte を precharge し、zero limit を拒否します。LowIR と HighIR の diagnostic budget は
互いに独立しています。同じ表は `MaxHighDispatchCandidates`、
program-wide aggregate
`MaxHighRecoveredArguments`、`MaxHighDiagnostics` と `MaxHighDiagnosticBytes`、
`MaxHighReferenceVisits`、`MaxHighMemoryTransferCells`、`MaxHighMemoryValueVisits` を
独立に課金します。candidate/recovered-argument record は、どちらの destination container への
挿入や name/type allocation よりも前に precharge されます。fixed malformed-IR diagnostic を
含むすべての HighIR output diagnostic は、構築/コピー前に count と最終 message byte を正確に
課金します。budget 不足は named hard error であり、diagnostic や fact を黙って省略しません。
default root CFG region は block-PC list を reserve または copy する前に
`MaxHighRegionBlockReferences` を課金します。

`EVMABIParserLimits.def` は tuple nesting、type node、aggregate array dimension を、
`EVMABITableLimits.def` は public signature/variant table の cardinality と aggregate text を
制限します。public table validation は parse/hash より前に上限を適用し、invalid enum、kind
metadata、standard、selector-evidence role、noncanonical type、derived hash、membership、
collision を拒否します。production selector lookup は indexed、event lookup は topic 順に sort
された table を使います。topic API は比較/ordering の前に `APInt` がちょうど 1 EVM word か
検証します。

`EVMInterpreterLimits.def` は `MaxSteps`、`MaxMemoryBytes`、`MaxTraceEntries`、
`MaxLogEntries`、aggregate `MaxLogDataBytes`、aggregate `MaxHostReturnDataBytes`、
`MaxCalldataBytes`、aggregate `MaxHostEnvironmentEntries`、aggregate
`MaxExternalCodeBytes`、`MaxPersistentStateEntries` を宣言します。host entry の aggregate は
`BlockHashes`、`Balances`、`CodeHashes`、`ExternalCode`、`BlobHashes` 全体、external-code byte
limit はすべての `ExternalCode` body 全体に適用されます。`MaxSteps` は明示的な `StepLimit`
のままです。
runtime memory、trace、log、log data、新しい persistent-state key は precharge され、上限超過は
`ResourceExhausted` として persistent state、log、selfdestruct effect を rollback します。
初期 host return-data aggregate または persistent-state map の超過は `execute` API error です。
interpreter は host return data を `ArrayRef` view として保持し、検証済み sort 済み instruction
table を `lower_bound` で検索するため、buffer copy や実行ごとの PC map 再構築は行いません。
`const execute preflight` は environment の copy、persistent-state snapshot の取得、result の構築より
前に、program とすべての host-input limit を検証します。

### go-ethereum HEAD とのライブ差分監査

標準のローカル監査と CI は、実行のたびに `git fetch --depth=1 --force` で公式
`https://github.com/ethereum/go-ethereum.git` default branch の remote `HEAD` を
取得します。各実行は予測不能な名前の private temporary bare repository を作り、shared
persistent Git repository や cache は使いません。revision を選べるのは、その fetch が返した
authority ref と、そこから解決した exact SHA だけです。SHA を表示し、detached temporary
worktree で probe した後、authority repository と worktree をまとめて破棄します。`local_docs`、既存
checkout、submodule は監査経路ではなく、pin された submodule は live drift を検出すべき時に
古くなります。

すべての Git command は、継承した `GIT_*`（`GIT_CONFIG_*` を含む）を最初に全消去し、
監査済みの値だけを設定します。`GIT_CONFIG_NOSYSTEM` と `GIT_CONFIG_GLOBAL` は
system/global configuration を、`GIT_ATTR_NOSYSTEM` と command scope の
`core.attributesFile` は system/global attributes を、`core.hooksPath` は hooks を
無効化します。private repository は想定外の
local configuration、graft、`objects/info/alternates`、`refs/replace` を拒否し、
`GIT_NO_REPLACE_OBJECTS` も replacement lookup を無効化します。逸脱は fail closed です。

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

public CLI が受理する唯一の option は `--manifest-output` で、remote/ref/toolchain の override は
提供しません。出力 manifest の closed contract は `schema 3` です。

Go probe は `params.Rules` の exported boolean field 全体を reflection し、対応付けた各
fork で公開 `LookupInstructionSet(params.Rules)` を呼び、256 byte slot すべてを走査します。
allocation は geth の `operation.undefined` だけで判定します。`HasCost` は defined zero-cost
operation に対しても false なので、cost の cross-check にしか使いません。すべての
`defined && !HasCost` slot は、宣言した activation fork から
`EVM_GETH_ACTIVE_WITHOUT_COST` と正確に一致しなければなりません。cost を持つ undefined slot、
未レビューの defined slot、marker を隠す upstream 変更は fail closed です。closed schema が受理するのは
schema version、exact geth revision、Go version、stack limit、fork rules、各 opcode の
byte、name、`base_min_stack`、`net_stack_delta` だけで、audit manifest は diagnostics array
だけを追加します。未知または重複した field、rule、fork、name、byte はエラーです。
`EVMUpstreamOpcodePolicy.def` は name alias と typed・レビュー済みの
historical/unscheduled-EOF exclusion を所有し、overlap/inactive invariant を検証します。
直交する `EVMUpstreamSemanticsPolicy.def` は closed `params.Rules` reflection inventory、fork
mapping、base-stack exception、EIP-8024 dynamic opcode family の宣言を所有します。CI は `dev` への push、pull request、手動実行、
日次 schedule で動き、失敗時には正確な revision、manifest、log を artifact として保存します。

具体的には、`EVMUpstreamSemanticsPolicy.def` は export された boolean `params.Rules` field
それぞれを、唯一の `EVM_GETH_RULE_FIELD` により `MappedForkSelector`、
`NoOpcodeAllocation`、`ExcludedSelectorExpectedError` のいずれかへ分類します。audit は field を
1 つずつ有効にして `LookupInstructionSet` を呼び、最初の 2 category では nil error、3 番目では
error を要求します。返された完全な 256-slot opcode/stack fingerprint は必ず `ExpectedFork`
と一致しなければなりません。現在の no-allocation fields `IsEIP155`、`IsEIP2929`、
`IsEIP4762`、`IsPetersburg` は Frontier fingerprint、`IsUBT` は error と Cancun fingerprint
が期待値です。

EIP-8024 dynamic opcode family の membership と activation は
`EVMUpstreamSemanticsPolicy.def` が宣言し、`EVMEIP8024Immediates.def` は引き続き single/pair の
各 byte に対する immediate semantics の唯一の authority です。single/pair inventory は各 256
byte value を valid/invalid に明示分類し、production decoder は直接 lookup します。live audit
は `go -overlay` で `core/vm` に virtual wrapper を注入して本物の private
`operation.execute` handler を取得し、active な table/family ごとに `DUPN`、`SWAPN`、
`EXCHANGE` の `3x256` candidates と `3 missing-operand cases` を実行します。acceptance、PC
delta、unique marker から導出した stack operand/mutation、valid case の正確な underflow、
operand 欠落時の `0x00` を検査します。Python は同じ `.def` と項目ごとに比較し、decode formula
を複製しません。

`EVM_HARDFORK_LATEST` の canonical target はちょうど 1 つです。closed
`EVMUpstreamForkAliases.def` は Prague を Pectra、Osaka と BPO1〜BPO5 を Fusaka に mapping
し、Paris、Shanghai、Cancun、Amsterdam、Bogota は identity とします。未知の新名称は fail
closed です。各 audit は 1 つの `audit_unix_time` を固定・記録し、
`MainnetChainConfig.LatestFork(time)` が NeverD latest に map すること、
`LatestFork(max uint64)` が alias inventory 内で canonical fork も probe 済みであることを
要求します。probe は実在する `canonical fork jump tables` と
`mainnet active/scheduled jump tables` を列挙して一表ずつ完全比較し、dynamic family または
fork の `inactive` 状態も明示的に記録します。一部の表・family・probe しか得られない
`partial` result は manifest として受理せず fail closed です。manifest は
`authority=official-fresh-fetch`、公式 URL、要求した `HEAD`、解決した SHA を記録します。
public CLI に remote/ref/toolchain bypass はなく、probe は `GOTOOLCHAIN=local` 固定です。

Go と Python は hostile metadata を materialize する前に制限します。両者は
`input/collection/string hard limits` を適用し、上限を超える JSON input、array、string は
fail closed です。別途 `bounded diagnostic output` を強制し、長すぎる diagnostic の表示には
full-content `digest` と `explicit truncated marker` が含まれるため、完全な message と誤認され
ません。各 child command の output と deadline も bounded です。timeout または output-limit
違反は `process group` 全体と子孫 process tree を kill して pipe を drain します。すべての
`.def parser` は unparsed、unknown、duplicate、missing、out-of-range の entry を拒否し、
どの逸脱にも fail closed します。

現在の schema-3 live receipt は `schema_version=3`、
`audit_unix_time=1787534659`、`authority=official-fresh-fetch`、
`remote=https://github.com/ethereum/go-ethereum.git`、`ref=HEAD`、revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`、local `Go 1.24.0`、
`stack_limit=1024`、`diagnostics=[]` を記録しています。`21 fork tables` と
`20 Rules probes` を比較し、分類は `15 mapped/4 no-op/1 expected-error` です。2 つの
`mainnet active/scheduled` record はともに `upstream BPO2` を示し、closed alias はそれを
`NeverD Fusaka` に map します。EIP-8024 は `23 table targets` を対象とし、active なのは
`Amsterdam/Bogota` だけです。その結果は `1536 candidate executions` と
`6 missing-operand cases` で、`three handler symbols` は 2 つの active target 間で一致します。
closing test は Python audit `67/67` と `C++ Opcode 10/10` です。macOS では実際の audit が
`sandbox-exec` 内で成功し、最後の `go run` は offline でした。Linux workflow は
`bubblewrap` を必須にします。

すべての Go phase、すなわち `go env`、`go mod init`、`go mod edit`、`go mod tidy`、
`go mod download`、`go run` は `capability-root` filesystem sandbox 内で実行されます。read
capability は private probe、fresh geth worktree、検証済み `resolved GOROOT`、必要な system
runtime root の正確な集合に限定され、書き込み可能なのは isolated environment root だけです。
network capability は必要な dependency phase にだけ付与され、final run は offline です。
`host HOME/workspace` の sentinel への access は拒否され、その内容が output に現れることも
ありません。Linux は `/` broad bind を持たない同型の `bubblewrap` policy を使用します。

`NeverDEVMDecoderPropertyTests` は decoder が変わる各 fork で全 2-byte 入力を網羅し、
完全な decode と正確な `JUMPDEST` boundary を比較します。さらに長さを制限した決定的な
hostile byte string を全 fork に通します。

LowIR/MedIR の path lane は path 内の相関を保持し、`MayReachable` は CFG candidate のみで
確定 fact を作りません。HighIR の selector、receive、fallback、return shape、byte-granular
memory fact は definitely reachable executing lane だけを使います。shared selector と
per-standard `KnownFunctionVariantInfo` は分離され、return type はすべての successful
terminal の shape check を通る必要があります。すべての analysis budget exhaustion は
fail loud で、emergency widening や silent truncation を行いません。

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
