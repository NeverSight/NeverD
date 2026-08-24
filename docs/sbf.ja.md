**言語**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Solana SBF 逆コンパイル

[← ドキュメント索引](README.ja.md)

NeverD は Solana deploy artifact を first-class SBF program として読み込み、CLI と
`libneverd` から全経路を提供します。

```text
SBF ELF
  → version-aware ELF loader / verifier
  → lossless LowIR + CFG
  → normalized MedIR + register facts
  → function、syscall、CPI/account observation、region を復元
       ├─ verified LLVM IR
       ├─ portable C11
       └─ safe stable Rust
```

Solana program を generic Linux eBPF とみなさず、現在の Anza `sbpf` VM に従います。
version/opcode/syscall/relocation/protocol metadata は `include/neverd/sbf/` の
`.def` database に集約し、loader/backend は generated typed table を利用します。

## 対応 input と VM version

入力は ELF64 little-endian Solana program（`.so`）です。

| SBF | ELF layout | Machine ID | 主な ISA behavior | 状態 |
|-----|------------|------------|-------------------|------|
| v0 | legacy section/relocation | `EM_BPF`, `EM_SBPF` | virtual gap 付き fixed frame、LDDW、legacy memory opcode | legacy |
| v1 | legacy section/relocation | `EM_BPF`, `EM_SBPF` | manually adjusted stack frame | legacy |
| v2 | legacy section/relocation | `EM_BPF`, `EM_SBPF` | PQR arithmetic、移動した memory encoding、swapped immediate subtraction、source-register CALLX | legacy、非単調 |
| v3 | strict program header、dynamic relocation なし | `EM_BPF` | static syscall/call、JMP32、destination-register CALLX、bytecode `0x100000000`、rodata zero | 現行 deploy toolchain format |
| v4 | strict program header、dynamic relocation なし | `EM_BPF` | v3 ISA と aligned memory-mapping contract | upstream `sbpf` 現行。cluster availability は異なり得る |

version 番号それ自体は仕様ではないため、`SBFVersionFeatures.def` が振る舞いの変更
を保持し、version テーブルがそれらを合成します。各レコードはその変更を受理した
SIMD 提案と、`anza-xyz/sbpf` が同じ問いに対して公開している述語を持ちます。複数
の提案が 1 つの version に着地し、1 つの提案が無関係な複数の点を変えるためです。
SIMD-0173 は memory instruction class を移すと同時に `lddw` を廃止し、SIMD-0174
は同じ version で独立に PQR class を追加します。提案を version ではなく feature
に記録することが、復元した version の主張をそれを決めた文書まで辿れるようにし、
2 つの `callx` 規則を別々の feature にしている理由でもあります。SIMD-0173 は
source register を、SIMD-0377 は destination register を読みます。

v2 の変更は v3 に漏れません。feature check は明示的で、`version >= N` とは推測
しません。既定の strict mode は malformed header/range/alignment、unsupported
writable legacy section、invalid continuation/register/frame-pointer write/branch、
version-inactive opcode を instruction slot と virtual address 付きで拒否します。

## 記述が対象とする runtime

ISA version はファイルから分かります。それ以外はほとんど分かりません。どの syscall
が解決するかは chain と slot に依存し、account フィールドが何バイト目にあるかはその
プログラムを所有する loader に依存し、entrypoint が第二引数を受け取るかは chain が
入れる切り替えに依存し、プログラムを deploy できるかどうかは実行できるかどうかとは
別の問いです。単一の version 切り替えではそのいずれも表現できないため、これらは
別々の軸として別々のテーブルに持ちます。

`SBFRuntimeFeatures.def` は cluster、用途、そして NeverD の報告内容を変える gate を
記録し、それぞれに runtime 上の識別子、有効化状態を記録する feature account、各 cluster
が有効化した slot を持たせます。pending account は存在しても gate を有効化していない
場合があります。ある cluster の有効化行を持たない gate は、そこではまだ
有効化されていません。`simd-0321` はすべての cluster で有効です。`simd-0449` と
SHA-512 syscall は testnet と devnet で有効、mainnet では無効であり、devnet で動く
プログラムが mainnet で失敗するのはまさにこのためです。

pin 済み Agave revision では、`syscall_parameter_address_restrictions`
gate (`simd-0459`) が syscall と CPI の引数に対する VM address と alignment の
契約を厳格化します。finalized RPC state が記録する有効化 slot は mainnet が
429,840,000、testnet が 407,468,256、devnet が 462,240,000 です。
`account_data_direct_mapping` gate は、adjusted address space の使用時に account data
を input buffer 内のコピーから直接 backing された memory region へ切り替えます。
mainnet では未有効化で、testnet の 408,332,256 と devnet の 463,968,000 で
有効化されます。どちらの gate も新しい Account ABI を作らず、ABIv0/ABIv1 の論理 field
offset も変えません。serialization を選ぶのは引き続き所有 loader であり、NeverD は
両 gate を runtime topology metadata として記録します。

feature bit は append-only のままです。観測対象 snapshot が 32 bit を超えたため、
`RuntimeFeatureMask` が storage と host ABI で唯一の `uint64_t` 型です。
v2 ABI の幅は固定され in-place で拡張しません。64 bit を超えるフィールドには v3 または multiword 表現を追加し、v2 の幅は変更しません。
`RuntimeFeatureDisposition` は、現在も生きている `RuntimeBranch` と、pin 済み revision
では active 側が無条件になった一方で historical slot では旧側も意味を持つ
`FoldedBranch` を区別します。finalized RPC の有効化情報（`—` は未有効化）:

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

この範囲は Agave の `FeatureSnapshot` 全体を対象にするとは意図的に主張しません。
NeverD が含めるのは、decoding または出力 host contract を直接変える loader、
verifier、VM、entry/input、syscall、CPI infrastructure gate だけです。transaction
scheduling、fees、consensus、transaction-level precompile verification、および
`CPI target built-in` の業務 semantics は `external runtime` の責任です。それらの
built-in を実装せず bit だけ追加すれば、NeverD にない能力を表示してしまいます。

`SBFLoaders.def` は所有関係とシリアライズを記録します。deploy と実行が同じ答えで
なくなってから何年も経ちます。`loader-v1` と `loader-v2` は送られてくる management
instruction をすべて拒否しつつ、すでに所有しているプログラムは動かし続けます。
そのシリアライズが今も読めなければならないのはこのためです。

| Loader | シリアライズ | deploy | 実行 |
|--------|--------------|--------|------|
| loader-v1 | `abi-v0` | 不可 | 可 |
| loader-v2 | `abi-v1` | 不可 | 可 |
| loader-v3 | `abi-v1` | 可 | 可 |
| loader-v4 | `abi-v1` | 不可 | 不可（built-in が削除済み） |

`SBFAccountLayout.def` は各シリアライズにおける account フィールドの位置を与えます。
両者は padding が違うだけではなく、フィールドの順序そのものが違います。offset 3 では
unaligned 形式は account アドレスの先頭バイト、aligned 形式は executable フラグに
なり、値そのものはどちらを読んだのかを何も告げません。さらに重複 account は
`abi-v0` で 1 バイト、`abi-v1` で 8 バイトを占めるため、単一フィールドではなく
エントリ列全体の走査がずれます。

呼び出しが解決するかどうかは一つではなく三つの問いなので、
`SBFSyscallLifecycle.def` は公開シグネチャがどれだけ固まっているかを保持し、
`SBFSyscallRegistration.def` が残りを保持します。すなわち syscall がどの registry に
現れるか、どの gate が支配するか、その gate がどちら向きかです。向きが重要なのは、
gate が追加と同じくらい容易に削除もできるからです。fees sysvar syscall を取り除いた
のは `disable_fees_sysvar` の有効化でした。削除する gate を追加する gate として読めば、
すべての cluster について答えが一度に反転します。`sol_alloc_free_` は境界の前後とも
実行用には登録されたままです。deployment は
`disable_deploy_of_alloc_free_syscall` より前には登録し、cluster 固有の有効化 slot
以降は拒否します。pin 済み Agave revision では active な deployment 側が registry
構築へ fold 済みですが、NeverD は historical profile が有効化前の答えを得られるよう
gate を保持します。

`simd-0321` を有効化した runtime では、entrypoint は instruction data のアドレスも
`r2` で受け取ります。NeverD はこれを定数ではなく固有の値種別としてモデル化します。
どこに置かれるかは account 次第であり、アドレスを捏造すれば、それを経由した load を
名前付きの account フィールドとして報告しかねないからです。有効化前のレジスタは
zero で届き、それを読むプログラムは zero を読みます。したがって生成される LLVM、C、
Rust の entry point は input buffer と instruction data を受け取ります。第二引数を
渡せない callable では、それを読むプログラムを再現できないからです。

現行 toolchain は `cargo build-sbf` を使い、v3+ production program は Rust 中心です。
upstream C toolchain が v3 を target にしないことは NeverD output を制限せず、すべての
accepted SBF input を C/Rust の両方に出力できます。

- [Solana programs](https://solana.com/docs/core/programs)
- [Program execution](https://solana.com/docs/core/programs/program-execution)
- [Syscall reference](https://solana.com/docs/core/programs/syscall-reference)
- [Anza sbpf VM](https://github.com/anza-xyz/sbpf)
- [Agave changelog](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

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

# 答えがどの runtime についてのものかを指定する。いずれもプログラムファイルには
# 書かれていない。
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`、`--sbf-slot`、`--sbf-loader`、`--sbf-purpose` は runtime profile を
選びます。既定値は現在の mainnet-beta を、`loader-v3` のもとで、すでに deploy 済みの
プログラムとして記述します。代わりに deployment について尋ねると、chain 自体は動かし
続けるにもかかわらずプログラムを chain に載せられなくする syscall を報告します。

`--sbf-version=auto|v0|v1|v2|v3|v4` は検出 layout の ELF check 後に instruction
semantics だけを変更する研究 fixture 向け option です。不信な file を別 packaging
standard として再解釈するためには使えません。

## 解析と復元

LowIR は 8-byte encoding、raw field、LDDW continuation、resolved call、syscall hash、
block/edge、reachability、diagnostic を保持します。MedIR は version-specific encoding
を typed 32/64-bit operation、explicit extension、guarded arithmetic、memory width、
call kind に正規化し、register dataflow は constant と stack/rodata address を追跡します。

HighIR は entry/internal function、direct call edge、official syscall name、string、
natural loop、reducible conditional、conservative Solana observation を復元します。
`sol_invoke_signed_rust`/`sol_invoke_signed_c` は CPI、input register 基点 memory は
account/input access です。IDL なしに Anchor type/account layout を作りません。

C/Rust は backend-neutral structuring pass を共有します。一意な reducible 表現なら
`if`/`if-else`、`while`/`loop` を出力し、internal call、CALLX、irreducible flow は
exact PC dispatcher を残すため、可読性が semantics を変えません。

syscall database は logging、memory、PDA、SHA-256/Keccak/Blake3、Poseidon、
secp256k1、curve/alt-bn128、big modular exponentiation、CPI、return data、sibling
instruction、compute unit、epoch rewards 等を含みます。legacy relocations
`R_BPF_64_64`、`R_BPF_64_RELATIVE`、`R_BPF_64_32` は中央処理されます。text
relocation は LDDW 両 half と official Murmur3 CALL key を decode 前に適用します。
すでに `R_BPF_64_32` が適用・strip 済みなら symbol/target slot から registry key を
再計算して internal call を復元します。

## Solana プログラム復元

SBF マシンモデルの上で、NeverD はそのプログラムが Solana プログラムとして何を
意味するかを報告します。記録される事実には必ずその根拠が付き、バイト列が決めない
ことは推測せず未設定のままにします。

| 復元対象 | 根拠 |
|----------|------|
| read-only データ中の base58 アドレス | `SBFKnownAddresses.def` と `SBFAnchorNamespaces.def` との一致、またはコードが生成する定数 |
| プログラム自身の宣言アドレス | read-only 定数に対する鍵長ちょうどの `sol_memcmp_` |
| Anchor instruction dispatch | 定数が namespace 付き SHA-256 discriminator と一致する 64-bit 比較 |
| CPI の呼び出し先 | invoke 引数から到達できる instruction レコード |
| 呼び出しが選ぶ操作 | `SBFProgramInstructions.def` に載る selector、または先頭の Anchor discriminator |
| PDA の seed | derivation 引数から到達できる seed descriptor 配列 |
| account フィールドの読み書き | アドレスが serialized input 内にあると証明できる load/store |

loader が渡す引数は input region 先頭の serialized input buffer 一つだけなので、
その entry state からの定数伝播により raw offset ではなく名前付き account フィールド
が得られます。`SBFAccountLayout.def` が公式のシリアライズを保持し、その固定
フィールドが隙間なく領域を敷き詰めることを検査します。

Anchor は `<namespace>:<name>` を SHA-256 でハッシュし先頭 8 バイトを取って
discriminator を作る一方向処理です。そこで NeverD は候補の確認のみを行います。
`SBFAnchorNames.def` は実運用プログラムで繰り返し現れる名前の辞書で、`--sbf-idl`
はそのプログラム自身の IDL を与え、こちらが優先されます。64-bit 比較は、少なくとも
一つが名前に解決されて初めて discriminator と呼ばれます。

`SBFKnownAddresses.def` はプロトコルおよび正準プログラムのアドレスを記録します。
各エントリはちょうど 32 バイトにデコードされねばならず、テストがそれを強制します。
復元には syscall ABI も必要です。SBPFv3 は read-only データを仮想アドレス 0 に
マップするため、長さ引数と低位のデータアドレスは同じ数値になります。したがって
`SBFSyscalls.def` はどの引数レジスタが VM アドレスを持つかを記録し、それだけを
追跡します。

二つの invoke syscall は同じ instruction を別々の構造で表すため、`SBFCPIABI.def`
が両方のレイアウトをそれを選ぶ syscall ごとに保持します。取り違えても失敗はせず、
最初の account を呼び出し先として静かに誤報するだけです。`SBFProgramInstructions.def`
は各プログラムが自ら公表する selector で操作を命名します。system、stake、
lookup-table、upgradeable-loader は bincode の variant 番号、token 系は先頭 1 バイト
で、元の token program と共有する番号の上に Token-2022 の拡張範囲が重なります。
未収録の selector は数値として報告します。

### scratch メモリと syscall ウィンドウ

プログラムが runtime に定数を直接渡すことはほとんどありません。seed 配列、
シリアライズされた instruction、その payload を自分の frame か heap に組み立て、
ポインタだけを渡します。ロードした image だけを読むとポインタしか見えないため、
復元はこのプログラムだけが書けるメモリのバイト単位モデルを保持します。上限は
`kMaxModeledScratchBytes` です。

scratch の復元は demand-driven です。Solana CPI/PDA scratch の fixed point は
実際の `scratch consumer` が存在するときだけ構築し、consumer がない program は
`whole-CFG fixed point` をスキップします。`SBFAnalysisLimits.def` は host の
`analysis policy` を定めるもので、`protocol limits` ではありません。
`MaxModeledScratchBytes` は `program point` ごとに 1,024 bytes、
`ScratchFlowRetainedByteBudget` は 8,388,608 bytes の `logical retained estimate`
です。予算を超えると復元は明示的に `ScratchRecoveryPrecision::BlockLocal` へ
widening します。失うのは `cross-block must-facts` だけで、`block-local replay` は
`sound` のまま、`same-block stores` も復元できます。printer は安定して `recovery scratch-precision=block-local` を出力し、
`half-converged must-facts` を widening から返しません。

呼び出しの後に何が残るかは二つの表が決めます。`SBFSyscalls.def` はどの引数レジスタが
VM アドレスを持つかを、`SBFSyscallMemory.def` は runtime がそれを通して何をするかを、
`Fixed`／`Counted`／`Opaque` の範囲を伴う read か write として記録します。write
ウィンドウを持たない syscall は呼び出し側の 1 バイトも変えられないので、`sol_log_`
の前に証明された内容はその後も成立します。長さ引数で区切られた write はそのウィンドウ
だけを無効化し、`Opaque` な write は基底アドレスとその上を無効化します。バッファは
開始位置より下へは伸びず、VM region の境界も越えないからです。`SBFSyscalls.def` の
効果要約とこのウィンドウ表は双方向に検証され、どちらか一方だけがずれることはありません。

`sol_memcpy_`、`sol_memmove_`、`sol_memset_` は無効化するだけでなく追跡します。
宛先・長さ・元がいずれも証明できるとき、宛先のバイトが判明します。Anchor プログラムの
payload はマップではなくコピーで置かれるため、呼び出す操作が分かるのはこの一歩です。

scratch を保持できるのは解決済み runtime syscall だけで、その監査済み write window
に従う場合に限ります。内部・間接・その他の未解決 call は、現在 scratch を指す引数が
なくても、モデル化済み byte を消去します。以前に escape した pointer や global alias
を通じて呼び出し先が書き換えられるためです。`sol_invoke_signed_rust` と
`sol_invoke_signed_c` が書くのは account data であって呼び出し側のメモリではないため、
同じ block で組み立てた二つの invocation はどちらも読めます。

このモデルは関数内 CFG 上の前方 must 解析です。ある block に至るすべての経路が同じ値
を書いたときだけ、そのバイトはその block まで残ります。呼び出し先は呼び出し側の frame
を引き継がないので call edge は辿りません。依存 worklist に block 数による精度低下の
逃げ道はなく、任意実行の Release gate が 10 MiB、`1,310,720` 命令の上限全体を検査します。

`SBFLints.def` はプログラム全体の観測を分類します。signer / owner チェックの欠落、
定数でない invoke 先、非推奨または feature gate 付き syscall、そして SIMD-0500 が
deploy を受け付けなくなる SBPF version です。各項目は severity と confidence を
持ち、lint がデコード済み意味論を変えることはありません。この層はネットワークに
一切接続しません。

## 生成 LLVM runtime contract

LLVM は VM address を host pointer にしません。checked load/store/syscall declaration
は `i32` status を返し、load/syscall value は output pointer に `i64` で書きます。
nonzero status は explicit SBF fault block に分岐し、module は
`llvm::verifyModule` を通過します。

## 生成 C host contract

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

`width` は bit 単位です。生成される C callback はすべて `int` を返し、
`syscall_with_features` も同じです。v1 entrypoint `neverd_sbf_program` では 0 が成功を
示し、`load` または `store` の非 0 return は `NEVERD_SBF_MEMORY_ACCESS` に、
`syscall` の非 0 return は `NEVERD_SBF_UNKNOWN_SYSCALL` に normalize されます（`v1-load-store-nonzero`、
`v1-syscall-nonzero`）。v1 は
callback の exact status をそのまま返しません。内部の `InvalidRegister` と
`InvalidBranch` も `NEVERD_SBF_INVALID_INSTRUCTION` に normalize されます
（`internal-invalid-instruction`）。
v2 entrypoint `neverd_sbf_program_v2` は exact status の経路です。認識された
`neverd_sbf_status_v2` callback value（9 と 10 を含む）は handled fault として保持され
（`v2-exact-status`）、
v2 entrypoint は内部の `InvalidRegister` と `InvalidBranch` も 9 と 10 として保持します。
未知の callback value には生成器の operation-specific fallback
（`operation-specific-fallback`）を使います。
`syscall_with_features` が null なら `base.syscall` に fallback し、その callback も `int`
を返します（`feature-aware-null-base-syscall`）。
v1 の struct と entrypoint は legacy host と互換です。分離された v2 entrypoint で
`syscall_with_features` と解決済み runtime-feature snapshot を受け取れます。生成 source は
register、return PC、callee-saved r6-r9、frame pointer、VM address、division fault、wide PQR
operation、wrapping shift を表します。実際に使う helper だけを出力するため、最小 output も
`clang -Wall -Wextra -Werror` を通ります。

## 生成 Rust host contract

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

旧 entrypoint `neverd_sbf_program` と `SbfEnvironment` は
`v1-result-abi` です。host の method は `Result` を使います。
`Some(SbfRuntimeFeatures::from_bits(0))` は `explicit-empty-snapshot` を表し、
`None` とは異なります。`syscall_outcome` は Result-based host method と
`SbfSyscallOutcomeV2` の間の `result-host-bridge` です。
`SbfErrorV2` には `#[non_exhaustive]` が付くため、呼び出し側は match で
`non-exhaustive-wildcard`（`_`）を使う必要があります。

出力は raw pointer のない safe stable Rust です。entry point は trait generic で、
register/call frame に fixed-size safe array を使い、test は
`rustc --edition=2021 -D warnings` で compile します。

## C API

SBF load 後も recovered function、disassembly、IR dump、CFG/call graph JSON、section、
symbol、relocation、string、header の session operation は共通です。Rust は ABI-stable
に追加された output-language enum で明示します。

```c
neverd_session_t session = neverd_session_create();
neverd_sbf_set_strict(session, 1);
neverd_sbf_set_version(session, "auto");
/* 答えがどの runtime についてのものか。既定値は現在の mainnet-beta を、
   loader-v3 のもとで、すでに deploy 済みのプログラムとして記述する。 */
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

## 検証と制限

`unittests/sbf/` は metadata invariant、v0-v4 loader、strict verifier、CFG/recovery、
verified LLVM、warning-free C/Rust compile、MedIR から独立した raw interpreter、public
C API を網羅します。conditional+loop fixture は両言語で実行して raw oracle と比較し、
official `sbpf` ELF corpus も vendoring せず local compatibility check に使います。

- SBF binary rewrite と object-code roundtrip は明示的に拒否します。
- Anchor IDL/type recovery と live RPC/account fetch は loader 範囲外です。
- generated source の syscall/VM memory は host contract 経由で、単独 runtime ではありません。
- relaxed mode は inspection 用で、invalid instruction に推測 semantics を与えません。

## 現在の conformance baseline（2026-08-24）

relocation 後は、VM address を持つ単一の immutable `ProgramImage` が decoder、
interpreter、string recovery、LLVM/C/Rust backend 共通の source of truth です。
loader semantics とずれ得る独立した text/rodata copy はありません。

閉じた record は `SBFVersions.def`、`SBFOpcodes.def`、
`SBFRelocations.def`、`SBFArgumentRegisters.def`、`SBFVersionFeatures.def`, `SBFProtocolLimits.def`、
`SBFSyscalls.def`、`SBFSyscallMemory.def`、`SBFCPIABI.def`、
`SBFProgramInstructions.def`、
`SBFUpstreamSources.def` に置きます。一度しか使わない diagnostic と LLVM block
name は、LLVM 自身の方針どおり local に保ちます。

`SBFProtocolLimits.def` は旧来の 65,536 instruction と現在の 10 MiB account
data 上限を記録し、NeverD は後者から保守的な decode 上限を導出します。

strict v3/v4 では bounds-check 済み program header が runtime contract です。
section/symbol table は optional debug enrichment なので、欠落・破損しても有効な
image を無効にしません。legacy v0-v2 は `.text`、`.rodata`、`.data.rel.ro`、
`.eh_frame` を統合し、`R_BPF_64_64`、`R_BPF_64_RELATIVE`、`R_BPF_64_32`
を image の immutable 化前に一度だけ適用します。

| Evidence | 監査結果 |
|----------|----------|
| official ELF manifest | `sbpf/tests/elfs` の 23/23 artifact |
| official oracle | `NeverDSBFExternalOracleTests` が pinned verifier と 1,411 opcode/boundary case を照合 |
| differential execution | raw-byte oracle と LLVM ORC/C11/stable Rust の memory/fault/syscall trace 比較 |
| integrated aggregate | `check-neverd-sbf` が登録 suite をすべて実行し、変動する総数は固定しない |
| ASan + UBSan | focused target を fail-fast で実行し report なし。変動する総数は固定しない |

監査 pin は Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84` と Agave
`ef210d67f2fabeee1730498188fa78854260c679` です。更新時は
`SBFUpstreamManifest.def`、`SBFUpstreamOpcodes.def`、
`SBFUpstreamSources.def` を確認して実行します。

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

比較では `sol-azy` が現在の strict ELF で crash し、legacy CFG に undefined node
を残しました。`solana-data-reverser` は account data 向け、`SolDragon` は analysis
を WIP とし、`bn-ebpf-solana` は Binary Ninja を必要とします。したがって official
`sbpf` と Agave が semantic authority です。

## 2026-08-24 監査済み evidence contract

`SBFUpstreamSources.def` は Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`、Agave
`ef210d67f2fabeee1730498188fa78854260c679`、Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd` を固定します。official manifest
は 23/23 を通過し、`NeverDSBFExternalOracleTests` は独立 build した official
verifier と 1,411 opcode/boundary case を `SBFOfficialOracleProtocol.def` と
`SBFOfficialVerifierCases.def` と `SBFOfficialExecutionConstants.def` 経由で照合します。malformed ELF は
`SBFOfficialELFMutations.def` と table-driven corpus に由来し、変動する総数は
文書契約にしません。
別軸の `41-case strict ELF の差分検証` は strict-v3 matrix 全体を official
`verify-elf-batch` process と NeverD に通します。この 41 case は 1,411 の
opcode/verifier total には含まれません。

公式の追加実行行列（`additional execution matrix`）は独立しています。
`(Version,Opcode)` の active case は正確に 508 件、boundary case は 58 件で、
合計 566 件の exact execution case です。これは 1,411 件の `verifier probes`
や 41 件の strict ELF の差分検証を置き換えず、その件数にも含めません。

`NeverDSBFAgaveConformanceTests` は Firedancer test-vectors revision
`68bb4af40235562e8852fa23d5727e49c2a0b862` も認証し、1,955 個すべての
`sol_compat_elf_loader_v1` fixture（accept 1,399、reject 556）を照合します。accept
された各 ELF では `entry_pc`、`text_off`、`text_cnt`、`rodata_hash`、
`calldests_hash` も比較します。Agave の段階を混同しないよう、この gate は loader
だけを検査し、後段の instruction verifier は実行しません。

default chain profile は Agave に忠実です。`SBF_RUNTIME_VERSION` row が historical
cluster/slot ごとの maximum ISA を official feature account activation に従って
V0→V1→V2→V3 と進め、現在の maximum は V3 のままです。これは
`RuntimeVersionPolicy::ChainProfile` で扱います。明示的な
`--sbf-version=v4` だけが `RuntimeVersionPolicy::UpstreamToolchain` を選び、
pinned `sbpf` に基づく offline 分析を可能にします。これは v4 の on-chain
activation を意味しません。現在の 10 MiB 上限は正確に `10'485'760` byte です。
65,536 は historical provenance/test としてのみ保持され、実行時には使われません。

feature、syscall、fault、source ABI の authority は typed `.def` registry、すなわち
`SBFSyscallRegistration.def`、`SBFValidationRules.def`, `SBFFaultCodes.def`、
`SBFSourceStatuses.def`、`SBFArgumentRegisters.def`、`SBFEdgeKinds.def` です。
`SBFFaultCodes.def` は execution fault の安定値を持ち、
`SBFSourceStatuses.def` は別レイヤーの generated-source ABI を持ちます。
loader は `raw-first` で、relative CALL を直し、raw relocation を ELF ordinal
順に一度だけ適用します。error order は text identity、CALL、relocation、
entrypoint、read-only layout で固定されます。file/VM mapping は gap-aware で、
gap 内に byte を捏造しません。

CFG/dataflow は per-function です。call edge は local predecessor にならず、shared
tail は ambiguous のまま、同じ loop の全 latch は一つの multi-latch region に
なります。worklist/ownership は 10,000 function、reverse-order block、
conditional latch fixture で検証し、machine 固有の秒数は主張しません。

公開 SBF call graph は `callgraph-budget=fail-closed` です。typed input、
provenance、node、edge、element、`CallGraphOutputByteBudget` により JSON は
exact-or-empty になります。budget を超えると `{"nodes":[],"edges":[]}` を返し、
`neverd_last_error()` を設定して、部分的な relation は公開しません。

各 activation row は cluster、feature account、slot を持つため、通常解析を
offline に保ったまま live node と `RPC activation audit` できます。比較対象は
Blueshift、`qedsvm`（selected path の Lean proof。ただし現行 ELF loader は V0
のみ）、`leanprover-solanalib`、`sol-azy`、`bn-ebpf-solana`、Ghidra/SolDragon
です。`ezBPF` は `88829078a6d7682a2baed0d696d500401c263750` で deprecated と
明記され、Blueshift へ案内しています。単一の byte-to-enum map を持つ archived
predecessor であり、moved-memory、JMP32、現行 v0-v4 matrix に対応する
version-aware decoder ではありません。この snapshot で監査した公開
比較対象の pin は Blueshift `704e40f7aa82446555b19d9ffbc0a6e18a35480f`、
`qedsvm` `99bd5ede85374adc7fc5c835c2432ecf4e123fd1`、`leanprover-solanalib`
`6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec` です。local tool は `sol-azy`
`362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`、`solana-data-reverser`
`bf90923adec984a61ca0437e9d341360ac1b11ee`、`SolDragon`
`002b98677a5e595a773af6607b77210f5ea71db7`、`bn-ebpf-solana`
`c3fe0de45d37eb68dcb08f2498c6e1f986056572` に固定されています。
general-purpose SBF decompiler の中では、
NeverD が最も強い再現可能 evidence を持つと確認できました。これは範囲を限定した
比較であり、絶対的な「世界一」の主張ではありません。

追加で公開競合ツールも監査した。`r2ghidra-solana` は
`eca0b8e2d307e00991e289b8f9b0f45743819f1b` に固定され、Ghidra の C-like UX と
`C-like-pdg`、account/Anchor/string/syscall の表示を提供する。この pin の CI は
成功したが、Solana 専用 testsuite はコメントアウトされ、CI の smoke は
`/bin/ls` の decompile だけである。直接再現でも、公式 V0 の
`relative_call_sbpfv0.so` は妥当な C を出す一方、公式 V3 の `relative_call.so` は
`pdg` で失敗することを確認できる。この結果は再現可能である。`radare2-solana` は
`292d845681be377cadc9959a74c2cadeb6e7f412` に固定され、V2-only の
SIMD-0173/0174 を `>=V2` として V3/V4 まで広げる一方、公式 `program.rs` は
V2-only と明記する。`SBPF-3-1` は
`0e602c93007faa96bccb8e1e12040954ff108b6f` に固定され、cargo test は 2/2 の
簡単なものだけで CI はない。version detection は none/V0 を返す placeholder、
high-nibble opcode decoder は誤り、jump は off でなく imm を使い、V0/V3 の
relative_call ELF も同じ誤った pseudocode になる。NeverD の優位性は V0–V4 の
official loader/verifier/runtime/process-oracle evidence を再現できる点にあり、
各ツールの UX や C output を否定するものではない。

`SBFComparisonTools.def` は比較ツールの表示名と完全 revision の唯一の authority
です。最後の bounded public sweep では、さらに次を確認しました。

- `blastrock/Solana-eBPF-for-Ghidra` を
  `c3ad719004726fe924dbed901eca2744ad82c85d` に固定。実際の Ghidra P-code UX は
  ありますが、version 非依存の SLEIGH model が CALLX を `dst` に固定し、legacy と
  current opcode を混在させます。実質的な test/CI はなく、default source には参照先の
  relocation constant class もありません。
- `SolEmu-Ghidra` は `6520af2ff104d5adbec24632ba3afa3bef0da529`。同一 decoder を
  継承し、明示的に simulated/placeholder である CPI、crypto、ZK behavior の周囲に
  emulator UI を追加しますが、実質的な test/CI はありません。`Ghidra_sBPF` は
  `907bd4476432ca83bb2352686ad1ccafdb38504c`。v1-v3 を手動選択できますが、V2-only
  encoding を V3 に累積し、V0/V4 auto-selection と test/CI がありません。
- `solana-ebpf-ida-processor` は
  `aacd215907266190ed9c6c1b408ca9309f92ecdd`。有用な IDA disassembler/relocation UI
  ですが source lifter ではなく、混在 opcode map は CALLX を常に `imm` から読み、
  test/CI もありません。`solana-bpf-reverse` は
  `39479a3bddb8cb866ee499266a76a1b54069b222`。hard-coded layout から heuristic report と
  Rust TODO scaffold を生成し、実行結果は 9 pass、2 fail、1 skip、CI なしでした。
- `solens` は `22defa1c8f4118dacd42f5c291f1ac31609fc0e5` の V2-only terminal
  disassembler で、テストは 0 件、CI もありません。`sbpf-decompiler` は
  `37b8bc0edc7ce347abee466f5f974e900c1948df` で、現在の実装は 3 行の
  `Hello, world!` のみで、テストは 0 件、CI もありません。
- `sbpf-eye` は `5277a52aeb58e50b6ff8f9020414334765369b49`。明示的に lightweight
  WIP の instruction/CFG TUI でテスト 3 件は通りますが、semantic IR、source emitter、
  CI はありません。`svm_bytecode_analyzer` は
  `12aa236db8964e6be661e38131c2dc81588cf19c` の disassembler/CFG analyzer で lifter
  ではなく、register/offset byte decode が誤り、実行結果は 17 pass、1 fail、CI なしです。
- `giraffexiu/Solana-eBPF-for-Ghidra` は
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07`。同じ Ghidra lineage の 1-commit
  snapshot で、version semantics、test、CI の追加はありません。`CertSBF` は
  `bb93a97cf0c64d119d08ec851e8e820315beb59e`。旧 rBPF semantics の価値ある
  Isabelle/HOL formalization ですが、現在の V0-V4 whole-program source decompiler
  ではありません。

これは限定した公開 snapshot での比較 evidence であり、将来の tool や private project
に対する絶対的な結論ではありません。

2026-08-24 の最終 RPC 監査は完全一致した。feature account は 38 件、activation
row は 89 件で、mainnet slot は 441305159、testnet は 433055669、devnet は
487238699。system-owned の空の pending account（mainnet の
`VirtualAddressSpaceAdjustments`）は activate されていなかった。RPC URL は
文書に固定していない。

Linux Release CI は `--print-pinned-revision`、`--print-test-vectors-revision`、
`--print-toolchain` で exact pin を読み、official oracle と sparse corpus を認証して
`NEVERD_SBPF_ORACLE` と `NEVERD_AGAVE_CONFORMANCE_ROOT` を export するため、両
external test は必須です。明示 oracle/corpus env がない通常 local run は case を
discover しますが skip できます。
