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
記録し、それぞれに runtime 上の識別子、存在すればそれを有効化する account、各 cluster
が有効化した slot を持たせます。ある cluster の行を持たない gate は、そこではまだ
有効化されていません。`simd-0321` はすべての cluster で有効です。`simd-0449` と
SHA-512 syscall は testnet と devnet で有効、mainnet では無効であり、devnet で動く
プログラムが mainnet で失敗するのはまさにこのためです。

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
すべての cluster について答えが一度に反転します。`sol_alloc_free_` には gate が
まったく不要です。runtime はこれを受け付け続ける一方、これを呼ぶ新しいプログラムの
受理は拒否します。これは二つの registry の違いであって、それ以上ではありません。

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
| read-only データ中の base58 アドレス | `SBFKnownAddresses.def` との一致、またはコードが生成する定数 |
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

この解析が記述していない関数への呼び出しは、到達できるすべてを書くと仮定します。
呼び出し先は自分の frame で動くので、引数レジスタがどれも scratch を指さないと証明
できる呼び出しではモデルが残り、それ以外では破棄されます。`sol_invoke_signed_rust`
と `sol_invoke_signed_c` が書くのは account data であって呼び出し側のメモリでは
ないため、同じ block で組み立てた二つの invocation はどちらも読めます。

このモデルは関数内 CFG 上の前方 must 解析です。ある block に至るすべての経路が同じ値
を書いたときだけ、そのバイトはその block まで残ります。呼び出し先は呼び出し側の frame
を引き継がないので call edge は辿りません。`kMaxScratchFlowBlocks` を超える block を
持つプログラムは block 単位の復元を保ち、block 境界を越える事実だけを失います。

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
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;
```

`width` は bit 単位で、nonzero host return は explicit SBF status です。register、
return PC、callee-saved r6-r9、frame pointer、VM address、division fault、wide PQR、
wrapping shift を表し、使用 helper だけを出力するため
`clang -Wall -Wextra -Werror` を通ります。

## 生成 Rust host contract

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

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

## 現在の conformance baseline（2026-08-10）

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
| official ELF manifest | `sbpf/tests/elfs` の 20/20 artifact |
| ISA matrix | v0-v4 ごとに全 256 encoding、合計 1,280 cell と verifier boundary |
| differential execution | raw-byte oracle と LLVM ORC/C11/stable Rust の memory/fault/syscall trace 比較 |
| integrated aggregate | 14 test binary の 145/145 case |
| ASan + UBSan | 13 core binary の 141/141 case、report なし |

監査 pin は Anza `sbpf`
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` と Agave
`cae40aa610fdbdb313209bc1eec737079eb59688` です。更新時は
`SBFUpstreamManifest.def`、`SBFUpstreamOpcodes.def`、
`SBFUpstreamSources.def` を確認して実行します。

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

比較では `sol-azy` が現在の strict ELF で crash し、legacy CFG に undefined node
を残しました。`solana-data-reverser` は account data 向け、`SolDragon` は analysis
を WIP とし、`bn-ebpf-solana` は Binary Ninja を必要とします。したがって official
`sbpf` と Agave が semantic authority です。
