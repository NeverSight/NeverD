**言語**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Windows 例外再構築

[← ドキュメント索引](README.ja.md)

NeverD は Windows のテーブルベース例外情報を、ロード、lift、逆コンパイル、
バイナリ書き換えの全工程で保持します。例外 metadata は関数の実行契約の一部です。
生成コード、runtime-function record、言語テーブル、guard table の整合性を証明できない
書き換えは拒否されます。

この文書では三つのサポートレベルを区別します。

- **解析**：ネイティブ表現を検査済みの正規化 record に decode し、IR pipeline に公開。
- **逆コンパイル**：reducible な protected region を明示的な HighIR 例外 node にする。
  その他は handler や state transition を失わない決定的なネイティブ注釈として保持。
- **ネイティブ再構築**：patch mode が LLVM に完全な代替例外契約を生成させ、最終 PE に
  インストール可能。

解析サポートはネイティブ再構築サポートを意味しません。

## サポート表

| ネイティブ形式 | Lift／解析 | 高水準出力 | Patch mode |
|----------------|------------|------------|------------|
| x64 unwind v1/v2 | 完全に検査された unwind record、operation、chain、handler data、provenance | frame/unwind 要約と、該当する場合は構造化言語 region | 完全な primary record をサポート。生成した `.pdata`/`.xdata` が置換対象 closure を更新 |
| x64 unwind v3/APX | 専用 v3 payload、epilog、operation accounting | 明示的 v3 注釈 | 解析のみ。対象関数の変更を拒否 |
| ARM32/ARM64 packed unwind | function range、packed field、primary/fragment identity | frame/unwind 要約 | 完全な非言語 primary record かつ独立 addressable fragment がない場合のみ |
| ARM32/ARM64 unpacked unwind | 検査済み xdata header/code extent、handler association、fragment | frame/unwind 要約 | 完全な非言語 primary record かつ独立 addressable fragment がない場合のみ |
| `__C_specific_handler` | scope range、filter、finally target、handler、continuation target | reducible region は `__try`/`__except`/`__finally`、それ以外は注釈 | 完全で表現可能な scope graph のネイティブ x64 再構築 |
| `__CxxFrameHandler3` | unwind/try map、catch、catch-object/frame offset、continuation、IP-to-state map | reducible state interval を明示的 C++ HighIR と C 互換型注釈に変換 | 後述する狭い verifier-clean subset のネイティブ x64 再構築 |
| `__CxxFrameHandler4` | action kind と object offset を含む bounded variable-length decode | FH4 provenance を持つ共通 HighIR graph | 解析のみ。対象関数の変更を拒否 |
| `__GSHandlerCheck_SEH/EH/EH4` | wrapped personality と検査済み GS cookie provenance | base language graph と wrapper 注釈 | 解析のみ。downgrade せず対象関数の変更を拒否 |
| x86 registration-chain EH | table-based EH と区別 | unsupported-form 注釈 | 再構築しない |

Malformed record を完全な通常 record として扱うことはありません。partial decode は調査に
使えますが、ネイティブ metadata 生成を許可しません。ARM xdata header から bounded な
executable fragment range を証明できても後続 unwind body が壊れている場合、range は
disassembly に残りますが record は malformed となり、patchable function にはなりません。

## 正規化モデル

`ExceptionInfo` は `BinaryImage` が所有します。各 `ExceptionFunction` は次を持ちます。

- 検査済み half-open code range。
- primary、chained、fragment の identity。
- native unwind encoding と正確な runtime/unwind provenance。
- 正規化 unwind operation/epilog。未解釈 operation の opaque operand bytes も保持。
- 正確な personality identity と handler data。
- 任意の SEH scope、C++ state map、GS cookie data。
- `Complete`、`Partial`、`Malformed` status と決定的 diagnostic。

loader はこのモデルから raw file pointer を公開しません。native RVA は診断と patch
replacement 用に保持し、IR consumer は検証済み VA/range だけを使用します。

image-wide index は chained/fragment record の overlap を許し、address を覆う最も具体的な
function を返します。破損 directory、range、pointer、count、state transition、compressed
integer、chain cycle、decode-budget exhaustion は該当 parse status を低下させます。

language-table limit は個別テーブルと関数全体の正規化 graph の両方に適用します。同じ
handler map を多数の try-map entry が再利用しても総予算を超えません。同じ `FuncInfo` と
personality を共有する FH3 record は bounded function group として decode され、親の
IP-to-state map は自身の catch funclet を参照できますが、無関係な runtime function の
address は受け入れません。

## IR 契約

例外 metadata は通常 CFG の意味を変えずに全表現へ渡されます。

- LowIR は protected-range endpoint、state transition、filter、handler、cleanup action、
  continuation target で block を分割。
- exceptional successor/predecessor は通常のものと分離し、dominator/structuring pass が
  runtime dispatch edge を machine branch と誤認しないようにする。
- MedIR は正規化 function descriptor と安定した exceptional edge を保持。
- HighIR は別々の `SEHTry`/`CxxTry` statement を使用。clause descriptor は native
  target VA、type descriptor、adjective、catch-object/parent-frame offset、cleanup
  action kind/object offset、state、continuation VA を保持。

HighIR structurer は interval-conservative です。完全な protected range に全 address が
含まれる連続 statement slice だけを動かし、nested region は内側から処理します。crossing
region、partial graph、address-less ambiguous boundary、out-of-line funclet は元の control
flow に残り、function の unstructured-EH count を増やします。

C backend は reducible な single-clause SEH region に MSVC SEH syntax を出力します。
HighC は C backend なので C++ catch/cleanup state は決定的な C-compatible comment とし、
compilable C++ を装いません。out-of-line native funclet は正確な address を保持します。

## LLVM metadata schema

emitted function に関連する解析済み例外 function は、native WinEH lowering 非対象でも
lossless LLVM metadata を受け取ります。

- function attachment：`neverd.windows.eh`
- native-lowering marker：`neverd.windows.eh.native`
- module table：`neverd.windows.eh.functions`
- current schema version：`3`

fixed function record は parse status、encoding、code range、native runtime/unwind RVA、
runtime-record kind/chain provenance、packed-unwind word、frame description、canonical/
resolved personality name、handler data、native unwind bytes、operation（native slot count
を含む）/epilog、SEH scope、C++ header/map、GS data、diagnostic、regeneration flag を保持。
patch validation は正確な schema version と loaded image との完全な range match を要求し、
例外契約を持つ auto-named lifted function は attachment を省略できません。

native x64 SEH lowering は LLVM WinEH を使用し、scope graph 全体が表現可能な場合だけ
verifier-clean `invoke`/funclet control flow を生成します。native FH3 lowering の条件は：

- x64 COFF、unwind v1/v2、complete metadata、有効な synchronous FH3 state graph。
- `noexcept`、asynchronous、separated-funclet、GS-wrapper、FH4、unknown flag semantics なし。
- protected interval は nested または disjoint で、crossing しない。
- destructor/unwind action、catch-object construction、parent-frame dependency なし。
- handler は lifted function 内の predecessor-free、call-free な通常 block。
- unwind し得る protected operation はすべて LLVM `invoke` で表現。

一条件でも偽なら LLVM は解析可能なまま lossless metadata を保持しますが、patch planning
は native language-table replacement を拒否します。PE entry point、TLS callback、CRT
callback root は preservation boundary であり、通常の ABI rewrite candidate ではありません。

## Patch transaction

サポートする rewrite は一つの PE transaction として処理します。

1. loaded exception graph と LLVM metadata attachment に対して各 touched function を検証。
2. section identity/alignment/allocation、code/data trait、semantic symbol-index reference を
   保持して replacement code を compile。local Windows personality は codegen 前に
   externalize し、emitted xdata を実証済み original executable handler に binding。
3. untouched runtime-function entry を保持し、各 touched primary function が置換する
   native closure（関連 chained record を含む）を削除。
4. generated code/xdata を relocate、generated/retained pdata を merge、begin RVA で sort、
   overlap を拒否。同じ personality class の generated runtime-function record が全
   redirected language-EH entry を覆うことを証明し、単一 replacement exception directory
   を install。
5. input CFG instrumentation mode を保持し、`.gfids` semantic reference と redirected entry
   を Guard CF table に merge。`.gehcont` reference を generated executable VA に解決して
   Guard EH continuation table に merge し、guard flag を保ったまま load-config を更新。
   unresolved helper は transaction を中止。CFW、return-flow guard、retpoline、XFG は
   異なる codegen contract が必要なため analysis-only。
6. disk へ書く前に完成 byte image を再 parse。

LLVM fork extension は汎用のままです。final-image writer が section trait と semantic
symbol-index reference を保持し、PE/MSVC decode、policy、directory merge、load-config
update、final validation は NeverD に残ります。

original Guard CF/EH continuation entry は original entry trampoline が有効な indirect
target のため保持されます。generated target は emitted code 内を指し、結果 table は
厳密な RVA sort を満たす必要があります。

## 最終 image validation

次の全条件を満たさない patched PE は拒否します。

- LLVM が bytes を COFF object として受理し、PE machine/class/section table、optional
  header directory bounds、image base/extent が一致。
- section raw/virtual extent が範囲内で、section range に overlap がない。
- exception directory が file-backed で image 内。
- runtime-function entry が sorted、nonempty、non-overlapping、fully executable。
- x64 unwind RVA/header/code array/version/flag/handler target が有効で、chain は bounded/acyclic。
- final import/export/COFF symbol を memory 内で再構築し、既知 SEH/FH3 personality と
  scope/state table を完成 bytes から再 parse。
- ARM runtime entry/xdata の version/range が有効かつ対応済み。
- guard flag が table を示す場合 load-config に Guard CF/EH continuation field が存在。
- guard pointer/count/stride が PE image と file の両方の範囲内で、entry は strict-sorted
  executable target。

失敗すると patch operation は中止され、best-effort image は出力しません。

## 対象検証

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

guarded x64 fixture は `/guard:cf` と `/guard:ehcont` で cross-assemble/link されます。統合
test は SEH scope/guard table、structured HighC、patched image の reload、table count/order/
executable target を検証します。

別の linked x64 FH3 fixture は同じ transaction で supported C++ closure を検査し、original
fixed table、HighC state annotation、personality binding、再構築 try/catch graph、reload 後
IP-to-state map を検証します。parser 変更時は共通モデルを使う ARM format case も実行します。

## ネイティブサポートの拡張

新しいネイティブ再構築対応には、同じ変更内で次が必要です。

- complete bounded parser と normalized-model invariant。
- HighIR/LLVM metadata round-trip coverage。
- 新たに受理する全 graph shape の verifier-clean native IR。
- 必要な emitted-section/semantic-reference retention。
- 正確な architecture/personality/version の linked PE fixture。
- exception-directory/load-config/final-image structural validation。
- 最も近い unsupported shape の explicit rejection test。

decode できるだけで allow-list を広げてはいけません。最終 linked image で runtime exception
behavior が保存されることが受理条件です。
