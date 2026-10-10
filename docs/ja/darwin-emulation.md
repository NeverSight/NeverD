**言語**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 1939e117643388dff149085b992e1ad646feefca63b630abec9b09122e15c2f1 -->

[← ドキュメント一覧](README.md)

# macOS と iOS のゲストプロセス環境

`lib/emulation/os/darwin/` は、ホスト CPU 転送層とは別に、制限を明示した独立 Mach-O プロセスをモデル化します。`NEVERD_ENABLE_CPU_EMULATION` を有効にします。Windows ドライバエミュレーションは不要です。`macos/` と `ios/` が明示的なプラットフォームプロファイルを定義します。

| プロファイル | Mach-O プラットフォーム | ゲスト ISA | OS ページ |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64、基本 ARM64 | x64 4 KiB、ARM64 16 KiB |
| `ios-macho64-v1` | iOS デバイス | 基本 ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64、基本 ARM64 | x64 4 KiB、ARM64 16 KiB |

デバイス用バイナリはシミュレーター用イメージではなく、ホストからゲストのプラットフォームを推測しません。macOS で ISA が一致すれば [HVF](macos-hvf.md)、異なる場合の `auto` は Unicorn を使います。CPU のマッピング単位は 4 KiB のままです。[C、Python、CLI API](process-emulation.md) はオプション、制限、レポートを共有します。

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## イメージと起動

`MachOExecutionImage` は解析による再配置パッチを適用せず元のバイト列を保持します。プラットフォームとエントリが一意な thin little-endian `MH_EXECUTE` のみ許可します。ユニバーサルイメージは必要なスライスを明示的に取り出してください。

メタデータと末尾バイトを含むファイル全体が、解析やコピーの前に `memory_limit` 内に収まる必要があります。通常ファイルから上限付きのプライベートスナップショットを読み、NUL を含むパス、短い読み取り、サイズ変更を拒否します。生きたファイルマッピングを保持しません。ファイルとゲストメモリは同じ値の別々の上限です。ホストのファイル I/O に厳密な実時間保証はありません。

セグメントは現在と最大の権限、ゼロ埋めを保持します。`__PAGEZERO` は広大な実メモリを割り当てずアドレスを予約します。ファイル/VM 範囲、OS ページ整列、丸め後の重複、ヘッダーの所属、実行可能エントリ、予算を確認します。ヘッダーセグメントは読み取りと実行が可能である必要があります。ガードページと専用の復帰ゲートは予約されたままです。最後のファイルページはページ境界または EOF まで元のバイトを保ち、後続の完全な VM ページをゼロ化します。[XNU ローダー](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c)に基づきます。

`LC_MAIN` は `argc`、`argv`、`envp`、apple ベクターを四つの整数引数で受け取り、戻り値の下位八ビットが終了ステータスになります。`/usr/lib/dyld` はインポートのないこのエントリ引き渡しに限り許可し、ホストの dyld は実行しません。非ゼロの `stacksize` は拒否します。予算を決めるのは呼び出し元の `stack_size` です。

`LC_UNIXTHREAD` は PC だけが設定された完全なネイティブ 64 ビット汎用レジスタレコードを一つだけ許可します。初期スタックは argc、終端付き argv/envp、`executable_path=<input filename>` を含む終端付き apple ベクターで構成します。独自 SP/フラグ、他のレジスタ、追加 flavor、矛盾するエントリを拒否し、ホスト環境や Linux 補助ベクターを継承しません。[dyld の設計](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md)を参照してください。

外部 dylib、インポート、rebases/chained fixups、コンストラクター/デストラクター、TLS セクション、arm64e/PAC、未対応 CPU サブタイプ、暗号化、未モデル化ロードコマンドは実行前に失敗します。fixup のない PIE は優先アドレスを使い、ASLR ではありません。署名 blob はメタデータであり、AMFI や entitlement ポリシーを実装しません。

## Darwin サービス

BSD 呼出しの ARM64 は X16、X0–X5 と `svc #0x80`、x64 は BSD クラス `0x02000000`、RAX、RDI/RSI/RDX/R10/R8/R9 を使います。成功は carry を消し、エラーは carry と正の errno を返します。ARM64 は X1 を消し、x64 は成功時に RDX を消してエラー時には保持します。SYSCALL が変更するレジスタは明示されています。レポートの `result` と `error=true` は BSD エラーを表します。戻らない要求や未対応要求には両フィールドがありません。XNU の [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c)、[x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c) の規則に基づき、Apple の実装コードは取り込んでいません。

サービスは `exit`、`write`、`getpid`、`getppid`、`getuid`、`geteuid`、`getgid`、`getegid`、`getgroups`、`mmap`、`mprotect`、`munmap` です。PID は1000、PPID は1です。UID/GID は既定1000で、下記の明示的な資格情報から実/実効IDを個別に指定できます。記述子 1、2 は NUL や非 UTF8 を含むバイトを捕捉し、閉じた記述子や読み取り専用記述子は EBADF です。部分コピー済みのバイトは保持しますが、後続の障害は EFAULT のままです。`INT_MAX` を超える長さは、記述子、ポインター、予算の確認前に EINVAL になります。[XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c)に基づきます。

メモリサービスは `flags=0x1002`、記述子 -1、オフセットゼロのプライベート匿名データマッピングに対応します。長さと非固定ヒントは OS ページに切り上げ、占有済みヒントでは上位アドレスを検索してから既定配置へ戻ります。従来の生 mmap は長さゼロで割り当てなしのゼロを返します。`MAP_UNIX03` に対応し、長さゼロは EINVAL です。Unmap/protect は整列済みアドレスを要求し、NONE/READ/WRITE に対応、WRITE は READ を含みます。物理所有は OS ページ単位なので部分解除で予算を解放し、新しいページはゼロになります。空白や最大権限境界を越える protect が失敗しても、範囲全体の元の権限を保持します。[XNU VM サービス](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c)を参照してください。

共有/固定/JIT マッピング、実行可能匿名メモリ、その他の Mach trap、間接システムコール、スレッド、シグナル、ホストファイル/ネットワーク、dyld、Objective-C/Swift runtime、Foundation/UIKit は対象外で、明示的に停止します。完全な Apple OS や iOS Simulator アプリケーションではありません。

## 検証

独自の C テスト入力を Clang と `ld64.lld` で生成し、Apple SDK や専有バイナリは使いません。五つのプラットフォーム/ISA、不正な Mach-O、4/16 KiB ページ、予算上限時の部分解放を検証します。`NeverDProcessPublicTests` が C API/CLI を比較し、`NEVERD_TEST_LIBNEVERD` と `NEVERD_TEST_DARWIN_FIXTURES` で Python SDK の同じ五つの組み合わせを有効にします。

## 明示的なファイル入力と記述子

`darwin_files` は3つのプロファイルに既定で読み取り専用の閉じたファイル一覧を提供します。必須の `files` は正規の絶対ゲスト `path` と16進数 `bytes_hex` を持ち、任意の `stdin_hex` は有限入力です。入力省略は未知で非ゼロ読み取りを停止し、空文字列は EOF です。一覧未指定の open は停止し、明示的な空一覧の未存在絶対パスは ENOENT を返します。ホストのファイルや入力は参照しません。

追加サービスは `open`、`read`、`pread`、`lseek`、`close`、`dup`、`dup2`、`fcntl` です。read/write/open/close/fcntl/pread の nocancel 入口も同じ実装を使います。O_RDONLY/O_CLOEXEC と F_DUPFD、F_DUPFD_CLOEXEC、F_GETFD、F_SETFD、F_GETFL を扱います。独立 open は別の位置、dup は共有位置と個別の close-on-exec フラグを持ち、pread は位置を変えません。0/1/2 の close・置換も後続 I/O に反映し、出力の複製は元の捕捉先と予算を保持します。

上限は256ファイル、パス/NUL/内容/入力の合計16 MiB、1024バイト未満のパス、255バイト以下の成分です。排他的上限 `descriptor_limit` は3–4096、既定256、JSON は64 KiBです。不正設定はロード前に拒否します。INT_MAX を超える read は FD 検査前に EINVAL、EOF は宛先に触れず、不正宛先は EFAULT です。部分的に書き込み可能な範囲はコピーや位置変更の前に停止します。SET/CUR/END の失敗は位置を保持します。旧 stat とその他 fcntl は未対応です。ファイルを祖先にすると ENOTDIR です。同じオブジェクトでネイティブ macOS と比較し、C/CLI/Python は5つのゲスト組合せを検証します。iOS 実機の証拠ではありません。

2026-10-05 の Release 検証は381項目中177成功、204スキップ、失敗なしで、ARM64 HVF 必須51/51を実行しました。ネイティブ macOS 7プログラム、公開 C/CLI・レポート35項目、Python の5ゲスト組合せ、検証スクリプト66項目も成功しました。件数は重複します。新しいファイルサービスの Intel HVF/KVM/WHP ネイティブ証拠はありません。Intel HVF は未検証で Actions を停止中です。iOS SDK と実機比較はありません。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 既存ファイルの変更

厳密な真偽値 `"writable":true`、または C++ の `DarwinFileOptions::WritableFiles` でプロセス内変更を明示します。省略・false は読み取り専用で、未知の許可は停止します。ホストや入力オプションは変更しません。write(4/397)、pwrite(154/415)、truncate(200)、ftruncate(201)、O_TRUNC は同じ内容ノードを使います。open の位置は独立、dup は位置と状態を共有し、最後の close 後も内容を保持します。拡張はゼロ埋め、切り詰めは位置を保持し、O_RDONLY|O_TRUNC も切り詰めます。

F_SETFL はネイティブフラグ変換後に O_APPEND|O_NONBLOCK のみを変更し、アクセスモード、close-on-exec、FWASWRITTEN を保持します。実際に非ゼロバイトを転送すると F_GETFL に 0x10000 が現れ、pwrite と出力捕捉も対象です。pwrite は append を無視して位置を保持します。INT_MAX 長さ検査は FD より先、pwrite の -1 はさらに先に EINVAL。INT64_MAX はゼロ書き込みより先に EFBIG となり、長さの制限後に追加位置を選びます。

成功した ftruncate は同サイズでも呼び出し元の open 記述と dup に FWASWRITTEN を設定します。O_TRUNC は O_RDONLY を含め新しい記述だけに設定し、パスの truncate は既存記述を変えません。

部分的に読める入力は効果の前に停止します。全体 EFAULT は内容を保持しますが、非空 append は位置を EOF に移します。転送失敗は内容・位置を確定しません。`mutation_policy` 未指定では、非ゼロ書き込み、切り詰め、非ゼロ全体 EFAULT は完全な stat 観測を無効化し、以後の stat は出力前に停止します。ゼロ書き込みは保持します。16 MiB はパス/NUL、入力、ディレクトリ記録、CWD、現在の内容と書き込みパス参照の合計論理予算です。縮小で backing を置き換えて容量を解放し、初期入力と有界の置換バッファは別に存在します。既知 inode 別名と immutable/append-only フラグは拒否します。

DarwinMemory の全マッピング区間を unmap するまで変更を拒否します。PROT_NONE と close 済み FD も含み、失敗・旧式ゼロ長マップはリースを残しません。新しいマップは現在の内容を使います。O_WRONLY の READ/WRITE mmap は EACCES、PROT_NONE は成功し後から mprotect で読み書きを許可できます。

元の通常/nocancel プログラムをネイティブと比較し、4K/16K 単体テストと C/CLI/Python の5構成を検証します。権限強制・ディレクトリ削除・異なる初期ディレクトリ領域間の改名・ハードリンク、実ファイルシステムのメタデータ更新、マップ整合性、EOF SIGBUS、完全な環境と iOS 実機は未完了です。Intel HVF は未検証、Actions は停止中です。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## 明示的な可変メタデータ

`writable: true` と完全な metadata に加え、ファイルに mutation_policy を設定できます。C++ は `DarwinFileOptions::MutationPolicies` を使います。これは明示的な仮想疎割り当て契約であり、APFS の推測やホスト時計の参照ではありません。省略時は変更後メタデータが未知のままです。

allocation_unit、mutation_time、seconds/nanoseconds は必須で、整数は既存の無損失規則を使います。単位は512バイト〜16 MiBの2の累乗で、block_size や VM ページとは独立です。通常権限（set-id/sticky なし）、flags=0、link_count=1、および初期密割り当て blocks=ceil(size/allocation_unit)*(allocation_unit/512) を要求します。ゼロ値から穴を推測しません。ポリシーのパス参照も16 MiB論理予算に含み、割り当て台帳から ENOSPC は推測しません。

書き込みは触れる全単位を割り当て、穴へのゼロ書き込みも対象です。truncate の拡張はゼロのみ追加し、縮小は切り上げ EOF より先の単位を破棄して末尾の部分単位を保持します。再拡張は破棄した割り当てを復元しません。非ゼロ成功書き込みと全成功 truncate（同サイズ、空 O_TRUNC も）は size/blocks と固定 mtime/ctime を更新します。他の値と初期入力は不変、read は atime を進めません。パス stat、別 open、dup、再 open は同じノードを参照します。

ゼロ書き込み、予算/マップ拒否、部分入力拒否、バックエンド失敗は既知状態を保持します。非ゼロ全体 EFAULT は未知状態にし、後の成功でも復元しません。stat 出力失敗はノードを変えません。virtual-file-metadata は5構成と C/CLI/Python で144バイト全体を検証します。割り当てはポリシーテストで、APFS 等価性の証拠ではありません。ネイティブは別にフラグ・位置・エラー順序を検証します。名前空間、実 FS 整合性、Mach、動的ランタイムは未完了です。

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## 疎ファイルの位置指定

通常ファイルに mutation_policy があり割り当てが既知なら、lseek は SEEK_HOLE=3 と SEEK_DATA=4 を受け付け、stat と同じ台帳を参照します。初回変更前はゼロ値も含む密割り当てです。指定種別の単位内なら入力位置、なければ次の一致単位の先頭を返し、終端の穴は EOF です。負値は EINVAL、EOF 以降（空ファイル含む）または後続データなしは ENXIO=6。失敗は位置を保持し、成功はその open 記述と dup のみ変更します。別 open は独立、再 open は現在の割り当てを見ます。メタデータ・フラグ・内容は不変、whence 上位ビットは無視します。

ポリシーなし、ディレクトリ、全体 EFAULT 後の未知割り当ては未対応です。ゼロや拒否された変更から割り当てを推測しません。独自 sparse-file-seek はネイティブ/ゲストでエラー、書いたバイト、EOF、記述の寿命を比較し、前方の FS 固有区間位置は仮定しません。virtual-file-metadata は厳密なポリシー配置を別途検証し、C/CLI/Python は5構成を確認します。

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 通常ファイル名の削除

ディレクトリの `mutable:true`（C++ `MutableDirectories`）は直下の名前変更を明示的に許可し、内容の `writable` とは独立です。権限がなければ未対応として停止。既知の非ゼロ flags、親の特殊アクセス権、子の link_count≠1、親または子の既知の別名を拒否します。stat とスナップショットの inode を共用し、明示的に異なるデバイスは区別。パスは既存の予算に計上されます。

`unlink(10)` / `unlinkat(472)` は既存の通常名を削除。通常ファイル削除の下位32ビットは0または `0x800` のみ対応し、未知ビットはパス/FDより先に EINVAL、AT_REMOVEDIR は後述の制限付き削除、DATALESS と SYSTEM_DISCARDED は未対応です。共通パス解決を使い、不在 ENOENT、ファイル末尾スラッシュ ENOTDIR、通常ディレクトリ EPERM、スラッシュのみのルート EISDIR、末尾が `.`/`..` のルート EBUSY。ネイティブで末尾 `.`/`..` も確認。

古い FD/dup/独立 open はデータ・カーソル・フラグを維持し、F_GETPATH は捕捉した旧パスを返します。新規 open は失敗し、暗黙の親と CWD は残ります。書込み権限はオブジェクトに属し、最後の記述子とマップ範囲の解放後だけ close/dup2/次の変更で現在のバイト予算を回収。初期パス費用は残り、権限強制・異なる初期ディレクトリ領域間の改名・ハードリンクは未完了です。初期ディレクトリの削除には後述の明示許可が必要です。

親の stat/readdir/SEEK_END は旧/新 FD とパス全体で未知となり、コピー/カーソル変更前に停止。read/pread は EISDIR、SET/CUR/F_GETPATH/fchdir/相対解決は継続。既知の変更ポリシーでは nlink=0、ctime=固定時刻のみ変更し、後の書込みでも nlink=1 に戻りません。ポリシーなし/EFAULT 後はメタデータ不明。失敗は状態を保持。元の `unlinked-file` はネイティブの名前/FD規則を比較し、時刻と親の失効は明示的モデル規則です。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## 通常ファイルの作成

O_CREAT=0x200 は明示的な mutable 親の直下に空ファイルを作成します。通常/nocancel open・openat は共通です。新規オブジェクトは書込み可能、既存は WritableFiles に従います。読取り専用 FD でも作成できますが書込みはできません。作成ポリシーがなければ stat64 と疎領域の検索は未知のままです。同名の旧 metadata/mutation_policy は常に継承しません。

O_CREAT と O_EXCL=0x800 の併用は既存ファイル・ディレクトリに切詰め前の EEXIST。O_EXCL 単独は無効です。既存ディレクトリの読取り専用 O_CREAT は成功します。以下の openat の先頭バイト・ディレクトリ事前検査後は、無効アクセスモード→FD 空き→O_CREAT|O_DIRECTORY の EINVAL→パスの順です。作成できるのは元パスの最後の欠落要素だけで、欠落祖先や末尾 `/`・`//`・`/.`・`/..` は ENOENT。新規 O_CREAT|O_TRUNC は FWASWRITTEN を設定せず、既存切詰めは設定します。

実際の挿入だけが親の観測を無効化。同名の新旧データ・FD・メタデータ・マップ寿命は独立です。256 項は初期非ファイル項と生存ファイルを数え、新しい正規パス/NUL とデータは 16 MiB に課金。削除後、最後の FD/マップ解放で動的費用を回収し、初期費用は保持します。予算超過や1024バイト以上の正規パスは明示的に停止し、ENOSPC やネイティブのパスエラーを捏造しません。失敗時は名前/FD を公開しません。created-file のネイティブ/5構成、4K/16K 境界試験が対象。権限強制・異なる初期ディレクトリ領域間の改名・リンク・ディレクトリ変更は残っています。

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## 明示的な作成メタデータとプロセス umask

任意の `darwin_files.umask`（C++ `InitialUmask`）は初期マスクを八進数 0..07777 で指定し、作成権限とは独立です。`umask(60)` は旧マスクを返し、入力の下位 07777 ビットを保存します。ゲストメモリも空き FD も不要です。省略は未知で、ホスト値や既定値を推測しません。一度だけ初期化し、将来の作成だけに影響し、呼出し元の入力を変更しません。下例の十進数 18 は八進数 0022 です。

`namespace_policy` を省略した場合、任意の `darwin_files.creation_policy`（C++ `CreationPolicy`）は新規オブジェクトの完全なメタデータを指定します。厳密なオブジェクトは `first_inode`、`block_size`、`generation`、`creation_time`、`mutation_policy` の5項目だけで、時刻と変更ポリシーには既存の形式を使います。明示 umask、1つ以上の mutable 親、全対象親の完全な metadata が必要です。block_size は 1..INT32_MAX、generation は uint32、割当単位は 512..16 MiB の2の累乗でブロックサイズ/VMページとは独立、ナノ秒は [0,1000000000) です。first_inode は非ゼロ uint64 で、別デバイスを含む全 stat/スナップショット inode より大きくします。JSON の正確な整数範囲を超える値は十進文字列を使います。

成功した新規挿入だけが全体共通の inode 列を進めます。UINT64_MAX を使うと永久に枯渇し、close/unlink/名前再利用/umask/後の検索でも戻りません。排他、FD、パス、項目数、バイト予算の拒否は名前・FD・採番を確定せず、既存 O_CREAT も消費しません。新 stat64 は直接の親の device/GID、選択したゲスト実効 UID（既定1000）、mode `S_IFREG | (mode & 0777 & ~umask)`、nlink=1、size/blocks/flags=0 を使います。ブロックサイズ、generation、4つの初期固定時刻はポリシー由来です。親の完全な stat/列挙が失効しても、不変の device/GID だけは使え、完全な記録は復元しません。

新ノードは独自のメタデータ/割当状態を持ち、旧同名オブジェクトを継承しません。write/truncate/unlink は変更ポリシーを共有し、inode/mode/birthtime と削除後 nlink=0 を保持します。全範囲 EFAULT 後は永久に未知、既存ノードには遡及適用しません。`created-file-metadata` は5構成でネイティブの権限、マスク返値、実効 UID、親デバイス/グループ、寿命を比較し、`virtual-created-metadata` は144バイト全体を別に照合します。ネイティブの4時刻は一致するとは限りません。固定時刻/疎割当は仮想規則で、権限強制、資格情報切替、ACL、ネイティブ APFS の動作は対象外です。

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 通常ファイルと作成ディレクトリの原子的交換

RENAME_SWAP=0x2 は renameatx_np で既存の通常ファイル同士、生きたプロセス作成ディレクトリ同士、またはファイルと作成ディレクトリを交換します。RENAME_NOFOLLOW_ANY を追加できます。明示的な初期ディレクトリは mutable:true と swap_rename:true を宣言し、C++ は DarwinFileOptions::SwapRenameDirectories を使います。作成した子孫は元の物体の能力を継承しますが、削除した名前の再利用は旧宣言を移しません。false や省略は不明です。同じ device や変更権限だけでは能力を証明できず、別の初期領域は未対応です。

`openat_nocancel`、`fstatat64` と `F_GETPATH=50` は同じファイル担当を利用します。交換後のパスと観測は各オブジェクトに属し、完全な stat の可用性は引き続きメタデータ契約に従います。

存在しない対象は末尾スラッシュがあっても ENOENT となり、ソース点/二点、領域、権限、能力より優先します。初期または削除済みディレクトリのオペランドは未対応です。許可された領域では、両方向の祖先交換とディレクトリ/子ファイル交換が EINVAL、通常ファイル対象の末尾スラッシュが ENOTDIR です。通常成分の同物体は名前空間の許可後に無操作となり、能力宣言は不要です。同物体のソース点/二点には不明な大小文字属性が必要です。RENAME_EXCL=0x4 + RENAME_SWAP=0x2 と未知 flags はパス入力前に EINVAL、SECLUDE は未対応です。

非空の両部分木は親物体をたどって移動し、FD/マッピングで保持された削除済みディレクトリや孤立ファイルも含みます。混合交換は正確なファイル根だけを移し、同名の古い孤立物体は自分の親に残ります。FD、dup、CWD、二点は物体と新しい親に従います。子ファイルの内容、識別、メタデータ、書込み権限、カーソル、flags、リースは保持されます。移動根と両直近親には既存の名前空間更新を適用し、根ファイルの設定ポリシーは自身の ctime を更新します。ポリシーなしや失効後の完全メタデータは不明です。

能力のパス+NUL は固定初期 16 MiB 予算に予約されます。両方向の全生存/保持パスを 1024 バイトと共有総量で事前確認してから、旧名前をまとめて外して新名前を公開します。根の削除や置換クレジットはなく、未オープン対象の内容も使えません。初期ファイルの最初の移動は動的パス費用を取得し、戻しても消さず、往復で累積しません。新規 FD、エントリ、作成 inode は不要です。失敗は両名前空間、親、カーソル、観測、マッピングを保持します。独自 SDK-free swapped-directory はネイティブ macOS と五つの C++/C/CLI/Python プロファイルでディレクトリ交換と混合両順序を比較します。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## 明示的なディレクトリスナップショット

`getdirentries64` (344) は既存の `directories` 項目の任意の不変 `contents` を列挙します。C++ は `DarwinFileOptions::DirectoryContents` を使用します。`entries` は `.`、`..` と全直接子項目を明示順序で含む完全な一覧です。空ディレクトリでも未指定は未知です。パスや stat 情報を作成せず、ホストを参照しません。

各項目は `name`、非ゼロ `inode`、`type`（0 未知、4 ディレクトリ、8 通常ファイル）、`next_offset`、`seek_offset` が必須です。型はパスと、同じ解決先の inode は他の一覧・メタデータと一致します。`next_offset` はディレクトリ内で一意の非ゼロ値で INT64_MAX 以下、昇順は不要です。ゼロは巻き戻しです。`seek_offset` は独立した符号なし64ビット d_seekoff 観測値で、ゼロの重複も可能です。整数は stat と同じ損失のない十進文字列規則です。

`contents.minimum_buffer_size` は EOF を含むペイロード最小値（1–128 MiB）として必須です。項目の任意の `minimum_buffer_size`（既定0）は、その位置から始まる読み出しをさらに制約します。例は APFS で最初のドット2項目に64バイト、EOF に1バイトを要した観測です。他の位置でも完全な1記録が必要です。LP64 記録長は `roundUp(25 + nameBytes, 8)`、8バイト整列です。全一覧合計4096項目まで、記録バイトも16 MiB予算に算入します。メタデータ/一覧だけを持つ祖先パスは重複なしで256パス制限に算入し、JSON は64 KiBまでです。

独立 open は独立位置、dup は共有位置です。ゼロか宣言済みの値だけで再開し、未知の位置は停止します。収まる完全記録を順に返します。要求長 >=1024 は元の末尾4バイトに EOF（末尾1、それ以外0）を予約し、記録部分だけを128 MiBに制限します。末尾アドレスは元の符号なし演算とラップを保持します。データ、位置更新、読出し前位置、フラグの順で処理し、後段 EFAULT は先行効果を保持します。EOF は空データをコピーしません。部分的に書込可能な個別コピーはその前で未対応停止し、以前の効果は保持します。

同じ `directory-entries` がネイティブ macOS と記録・dup/巻戻し・小分け読出し・EOF・コピー順序を比較します。別テストで SDK 配置と実記録の全バイト、長い名前を比較します。固定スナップショット値は巻戻しでも変わらず、APFS の動的世代は再現しません。旧 `getdirentries` (196)、変更後の列挙、他のネイティブバックエンド、iOS実機は未検証です。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

列挙検証（2026-10-05、Release）：Darwin 498件中246成功、利用不能バックエンド252件スキップ、失敗0。ARM64 HVF 必須63/63件を実行。ネイティブmacOS 11プログラム、C/CLI/レポート40件（スキップなし）、Python 5構成で各8ファイルワークロード、ランナー66件が成功しました。計数は重複します。証跡：`build-hvf-arm64/darwin-dirents-merged-evidence/`。Intel HVF Actions は停止中で、他のネイティブバックエンドとiOS実機は未検証です。

## プライベートファイルマッピング

`mmap` は通常のカタログファイルの `MAP_PRIVATE` に対応します。`flags=0x2`、または `MAP_UNIX03` を加えた `0x40002` を指定し、オフセットは OS ページに整列させます。要求長が短くてもページ内の元のファイルバイトを保持し、EOF 最終ページの残りはゼロです。書き込みは現在のマッピングだけを変え、元のファイル、他のマッピング、固定メタデータ、共有カーソルを変えません。close や記述子再利用後も有効です。読み取り専用や PROT_NONE の初期内容を保持し、`mprotect` で書き込みを許可できます。

ファイル終端の算術オーバーフロー、UNIX03 の長さゼロや未整列オフセットは FD 検索前に EINVAL、無効な FD は予算検査前に EBADF です。従来の長さゼロも FD を検査してから割り当てなしでゼロを返します。従来の未整列オフセット、ストリーム、空ファイルのページ、EOF を完全に超えるページは割り当て前に未対応として停止します。macOS は EOF 外のマッピングを作成できますがアクセスで SIGBUS を発生させるため、モデルは読み取り可能なゼロページやシグナル配信を捏造しません。共有、固定、実行可能、JIT マッピングは未対応です。

`DarwinFiles` が記述子とバイトを解決し、`DarwinMemory` が配置、権限、予算、ロールバックを管理します。入力は `darwin_files` のみです。同じ `file-mapping` プログラムがネイティブとゲストでコピー、close、カーソル、エラー、匿名ページ再利用を検査します。独立したネイティブ比較は非ゼロオフセット、ページ全体と SIGBUS 境界を検証します。

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### プライベートマッピングの検証（2026-10-05）

Release Darwin は438件の一意な登録を照合し、210件成功、228件スキップ、失敗ゼロでした。ARM64 HVF 必須57/57件を実行し、Unicorn は5種類のゲストを検証しました。ネイティブmacOSの9プログラム、非ゼロオフセットの全ページ比較、隔離子プロセスのSIGBUS確認が成功しました。公開API/レポート36件はスキップなし、Pythonの5組合せ（`file-mapping` を含む）、検証スクリプト66件、来歴回帰38件も成功しました。件数は重複します。証拠：`build-hvf-arm64/darwin-mmap-verified-evidence/`。Intel HVF/KVM/WHPやiOS実機の追加証拠はなく、Intel HVF Actionsは停止したままです。

## 明示的なファイルメタデータ

ファイル項目には `metadata` を追加できます。指定する場合、下の全フィールドが必須です。10進文字列は整数の全幅を保持し、JSON 数値は ±(2^53−1) 内の正確な整数に制限されます。device は符号付き32ビット、mode/link_count は符号なし16ビット、inode は符号なし64ビット、uid/gid/flags/generation は符号なし32ビットです。size はファイルのバイト数と一致し、blocks は符号付き64ビット上限以下、block_size は非負の符号付き32ビットです。時刻は符号付き64ビット秒と 0–999999999 ナノ秒を使います。

`stat64` (338)、`fstat64` (339)、`lstat64` (340) は ARM64/x64 で同じ144バイト LP64 レコードを返します。open とパス解決を共有し、FD の複製と close を反映します。FD やカーソルは変更せず、rdev、パディング、予約領域はゼロです。入力は初期メタデータを与え、変更は任意のポリシーに従います。read は時刻を更新せず、mode はアクセス許可を変えません。未指定メタデータ、ストリーム、旧 stat、拡張セキュリティは未対応です。パス/FD エラーを出力ポインターより先に処理し、部分的に書ける出力は変更前に停止します。ネイティブ試験は実ファイルの全バイトと SDK 配置を比較し、同じ独自プログラムで3呼び出しを検証します。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### メタデータ検証と残作業（2026-10-05）

stat64 追加後の Release 検証は409件：成功193、スキップ216、失敗0。必須 ARM64 HVF は54/54件実行し、Unicorn は5つのゲスト組合せを検証しました。SDK 配置と実レコード全体の比較、ネイティブ8プログラム、公開 API/レポート36件（スキップなし）、Python の5組合せ、検証スクリプト66件も成功しました。件数には重複があります。ネイティブ試験の出力をケース別に分け、短い出力に以前の末尾が残る問題を修正しました。Intel HVF/KVM/WHP と iOS 実機の追加機能の証拠はありません。

次は共有マッピングと EOF フォールト、有界書き込み（EOF ページ、close 後の寿命、エラー順序を検証）、明示的な時刻/システム情報、必要な Mach/スレッド、Mach-O 依存関係・再配置/バインド・初期化/TLS の順です。Objective-C/Swift と Foundation/UIKit は実行可能なネイティブ例で進めます。iOS 実機には SDK と端末が必要です。Intel HVF は未検証で Actions を停止したままです。



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

独立したワークロード検証は ARM64 111 件または x64 74 件をすべて要求し、各プラットフォームの `LC_MAIN` と `LC_UNIXTHREAD` を含みます。必須項目の欠落、スキップ、`ld64.lld` の不足はいずれも失敗です。

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Linux は `kvm`、Windows は `whp` を使用します。[Darwin ワークフロー](../../.github/workflows/darwin-native.yml) は Unicorn なしで両 x64 転送層を検証し、個別再実行も可能です。[カーネル参照](../../.github/workflows/darwin-kernel-reference.yml)は NeverD/LLVM なしで両 macOS ISA 上のプログラムを直接実行します。`DarwinNativeCases.def` がモード、終了状態、期待バイトを管理します。実際の dyld エントリ用に libSystem をリンクするのはホスト参照だけです。ISA 不一致、Rosetta、タイムアウト、結果の差異は失敗し、iOS 実機カーネルの証拠とは区別します。

## 証拠と残る範囲

2026-10-03 の結果です。重複する行は合算しません。

| 転送層 | ソース | 合格 | 失敗 | スキップ | ネイティブワークロード |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

[Intel 実行](https://github.com/NeverSight/NeverD/actions/runs/37106013999)は 286 件の CTest 識別子と 32 プロセスを元の XML と照合しました。234 件のスキップは、無効な Unicorn 65 件、ARM64 ゲスト 39 件、他のホストプラットフォーム 130 件です。成果物 `11267489438` の SHA-256 は `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef` と検証済みです。[KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) も独立に照合しました。[カーネル参照](https://github.com/NeverSight/NeverD/actions/runs/37064795867)は各 ISA で 4/4 プログラムが合格し、終了状態 37、完全一致の出力、空の stderr を確認しました。

Unicorn ありの C API/CLI は合格 138、スキップ 156、失敗ゼロです。Python は五つの組み合わせをカバーします。パッケージのエンジンは ARM64 CLI の 18 レポートと一致し、186 個の Mach-O 署名を確認しました。HVF/Unicorn OFF では 38 合格、231 スキップで、Hypervisor.framework をリンクしません。これらは統合証拠であり、追加のネイティブ実行数ではありません。Intel CPU 全体は未検証です。[HVF](macos-hvf.md)と[詳細記録](../darwin-emulation.md#hosted-native-verification-2026-10-03)を参照してください。

## 明示的な時刻の観測値

`ProcessOptions::DarwinTime` / `darwin_time` は、すべての Darwin profile で raw `gettimeofday` (116) に固定の観測値を渡します。第3出力 `mach_absolute_time` も含みます。`time_of_day`、`timezone`、`mach_absolute_time` はそれぞれ省略可能で、省略は不明、明示的なゼロは値です。空のオブジェクトは既定の時計を作りません。ホスト時計の参照、タイムゾーンの推測、時間の進行、絶対 tick の換算は行いません。

指定するレコードには全メンバーが必要です。`seconds` は符号なし32ビット、`microseconds` は [0, 999999]、`minutes_west` / `dst_time` は符号付き32ビット、絶対 tick は符号なし64ビットです。JSON は共通の無損失整数規則に従い、安全な整数範囲外では10進文字列を使います。不明なフィールド、範囲外の値、Darwin 以外の profile はイメージ読込前に拒否されます。

LP64 `timeval` は16バイトで、オフセット0がゼロ拡張した秒、8が32ビットのマイクロ秒、12の4バイトはゼロです。タイムゾーンは符号付き32ビット値2個、tick は8バイトです。暦時刻と絶対時刻は最初に同時採取するため、要求した両観測値がコピーやポインタ検査より先に必要です。その後 timeval、timezone、absolute ticks の順でコピーします。タイムゾーンの欠落や後続の EFAULT は、それ以前の書込みを保持します。重複アドレスも同順序です。個々の出力が一部しか書込み可能でない場合、そのコピー前に未対応として停止し、以前のコピーは残します。全ポインタが null なら設定不要で成功し、個別問い合わせは要求した値だけを必要とします。

独自の `time` ワークロードでネイティブ動作を確認し、`time-values` は5種類のゲストの C/CLI/Python で指定した32バイトを出力します。別の SDK オラクルは1回の raw ネイティブ呼出しで3出力を採取し、全バイトを比較します。時計進行、換算、commpage カウンタ、タイマー、Mach 時計オブジェクト/IPC は対象外です。dyld、スレッド、Objective-C/Swift、Foundation/UIKit は引き続き必要です。Intel HVF Actions は停止中で、ネイティブ Intel と実機 iOS の検証は追加していません。

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

時刻検証（2026-10-06、Release）：Darwin 登録538件、成功274件、利用不能バックエンドのスキップ264件、失敗0件。必須 ARM64 HVF は66/66件を実行しました。独自のネイティブ macOS 12プログラムと単一サンプルの SDK バイト比較も成功。公開 C/CLI/レポートは43/43件、スキップなし。Python は時刻の完全なバイト列と既存8ファイルモードを含む5ゲスト構成で成功。ランナー66件、翻訳、機能一覧、書式検査も成功。件数は重複します。証拠：`build-hvf-arm64/darwin-time-verified-evidence/`、`darwin-time-native-first/`、`darwin-time-public.xml`。

## Mach 時刻と戻り規約

`darwin_time.timebase` の `numerator` と `denominator` は非ゼロの符号なし32ビット値で、約分・換算せず保持します。`mach_timebase_info_trap` の番号89は ARM64 X16=-89、x64 RAX=0x01000059 です。分子・分母をリトルエンディアン8バイトで書き、ゼロを返します。完全に無効な出力先でもゼロを返しますが、一部だけ書ける出力はコピー前に停止し、転送自体のエラーは伝播します。未設定の timebase は null を含むポインタ検査より先に停止します。

ARM64 X16=-3 と X16=-4 は `mach_absolute_time` と `mach_continuous_time` の符号なし64ビット全体を返します。それぞれ自身の値だけが必要で、明示的なゼロも有効です。x64 の該当ネイティブ表項目は EXC_SYSCALL を起こすため未対応です。時刻進行、commpage、タイマー、Mach 時計オブジェクト/IPC は未実装です。

分派は番号の下位32ビットを使い、報告は元の64ビットを保持します。ARM64 の負数は Mach、x64 の Mach は0x01000000、BSD は0x02000000です。BSD 3/4 は read/write のままで、未知番号や別形式のクラスは停止します。解決済みの対応表が戻り規約を決めます。Mach はフラグと X1/RDX を保持し、BSD は既存の carry 規則に従います。x64 の RCX/R11 更新は残ります。Mach 報告は `result` を持ち、入力 carry にかかわらず `error` を省略します。

`mach-time` は ARM64 ネイティブでフラグ、次結果、上位番号ビット、不正ポインタ、BSD への遷移を比較します。`mach-timebase-values` は5ゲスト構成、`mach-clock-values` は ARM64 で正確なバイト列を検証し、SDK は配置と採取した比率を照合します。Intel HVF Actions は停止中で、x64 ソフトウェア・構文検査はネイティブ Intel や実機 iOS の検証を意味しません。

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach 検証（2026-10-06、Release）：Darwin 569件、成功293、利用不能バックエンド276件スキップ、失敗0。必須 ARM64 HVF 69/69を実行し、最終実行でネイティブ13ワークロードと2つの時刻 SDK 比較が成功しました。公開 C/CLI/report は100/100、スキップなし。Python は5ゲストを検証しました。公開比較はプラットフォーム・ワークロード別で、明示的な10秒の実行予算を使用します。製品既定値と期限回帰は変更しません。計数は重複します。

初回ネイティブ起動は既存の5秒制限を超え、独立計測で6.056秒、再利用時0.010秒でした。同じバイナリの再試行は元の制限で13件成功し、失敗記録も保持しています。ホスト負荷下のタイムアウト後、個別の逐次検証は成功しました。証跡：`build-hvf-arm64/darwin-mach-time-final-evidence/`、`darwin-mach-time-native-recheck/existing-binary-recheck.json`、`darwin-mach-time-public-accepted.xml`。コミット前の作業ツリーです。その時点では ARM64 MRS/MSR NZCV が未対応だったため、整数命令でフラグを観測していました。以下の変更でこの CPU 不足を解消します。

## ARM64 条件フラグレジスタ

共通の checked ARM64 契約は EL0/EL1 で正確な `MRS Xt, NZCV` と `MSR NZCV, Xt` の符号化を許可します。読み出すのはビット31–28のみで、書き込みも入力のその4ビットだけを使い、他は無視します。`XZR` への読み出しは破棄され、`XZR` からの書き込みは SP を読まず4フラグをクリアします。各バックエンドは元の命令を実行します。ホストのレジスタ設定検証と FPCR/FPSR の制限は変更せず、近隣の未登録システムレジスタは未対応のままです。

`NeverDAArch64NZCVTests` は全フラグ組合せをホスト命令と比較し、全スカラ/ベクトル状態、メモリ、レジスタ境界、監視停止/失敗、コンテキスト復元後の再試行、共有命令予算を検証します。ARM64 `mach-time` は SVC の前後で実際の MSR/MRS を使い、Mach のフラグ保持と BSD への遷移を確認します。ネイティブ HVF 必須項目には両権限の6メソッドとホスト比較を含めます。ARM64 KVM/WHP と実機 iOS は未検証です。書込み可能ファイル、システム情報、進行する時計、Mach IPC/スレッド、dyld/ランタイム/フレームワークと実機受入れは引き続き環境整備の対象です。

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


変更可能ファイルの検証（2026-10-06）：Release Darwin は610項目中322成功、利用不能バックエンド288スキップ、失敗0。ARM64 HVF 必須72/72を実行。最後に追加した EFAULT メタデータ検査を含む集中テストは114項目中102成功、12スキップ。ネイティブ15プログラムと公開 C/CLI/レポート111項目も成功しました。件数は重複します。最初のネイティブ実行で FWASWRITTEN の欠落を検出し、修正後に成功、失敗証拠は保持しています。期限は不変で、GitHub 全体 CI と iOS 実機は別途検証が必要です。Intel Actions は停止中です。

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python の初回全体テストで ARM64 の3ディレクトリ例がタイムアウトしました。同じ引数の診断で新規書き込み10/10は成功、iOS の1例は実時間5.005秒・CPU1.263秒で停止。5秒制限を変えない個別再検査は3例とも成功（2.43–3.17秒、10,941命令、出力65）。16論理CPUで負荷54–70はスケジューリング圧力を示しますが、安定した遅延の保証ではなく初回失敗も保持します。

最後の未変更 Python 統合メソッドは5構成すべて成功し、合計41.118秒でした。各プロセスの5秒制限は不変で、前の失敗・診断記録は別に保持します。


メタデータ検証（2026-10-06）：Release単体148件は124成功/24未対応バックエンドskip。全Darwin645件は343成功/300skip/既存ARM64 HVFディレクトリ2件timeout。同条件・元の5秒上限で20件を再検証し8成功/12skip、対象は3.818/3.949秒でした。必須HVF75件すべてに成功観測がありますが初回失敗は保存します。公開C/CLI/レポート117/117（Darwin73）、Python5構成27.359秒、ネイティブ15/15、runner66/66成功。割り当てはAPFS証拠ではありません。期限変更なし、全CI・Intel・iOS実機・完全な環境は未完了です。

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

疎ファイル位置検証（2026-10-06）：Release Darwin 671 件中 359 成功、利用不可バックエンド 312 スキップ、失敗ゼロ。必須 ARM64 HVF 78 件をすべて実行し、Unicorn は五つのゲストを検証。対象テストは 147 件中 123 成功、24 スキップ。ネイティブ 16、公開 C/CLI/report 122（Darwin 比較 78）、Python 五構成（12.344 秒）、runner 66 が成功。件数は重複し、期限は変更せず、過去の失敗も保存。証拠：`build-hvf-arm64/sparse-seek-validation-summary.json`。割り当て規則は明示的な仮想ポリシーであり、APFS 同等性ではありません。完全な CI と iOS 実機検証は別途必要で、Intel HVF Actions は停止中です。

unlink 検証（2026-10-06）：Release Darwin 708 件中 384 成功、利用不可324スキップ、失敗ゼロ。必須 ARM64 HVF 81 件すべて実行。対象156件は137成功/19スキップ、ネイティブ17/17、C/CLI/report128/128（Darwin83）、Python五構成16.268秒、runner66/66成功。独立設計・実装レビューに残る阻害事項なし。件数重複、期限不変、再試行不要。証拠：`build-hvf-arm64/unlink-validation-summary.json`。親の失効と固定時刻はモデル規則であり、完全なファイルシステム/ランタイムやiOS実機の検証ではありません。Intel HVF Actions は停止中、完全CIは別途必要です。

### 作成の検証、2026-10-06

Release Darwin は748登録、412成功、336バックエンド不在のスキップ、失敗なし。必須ARM64 HVF 84件を実行。重点162件は150成功/12スキップ。C/CLI/レポート133/133（Darwin88）、Python5構成9.982秒、ネイティブ18/18、runner66/66成功。初回ARM64は試験用ポインタ表の再配置を正しく拒否し、インラインバイトへ変更して成功。ローダーを緩和せず、初回失敗とバイナリを保存。runner期待数は27から28へ更新。独立レビューに阻害事項なし、親観測を保持する容量失敗も検証。件数は重複、時間制限は不変。完全なCIと実機iOSは別途、Intel HVF Actionsは停止中。

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### 作成メタデータ検証、2026-10-06

Release Darwin は登録 787 件：成功 439、利用不可バックエンドのスキップ 348、失敗 0。必須 ARM64 HVF 87 件をすべて実行。対象 151 件は成功 139、スキップ 12。公開 C/CLI/レポートは 145/145（Darwin 入力比較 98）、未変更の Python メソッドは5構成を 12.211 秒で成功。ネイティブ 19/19、検証スクリプト 66/66 成功。独立レビューに阻害事項なし。別親の device/GID と全体 inode、最初の書込み前 unlink、FD/入力が使えない umask を追加検証。件数は重複し、期限不変、失敗再試行は不要。固定作成/変更時刻と割当は仮想ポリシーです。完全 GitHub CI と iOS 実機は別途必要、Intel HVF Actions は停止中です。

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### 改名の検証、2026-10-06

Release Darwin は835件：成功474、利用不可スキップ360、既存 macOS ARM64 HVF 仮想メタデータ1件が5.087秒で期限超過。同じ引数・5秒制限の20件再確認は成功8、スキップ12、該当項目0.113秒。両実行で必須 ARM64 HVF 90件の成功観測を網羅しますが、完全ゲートの失敗記録は維持します。改名対象は42/54成功、12スキップ。C/CLI/レポート150/150（Darwin比較103）、未変更Pythonの5構成18.478秒、ネイティブ20/20、スクリプト66/66。独立レビューで入れ子のドット分類を修正し、4K/16K回帰は修正前失敗・後成功。以前の読取り専用 ftruncate テスト期待値を EINVAL に訂正。失敗とプローブ各版を保持し、件数は重複、期限は不変。完全GitHub CI・iOS実機は別途、Intel HVF Actionsは停止中。

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## 明示的なシステム観測値

`ProcessOptions::DarwinSystem` / `darwin_system` は、すべての Darwin profile の `sysctl(202)` と raw `sysctlbyname(274)` に固定観測値を渡します。各フィールドは省略可能で、未指定の値や一覧外のキーは未対応として停止します。ホストへの問い合わせや版・機種の推測はありません。厳密な JSON と C++ 検証は、読み込み前に不正値と Darwin 以外の profile を拒否します。

`os_revision` は符号付き 32 ビット、`cpu_count` は 1..INT32_MAX、`memory_size` は符号なし 64 ビットです。`max_files_per_process` は 0..INT32_MAX の四バイト int。その他のスカラー項目は最大 1023 バイト（`hostname` は 255 バイト）の NUL を含まない文字列で、明示的な空文字列も有効、結果は終端 NUL を含みます。観測値はスケジューリング、割り当て、記述子予算を変更しません。

| JSON フィールド | sysctl 名 | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |
| `max_files_per_process` | `kern.maxfilesperproc` | `1,29` |
| `hostname` | `kern.hostname` | `1,10` |

`hw.pagesize` は既存のゲストメモリ方針を使用し、通常8バイト、非 null 出力の容量がちょうど4なら4バイトです。旧 MIB `[6,7]` と `hw.pagesize_compat` は常に4バイトです。`hw.pagesize` の動的数値 OID は未対応です。`hw.memsize` も容量4では、64ビット値が符号付き32ビット値の符号拡張と一致する場合だけ縮小します。それ以外は ERANGE34 で出力と長さを保持します。

MIB 数は下位32ビットで2～12、名前長は64ビット全体で1024未満です。指定された全バイトを検査してから最初の NUL を解釈し、末尾の点を一つ除きます。空の名前は ENOENT、部分的に読める入力は未対応です。非 null `oldlenp` は副作用前に8バイト全体の読み書きが必要です。不正な長さポインタのネイティブ試験は期限内に戻らなかったため、明示的な未対応範囲とします。null `oldlenp` は容量0、null `oldp` はサイズのみの問い合わせです。`kern.hostname` 以外の短い領域では ENOMEM12、データは不変で長さ0です。データ EFAULT は元の長さを保持します。入力と容量の取得、データ、最後の長さという順序で、別名と後続の転送失敗時の既存コピーを保持します。

`hostname` はこの guest 呼び出し元に見えるバイト列を宣言します。ホスト照会、モバイル環境の `localhost` 既定値、entitlement 推定は行いません。省略は未知、明示的な空文字列は NUL 一つです。`kern.hostname` の非 null 出力が正の容量で不足する場合、容量ちょうどのバイトを末尾 NUL 付きで正常に返し、その容量を報告します。容量0は ENOMEM12、長さ0、データ不変です。null 出力は NUL を含む全長を報告します。検査は実際の出力範囲のみです。部分的に書ける範囲は未対応で、接頭部を公開しません。ネイティブの部分コピーはモデル外です。libc uname/gethostname が用いる raw 観測の追加であり、dylib インポートや完全なランタイムの実装ではありません。

newp/newlen が両方非ゼロなら書き込みです。名前/MIB と oldlenp の完全な読み書き事前検査を先に保ち、既定または明示的非root EUID は観測値・データ出力検査前に EPERM1。明示的EUID0 の kern.osversion / kern.maxfilesperproc / kern.hostname 特権書き込みは未モデル化のため unsupported；RUID は判断に使いません。他のネイティブ読み取り専用ノードはrootでも EPERM1。新長0はポインタを無視し、未知キー/ツリー/動的OIDの ENOENT は推測しません。

独自の `system-info` はネイティブ macOS とゲスト ABI を検査し、`virtual-system` は C++、C/CLI、Python で設定済みバイト列を比較します。別の SDK 検査はホストの九つの観測値を明示的なテスト入力として名前・数値出力を照合します。iOS 実機や Intel HVF の検証ではありません。

```json
{"darwin_system":{"hostname":"guest-node","os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/sysctl.3.html).

### システム問い合わせの検証、2026-10-06

Release Darwin は881登録、509成功、利用不可372スキップ、失敗なし。必須 ARM64 HVF 93件をすべて実行。重点49件は37成功・12スキップ。C/CLI/レポート163/163（Darwin 比較113）、未変更 Python の5構成15.302秒、独自ネイティブ21/21、検証スクリプト66/66が成功しました。独立レビューに阻害事項はなく、追加のエラー優先順位と SDK 捕捉比較も成功。新しい SDK 検査は StringExtras ヘッダー不足で一度コンパイルに失敗し、追加後に成功しました。元のログ・ソースと、未対応境界に置いた不正長さポインタのネイティブ記録を保持します。検証後は二つのファイル先頭コメントだけを整理して再ビルド成功。件数は重複、期限は不変、実行失敗の再検査は不要でした。完全 GitHub CI と iOS 実機は別途、Intel HVF Actions は停止中です。

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## ベクトルファイル入出力と出力捕捉

`readv`/`writev`、`preadv`/`pwritev` と nocancel 入口は、スカラーのファイル・出力捕捉実装を共有します。新しい設定やホストアクセスはありません。LP64 iovec は8バイトのアドレスと8バイトの長さです。iovcnt の符号付き下位32ビットは1–1024で、記述子検索より前に配列全体をコピーします。出力との別名でも要求は変化しません。部分的に読める配列は未対応です。

アクセス権とストリームの位置指定可否を先に検査し、その後、各長さと合計を INT64_MAX 以下に制限します。通常ファイル・ディレクトリの合計はさらに INT_MAX 以下です。有限 stdin は残りの入力に短縮し、捕捉は出力予算に従います。pwritev の負の位置は配列を読む前に拒否し、preadv の位置検査は記述子と長さの後です。空の項目はアドレスを無視しますが、記述子・種類・位置の検査は残ります。EOF 後の項目には触れません。位置指定ではカーソルを変えず、pwritev は追記を無視します。通常の追記は元のカーソルで要求全体を一度だけ短縮してから EOF を選びます。

後続の完全に無効な項目は EFAULT を返し、完了済みのバイト、通常カーソルの進行、非ゼロバイトを書いた場合の FWASWRITTEN を保持します。承認済みの非ゼロ書き込みがデータバッファの EFAULT を返す場合、完全なメタデータを無効化します。引数エラー、モデルの受付拒否、バックエンド障害では保持します。部分的に書ける読み取り先は UnsupportedService で停止し、現在の項目はコピーせず、前のコピーは残ります。部分的に読めるファイル書き込み元は全ファイル効果の前に未対応で停止します。権限・マッピングリース・合計保存予算を先に検査し、バックエンドの事前検査・読み取り失敗ではファイルや捕捉を公開しません。

捕捉は stdout/stderr 共通予算を先に検査します。ユーザーアドレス上限をまたぐ項目はコピーせず、先行項目だけ保持します。その他の部分入力は確認済み接頭部を保持して EFAULT です。スカラー範囲エラーは従来どおり予算より優先します。複製・転送記述子は元の出力先を保持します。独自 `vectored-io` がネイティブ macOS、5つのゲスト構成、公開 C/CLI/Python で8入口を検証します。キャンセル、パイプ、スレッド、実機 iOS の受け入れは含みません。

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### ベクトル I/O 検証、2026-10-06

Release Darwin は937登録、553成功、未提供バックエンド384スキップ、失敗なし。必須 ARM64 HVF 96件をすべて実行しました。対象検査は45/57成功、12スキップ。公開 C/CLI/report は168/168成功、うち Darwin 入力比較118件。Python は5構成を20.397秒で検証、ネイティブ22/22、検証スクリプト66/66成功。独立レビューで疎な位置指定書き込みの障害テストを追加し、カーソル、実際の EOF、メタデータ拒否、残存容量を検証しました。初回ビルドは旧テストの削除済み内部問い合わせ参照で失敗し、実出力の検証に変更しました。新イベント断言の optional<bool> 誤用で正常な8ゲスト実行が失敗扱いになりましたが、修正後すべて成功。両失敗のソースとログを保存しています。件数は重複し、制限時間は不変。完全な GitHub CI と実機 iOS は別検証、Intel HVF Actions は停止継続です。

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## ファイルの存在確認

`access(33)` と `faccessat(466)` は現在の仮想カタログを照会し、記述子の割り当てや内容・カーソル・フラグ・メタデータの変更を行いません。F_OK は既存の探索契約で名前の存在を確認します。メタデータはカタログのアクセス権を付与・撤回せず、祖先ディレクトリの検索権限、ACL、MAC のネイティブ検証ではありません。stat 観測が不明でも照会可能です。古い FD やマッピングが残っても削除名は ENOENT となり、作成・名前再利用・改名は現在の名前空間に従います。

モードは下位32ビットです。R/W/X はビット0–2、拡張権限は9–21を使用します。`(mode & 0x003ffe07) == 0` が存在確認で、符号を含む他のビットは EINVAL にせず無視します。権限要求は探索成功後に UnsupportedService となり、許可を示すようなメタデータや変更許可から推測しません。既知のパス・記述子エラーが先です。

Faccessat は下位フラグ AT_EACCESS(0x10)、AT_SYMLINK_NOFOLLOW(0x20)、AT_SYMLINK_NOFOLLOW_ANY(0x800) の任意の組合せを受け入れます。それ以外は、カタログ未設定でもパス・FD の前に EINVAL です。実/実効 ID は固定で、固定リンクは下記の規則で解決します。絶対パスは dirfd を無視し、相対パスは設定済み CWD/ディレクトリ FD に従います。非 AT_FDCWD の nameiat は1バイトを読んで相対ディレクトリ FD を検査した後、全文字列を読みます。`/` は FD 検査を省略。最初のバイト故障は EFAULT14、相対の不明/ファイル FD は後続故障より先に EBADF9/ENOTDIR20。空の相対パスでも未知 FD は EBADF、通常ファイル FD は ENOTDIR、それ以外は ENOENT。未設定カタログや未知のストリーム種別は未対応です。

独自 `file-access` はネイティブ macOS と5ゲスト構成、C++/C/CLI/Python で両入口、無視ビット、フラグと順序を比較します。NOFOLLOW_ANY は相対ディレクトリ FD を使い、ホスト `/tmp`、`/var` のリンクに左右されません。直接テストは名前の変更、混合権限ビット、FD 枯渇、メタデータ非依存とメモリエラーを確認します。

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### ファイル存在確認の検証、2026-10-06

Release Darwin は971登録、575成功、未提供バックエンド396スキップ、失敗なし。必須 ARM64 HVF 99件を実行しました。対象35件は23成功・12スキップで直接14件を含みます。公開 C/CLI/report は173/173成功、うち Darwin 入力比較123件。Python は5構成を16.235秒で検証、ネイティブ23/23、検証スクリプト66/66成功。独立した設計・実装レビューに阻害要因なし。ホスト /tmp のリンクによる初期 NOFOLLOW_ANY 結果と正規パス比較を保存し、共通ワークロードは相対ディレクトリ FD を使います。件数は重複、制限時間は不変、実行失敗の再検査は不要でした。完全な GitHub CI と実機 iOS は別検証、Intel HVF Actions は停止継続です。

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## ディレクトリの作成と削除

`mkdir(136)` と `mkdirat(475)` は明示的に変更可能な直接の親内に作成します。新しいディレクトリは名前空間変更権限を継承し、初期ディレクトリには個別の許可が必要です。既知の親デバイス/GID だけを継承し、完全な stat、サイズ、割当、時刻、列挙位置は推測しません。再利用したファイル名の古い観測は遮蔽します。`creation_policy` は通常ファイル専用で、配下のファイルは継承した親識別と既存の全体 inode 列を使用し、mkdir はその inode を消費しません。権限強制とネイティブなディレクトリメタデータは対象外です。

共通の要素別探索で、mkdir は末尾スラッシュだけが続く欠落名を作成できます。欠落祖先後の点/二点は ENOENT、ファイル祖先は ENOTDIR、既存名は EEXIST。相対 FD/CWD、絶対パスの FD 無視、文字列障害優先を維持します。空き FD は不要で、探索・許可・容量・転送の拒否は名前や親観測を変更しません。

`rmdir(137)` と AT_REMOVEDIR(0x80) 付き `unlinkat(472)` はこのプロセスが作った空ディレクトリを削除し、AT_SYMLINK_NOFOLLOW_ANY(0x800) も併用可能です。未知の下位32ビットは入力前に EINVAL、DATALESS と SYSTEM_DISCARDED は未対応。既知のパス/型/ルートエラーを保持し、removable 許可のない初期ディレクトリ削除は引き続き UnsupportedService。許可された対象の末尾点は EINVAL、リンク中のディレクトリからの二点・非空対象は ENOTEMPTY。ディレクトリ FD、dup、CWD は元のオブジェクトを保持し、削除を妨げません。unlink 済み通常ファイルの FD/マッピングは名前ではなく、親削除・再利用後も内容、inode、最終 F_GETPATH が残ることをネイティブ対照で確認します。

新規ディレクトリの正規パス+NUL と1項目を共通16 MiB/256項目予算に計上し、削除後に参照がなくなるとそれだけ返却します。孤立ファイルとマッピングは保持します。成功時だけ直接の親の stat/列挙観測を無効化し、新規ディレクトリの完全な観測は常に未知です。独自 `directory-mutations` はネイティブ macOS、5ゲスト、C++/C/CLI/Python で入れ子作成・改名・unlink・削除・孤立物体の再利用を比較します。 FD またはマッピングリースが保持する孤立ファイルも親ディレクトリを保持します。全ディレクトリ FD を閉じても親の現在パス/NUL と項目は残り、ファイル解放後に親鎖を回収します。リンク中の作成済み祖先を移動すると F_GETPATH が更新されますが、同名の置換物には再所属しません。

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### ディレクトリ変更の検証、2026-10-06

Release Darwin: 1,017登録、609成功、未提供バックエンド408スキップ、失敗なし。必須 ARM64 HVF 102件を実行。対象58成功・12スキップ、新規4K/16K直接26件を含みます。公開 C/CLI/report178/178（Darwin入力128）、Python5構成17.255秒、ネイティブ24/24、検証スクリプト66/66。独立レビューは予算、名前再利用、親の同一性、ファイルリース、ロールバックを確認。最初の成功後の追加ネイティブ検査で、スラッシュのみのルート削除は EISDIR、末尾点/二点は EBUSY と判明し、共通判定と共通テストを修正しました。以前の結果とソース/実行ファイルを保存。件数重複、期限不変、Intel HVF Actions停止継続。全GitHub CIと実機iOSは別検証。

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## 保持されたディレクトリ識別

削除後も FD/CWD は元の親チェーンを保持し、名前再利用で別のオブジェクトへ接続しません。点の open は独立カーソル、dup は共有カーソル、二点は元の親を使います。削除済みの通常子名は ENOENT。LOOKUP は保持された削除済み親をたどれますが、作成・削除・改名検索は ENOENT。改名の末尾点/二点はその要素より先に EINVAL、以前の祖先エラーが優先します。F_GETPATH は最後のパスを保持し、完全な stat/列挙は不明です。新規ディレクトリのパス+NUL と一項分は FD/CWD/旧子参照がなくなるまで課金し、close/dup2/CWD変更/変更受付が到達不能チェーンを回収します。初期入力とファイルリースは別です。独自 `deleted-directories` はネイティブ macOS と5構成で保持中削除、親と名前の再利用、検索意図、CWDだけの保持を比較します。 FD またはマッピングリースが保持する孤立ファイルも親ディレクトリを保持します。全ディレクトリ FD を閉じても親の現在パス/NUL と項目は残り、ファイル解放後に親鎖を回収します。リンク中の作成済み祖先を移動すると F_GETPATH が更新されますが、同名の置換物には再所属しません。

### ディレクトリ寿命の検証、2026-10-06

Release Darwin1,051登録、631成功、未提供420スキップ、失敗なし；必須 ARM64 HVF105件実行。対象98成功・12スキップ、初期直接64/64（新規14件と保持中削除更新）。C/CLI/report183/183、Darwin133件；Python5構成18.691秒、ネイティブ25/25、スクリプト66/66。追加ネイティブ改名検査で末尾点の順序を修正し、初期ソース/結果/スナップショットを保持。主エージェントが証拠を照合し、最終独立レビューは利用不可でした。件数重複、期限不変、完全CIと実機iOSは別、Intel HVF Actions停止継続。

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## 明示的に許可した初期ディレクトリの削除

ディレクトリ項目の厳密な真偽値 `"removable": true`（C++ は `DarwinFileOptions::RemovableDirectories`）は、名前空間上の識別対象が一つで、マウントではない通常のディレクトリを宣言します。ルート以外の明示的な初期 `directories` 項目と、変更を明示許可した直接の親が必要です。既知の特殊モード/フラグ、スナップショットを含む inode 別名、親子の既知デバイス番号の矛盾は拒否します。番号の一致だけではマウント不在の証明になりません。省略/false は未対応のまま、他の JSON 型は無効です。一般的な権限やマウントのモデルではありません。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

削除には現在の名前空間が空であることが必要です。初期の暗黙的な子ディレクトリは、最後の元ファイルを unlink しても残ります。成功すると対象と直接の親の完全な stat/列挙は不明になりますが、旧 FD/dup/CWD は元のオブジェクトと親チェーンを保持します。不変入力から削除名を復活させません。同名の新しいファイル/ディレクトリは別の識別対象で、旧メタデータやスナップショットを継承せず、呼出元の入力も変更しません。

各 removable 参照のパスと NUL は初期 16 MiB 費用に含まれます。初期項目、パス、参照、スナップショットの費用と256項目上限の初期枠は、削除や最後の close でも返却しません。新しいオブジェクトには独自の動的費用を適用します。独自の `initial-directory-removal` はネイティブ試験の既存空ディレクトリを保持中に削除し、同名ファイル、次いでディレクトリを作成して CWD 単独保持を確認し、空ディレクトリを復元します。同じプログラムを C++/C/CLI/Python の5構成で実行します。

### 初期ディレクトリ削除の検証、2026-10-06

最終 Release ソースで Darwin 登録 1,089 件を照合し、657 件成功、バックエンド利用不可によるスキップ 432 件、失敗 0 件となった。必須 ARM64 HVF 108 件はすべて実行した。重点検証は 27/39 件成功、12 件利用不可スキップで、追加のスナップショットのみの別名検証も成功した。公開 C/CLI/レポートは 191/191、Python は 76.276 秒で 5 組合せ、独自ネイティブ負荷は 26/26、証拠ランナーは 66/66 成功。初回の inode/スナップショットが矛盾したテスト入力は修正し、失敗記録を保存した。

先行する完全検証 2 回では既存のファイル/rename ケースに 1 件と 3 件のタイムアウトがあり、診断付き実行でも実時間 5.008 秒、プロセス CPU 時間 0.171 秒のファイルタイムアウトを再現した。同一メソッドと旧プログラムの比較は成功したが、遅延原因は未解明であり、最終成功はタイムアウトの安定性を証明しない。一時診断を除去し、フィクスチャのハッシュを復元し、元のゲスト 5 秒制限を維持した。主担当によるソース/証拠監査を完了し、独立レビューは利用不可だった。件数は重複する。完全 GitHub CI、実機 iOS、停止中の Intel HVF Actions はこのローカル受入範囲外である。

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## 通常ファイルの排他的な改名

ソースとターゲットの検索後、RENAME_EXCL は別の既存ファイルまたはディレクトリに EEXIST を返し、マウントや名前空間変更の検査に先行します。末尾ドット/二重ドットの EINVAL など先のパスエラーは優先されます。ターゲットがなければ既存の有界改名トランザクションを使用し、保持された記述、カーソル、フラグ、マッピングリースと設定済みメタデータ遷移を保ちます。同一オブジェクトへの排他的改名は、ファイルシステムの大文字小文字区別に依存するため明示的に未対応です。正確なカタログキーからこの性質を推定しません。大小文字の同一視、初期ディレクトリ移動、SECLUDE は範囲外です。既存の独自 `renamed-file` は拒否時のメタデータ保持と EXCL|NOFOLLOW_ANY の成功をネイティブ macOS と C++/C/CLI/Python で比較します。

検証、2026-10-06（Release）：Darwin 登録 1,097 件、成功 665 件、利用不可スキップ 432 件、失敗なし。必須 ARM64 HVF 108 件すべて実行。重点検証は成功 44 件、利用不可 12 件で、新規直接検証 8 件を含む。公開 C/CLI/レポート 191/191、Python 5 組合せ 19.241 秒、独立した生呼出しプローブ 26 件成功。最初のネイティブ全体実行は既存 return がタイムアウトし、renamed-file を含む他の 25 件は成功。同じ未変更バイナリの return 再確認 3 回は 0.014–0.034 秒、その後全 26 件が元の 5 秒制限で成功した。初回失敗は保存し原因は未解明で、以前の最終 HVF 成功を含め遅延安定性の証明ではない。主担当監査済み、独立レビュー利用不可。件数は重複する。実機 iOS、完全 GitHub CI、停止中の Intel HVF Actions はローカル受入範囲外。

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## 作成したディレクトリ間の改名

初期ディレクトリと、このプロセスが mkdir/mkdirat で作成した子孫は同じ仮想名前空間領域を共有します。`rename`、`renameat`、`renameatx_np` はこれらの親間で通常ファイルを移動できます。新しい JSON フィールドは不要です。変更可能な初期 `/work` 内に `/work/left` と `/work/right` を作成すれば、`/work/data` を各子へ、また子間へ移動できます。別途初期入力で宣言した `/work/left` は device が一致しても別領域です。一般的なマウント構造は不明のままです。

両方の直接親に名前変更の許可が必要で、作成したディレクトリは許可と既知 device/GID を継承します。ファイル自身の識別子、所有者/グループ、書込み許可、割当は保持されます。既知 device の矛盾は拒否します。実移動は両親の完全な stat/列挙を無効にします。削除した初期ディレクトリと再利用パスは別物体で、古い FD/CWD は新しい領域を得ません。

既存の有界置換、マップ寿命、パス/NUL 費用、エラー順序が適用されます。EXCL の異なる既存ターゲットは領域/許可検査前に EEXIST です。`renamed-file` は作成した子への移動、初期親への置換、子への再移動を C++/C/CLI/Python とネイティブ macOS で比較します。権限強制、初期ディレクトリ移動、ハードリンク、動的シンボリックリンク、ネイティブ APFS メタデータは未完了です。

### 異なる親ディレクトリ間の検証、2026-10-06

最終 Release 検証では Darwin の登録 1,115 件を照合し、683 件成功、バックエンド利用不可で 432 件スキップ、失敗なし。必須 ARM64 HVF 108 件をすべて実行した。直接検査は新規 4K/16K 18 件を含め 56/56 成功。公開 C/CLI/report は 191/191、Python メソッドは 22.254 秒で五つの環境を検証した。元のネイティブプログラム 26/26、独立した生システムコールプローブ 34 件、文書/機能/証跡ランナースクリプト 296/296 が成功。件数は重複する。MSVC のみに適用する CMake 変更後も、検証用バイナリ十個のハッシュは変わらなかった。

以前の全体検証二回では既存 HVF ファイルメソッドのタイムアウト三件と二件を保持している。メソッド全体、作業ディレクトリ、セッションの比較は成功したが原因は確定しておらず、最終成功は遅延の安定性を証明しない。プローブは当初 /tmp と正規化された /private/tmp を比較していたため、root FD からパスを取得して四つの期待値を修正した。拡張ネイティブプログラムの後処理は mkdir(136) を誤用しており、rmdir(137) で exit150 を修正した。元のソースと失敗を保存し、ゲストの 5 秒制限は変更していない。

Linux 全体 CI が検出した LP64 初期化リスト四箇所は明示的 uint64_t に修正し、MSVC の NeverDJumpTableTests に /bigobj を追加した。実際の Linux/Windows コンパイルは CI 待ち。以前の全体 CI の Windows EH コーパスと閉じた PR のキャンセル失敗は別の問題として残る。主エージェントによるソース/証跡自己レビューのみ実施し、独立レビュー、実機 iOS、停止中の Intel HVF 検証の成功は主張しない。

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## 通常ファイル名の原子的交換

RENAME_SWAP=0x2 は renameatx_np で既存の通常ファイル二つの名前を交換し、RENAME_NOFOLLOW_ANY を追加できます。明示的な初期ディレクトリに mutable:true と swap_rename:true の両方が必要です。C++ は DarwinFileOptions::SwapRenameDirectories を使用します。作成された子孫は元のディレクトリオブジェクトの能力を継承します。削除後のパス再利用は古い宣言を移しません。false または省略は能力不明で、同じ device や名前空間の許可だけでは対応を証明できません。異なる初期領域は引き続き未対応です。

両パスは既存のコンポーネント解析器を使います。ターゲットがなければ領域・許可・能力の検査前に ENOENT です。ディレクトリオペランドは明示的に未対応です。ネイティブ swap はファイルとディレクトリも交換できるため、通常改名の EISDIR は適用しません。同一オブジェクトは名前空間の許可後に無操作となり、能力宣言がなくても状態は変わりません。RENAME_EXCL=0x4 + RENAME_SWAP=0x2 と未知 flags はパス入力前に EINVAL、SECLUDE は未対応です。

両ファイルはリンクされたままです。各自の識別、所有者/グループ、バイト、書込み許可、開いた記述、カーソル、flags、マッピングリースを保持します。設定済み仮想ポリシーは各自の ctime だけを更新し、ポリシーが欠落または無効なら完全なメタデータは不明のままです。実際の交換は両親の完全なメタデータと列挙観測を無効にします。作成 inode、項目、FD を消費せず、呼出元の入力も変えません。

能力参照ごとのパス+NUL は固定の初期16 MiB予算に予約します。トランザクションは両方の完全な動的名前費用を検査してから公開し、まだリンクされたバイトやリースを置換回収に使いません。反復交換では動的費用を再利用します。独自の renamed-file は作成した子への交換と逆交換で両オブジェクトを検査した後、通常置換を続け、ネイティブ macOS と全 C++/C/CLI/Python プロファイルで実行します。権限強制、マウント構成、大小文字の同一視、初期ディレクトリ移動 は別作業です。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

[Apple volume swap capability](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2), [XNU rename](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

### 交換の検証、2026-10-06

Release Darwin は1,133登録、701成功、後端利用不可432スキップ、失敗なし。必須ARM64 HVF108件をすべて実行しました。直接改名検査68/68、うち新規オプションと4K/16Kの18件。公開C/CLI/レポート192/192、Pythonの一つのメソッドは16.153秒で五つの構成を検証しました。独自ネイティブプログラム26/26、独立した生呼出しプローブ45検査。件数は重複し、ゲスト期限は不変です。

初期の直接テスト二件は未知の書込み許可と変更後メタデータに誤った期待を持ち、期待だけを修正しました。最初のJSONフィルターは0件を選び、検証に数えません。その後、正しいテスト所有者と完全な公開検査は成功しました。初期ソースと結果を保存しています。過去のHVF/ネイティブ遅延は未解明で、今回の成功は安定性を証明しません。ソース/証拠の自己レビュー済みですが、独立レビュー、iOS実機、完全GitHub CI、停止中Intel HVFの受入れは主張しません。

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## プロセス作成ディレクトリの改名

通常の rename、renameat、renameatx_np は同じ初期ディレクトリ物体の領域内で、リンクされたプロセス作成ディレクトリとサブツリーを移動できます。両方の直接の親に許可が必要です。未存在のディレクトリ先は末尾スラッシュを許可し、通常ファイル先は ENOTDIR、非空ディレクトリは ENOTEMPTY、子孫への移動は EINVAL です。許可済みの通常同名操作は無変更です。EXCL の別既存先は種類・循環・領域・許可より先に EEXIST。先の検索エラーがソースの点/二重点より先です。同物体を点/二重点で指定する場合は大小文字特性が未知なので UnsupportedService。初期/削除済みソース、初期置換先、別初期領域、リンク、権限強制、SECLUDE は未対応です。

親物体の鎖から子孫を選び、名前付きツリー、FD保持の削除済み子ディレクトリ、FD/マップ保持の孤立ファイルのパスを更新します。ソース FD、dup、CWD、二重点は同物体と新しい親を参照します。置換された空の作成ディレクトリは旧パスと旧親を保持し、普通の子は ENOENT、点/二重点/CWD は旧物体を保持します。新ソースを再移動しても同じパス文字列の旧先の孤立ファイルは移動しません。

子ファイルのデータ、識別、メタデータ、書込許可、共有/独立カーソル、FDフラグ、マップ寿命は維持します。ソースと両親の完全 stat/一覧は無効となり、子ファイルは祖先移動だけでは無効になりません。作成ディレクトリの名前空間許可は将来の作成/許可済みファイル改名にも残り、新項目、FD、作成 inode は不要です。

公開前に全生存/保持子孫の新キーとパスを確保し、各正規パス/NUL の1024バイト境界と共有16 MiBを確認します。旧動的費用は一度だけ置換。FD/CWD/保持子孫のない空の作成先だけが容量回収を提供し、一度だけ回収します。拒否は全名前、親、観測、カーソル、マップを保持します。マップだけの孤立ファイルも削除済み親のパス/項目費用を保持します。

独自 SDK-free `renamed-directory` をネイティブ macOS と5ゲスト構成の C++/C/CLI/Python で比較します。4K/16K は物体再利用、全体ロールバック、厳密容量、長い子孫パス、先の回収、マップ保持親の回収、項目/FD/inode枯渇を検証します。


### 2026-10-06

最終 Release Darwin は1,175登録：731成功、444利用不可スキップ、失敗0；必須 ARM64 HVF111件すべて実行。集中32/44（12スキップ、新規4K/16K22件）、最終境界/既存試験4/4。公開 C/CLI165/165、報告32/32でスキップなし。Python5構成21.759秒、ネイティブ27/27、独立プローブ79件。独立審査でルート区切りの試験不具合と同物体ソース点のFS属性境界を修正確認。初期ゲストexit124と古い期待値2件の失敗、元ソース/バイナリを保存し、最終全体検証は成功しました。件数重複、時間制限は不変。以前のHVF/ネイティブタイムアウトは未解明で、遅延安定性を証明しません。Intel HVF Actions は停止、iOS実機と全GitHub CIは別検証です。

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### ディレクトリと異種交換の検証、2026-10-07

Release Darwin の登録 1,219 件を照合し、763 件成功、利用できないバックエンドによるスキップ 456 件、失敗ゼロとなった。必須 ARM64 HVF 114 件はすべて実行された。重点検証は 32/44 件成功、12 件スキップで、新しい 4K/16K 直接検証 24 件をすべて含む。ファイル担当全体は 317/317 件、ネイティブプログラムは 28/28 件成功。独立した生システムコールのプローブは、この大文字小文字を区別しない macOS ファイルシステムで 32 件の成功した観測を記録した。件数には重複があり、ゲストの期限は変更していない。

独立した計画・実装レビューで、双方向トランザクション、初期ファイルのパス課金、両側の保持された部分木、マッピングだけで保持する孤立オブジェクトと正確な回収を確認した。最初の失敗テストは明示的なルート交換宣言を欠き、受け入れ対象から除外した。修正した旧実装の基準では選択した 6 件すべてがディレクトリ交換の拒否で失敗した。異種ファイルの初期アサーション 2 件は設定済みメタデータを誤って破棄しており、現在は ctime だけが変わる完全なレコードを比較する。ローカルの定数ポインタ配列が ARM64 リベースを導入して 6 件のロード拒否を起こした。4 件のスカラーアサーションへの置換後は従来型リベースがなく、ローダーの未対応 fixups 拒否境界は維持される。

修正済みプログラムの最初の重点実行では、5 秒の HVF タイムアウト 3 件を保持した。単独実行と 3 プロファイルの対照確認、その後の完全検証は成功したが、原因は不明であり遅延の安定性は証明しない。初期ソース、バイナリ、失敗と対照記録を保持している。初期ディレクトリ移動、別の初期ドメイン、権限・マウント・大文字小文字の宣言、動的依存とフレームワーク実行環境は未完了。実機 iOS、停止中の Intel HVF と全 GitHub CI は別の受け入れ境界となる。

`build-hvf-arm64/directory-swap-research/`: `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/`, `hvf-controls.json`.

最終リンク済みバイナリで完全 Darwin 検証を再度成功させた。固定 dev 0a9a1d28d の統合検証は共通担当 20 組の 4,515 件を記録し、4,489 件成功、任意 Z3 6 件と利用できない Windows EH コーパス 20 件をスキップ、失敗ゼロだった。この数に C/CLI 170/170 とレポート 32/32 を含む。Python は 30.780 秒で全 5 プロファイルを検証。ネイティブモバイルの全 12 アーキテクチャ/fixup 変種で 4,532 件の結果が元のプログラムと一致し、単一セッションと Swift メタデータ全体の一致は 12/12。Swift witness ジェネレーターもコメントの字下げ修正後に記録済み SDK/コンパイラでカタログを再現した。これはローカル Release LLVM 23/Apple Clang 17 の受け入れであり、後続 dev や Linux Clang 18 の受け入れは意味しない。


## 明示した初期ディレクトリ部分木の通常移動

明示的な初期非ルートディレクトリの厳密な Boolean `"movable": true` は、その根の通常 rename を許可し、初期部分木全体を一意の名前を持つ通常の非マウントディレクトリと宣言します。C++ の `DarwinFileOptions::MovableDirectories` は既存の集成体メンバーの末尾に追加されます。直接の親は mutable が必須です。省略・false は未知、他の JSON 型は無効です。子孫の mutable/removable/movable・ファイル書き込み権限は別々に維持します。初期ディレクトリ SWAP、権限判定や一般的なマウントは許可しません。既知の flags、特殊なディレクトリモード、複数リンクの通常ファイル、stat/スナップショットの inode 別名を拒否します。宣言で結合した非マウント領域全体の既知デバイス番号は一種類までです。stat がない祖先の兄弟部分木やファイルも含み、番号の一致だけでは別領域を結合しません。

名前・親・元の stat/スナップショット・権限はオブジェクトが保持し、古い入力パスから移動済みや削除済みの名前を再生成しません。未オープンの子孫、FD/dup/CWD、マッピング、削除済み子孫も元のオブジェクトを保持します。未変更の子孫は元の stat、スナップショット、cookie、SEEK_END を維持し、移動根と変更された親の完全な観測は未知になります。旧名の再利用に観測や権限は継承されません。初期入力は一回の同期実行の宣言であり、実行中のカタログ置換 API ではありません。SWAP は別の能力です。初期オブジェクトの直接 swap_rename 宣言を保持し、mkdir は親の能力をコピーし、移動では再計算しません。両方の SWAP 親に支持宣言が必要です。

空の初期ターゲットの置換には別の removable が必要です。全パスの 1023 バイト上限と共有 16 MiB を公開前に確認します。movable 参照は元のパス＋NUL を固定予約し、エントリを追加しません。初期ディレクトリのパス・参照・スナップショット・エントリは削除後も固定予約されます。動的 PathCharge はゼロから始まり、リンク中・保持中の各メンバーの現在パスを移動時に一度課金します。即時解放できる置換先の既存動的料金のみを差し引けます。FD/CWD/子オブジェクト/孤児マッピングの保持は信用を供給せず、最終回収は一度だけ返金します。暗黙の初期祖先は 256 エントリ制限に追加されず、初期通常ファイルの回収は従来どおりです。

例：

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

SDK 不要の initial-directory-move は移動、カーソル共有と独立、FD flags、CWD、私有マッピング、置換、元の名前の復元を確認します。ローカル guest 設定の検証と物理 iOS の比較は別です。

初期ディレクトリの SWAP 未対応は、システムコールの交換元または交換先となる根オブジェクトを指します。作成した祖先を交換する場合は、その配下に移動済みの初期の子孫を含め、オブジェクト状態と動的費用を維持します。前述の初期ディレクトリの制限は、必要な宣言がない場合に適用されます。

最終独立レビュー後の ARM64 macOS 検証は Darwin 1,264 件中 796 成功、利用不能バックエンドによる 468 スキップ、失敗ゼロです。必須 ARM64 HVF 117 件はすべて実行済み。ファイル所有層は 342/342（新しい 4K/16K 動作 22 例、受入れ条件 3 件を含む）。CreationPolicy は移動した親の元 Device/GID を維持し、名前再利用、inode 順序、現在の umask を区別します。厳密な 16 MiB 上限で 16 往復の交換は費用を蓄積せず、戻す際は実際の 6 バイト差分だけを解放します。公開 C/CLI 175/175、レポート解析 33/33、ネイティブカーネル 29/29、独立の原始プローブ 19 観測が成功。Python の 5 設定は 27.865 秒で成功し、純粋 API 71 件、SDK 差分、runner 49 件も成功しました。件数は重複します。ソース、バイナリ、失敗試行、最終結果は build-hvf-arm64/initial-directory-move/ に保存し、コミットに結び付けます。実機 iOS、停止中の Intel HVF、初期根 SWAP、権限・マウント・大文字小文字、共有マッピング EOF、Mach・スレッド・dyld とフレームワーク実行環境は別の未完了範囲です。

## 宣言した初期ディレクトリ根の原子的交換

厳密な Boolean exchangeable:true（C++ DarwinFileOptions::ExchangeableDirectories、集約末尾に追加）は、明示した非ルート初期ディレクトリを RENAME_SWAP の根オペランドとして許可します。直接の初期親は mutable 必須。省略/false は未対応、他の型は無効です。movable と通常の非マウント・一意名の子孫宣言、flags/特殊モード/別名/ハードリンク/接続成分全体のデバイス検証を共有しますが、宣言の和集合はトポロジーだけを定めます。各参照は元パス+NUL を固定計上し、両方を宣言すれば両参照を計上します。エントリは増えず、デバイス一致だけで領域は接続しません。

通常/EXCL の初期交換元は movable、通常の初期置換先は removable が別途必要。exchangeable はそれら、子孫 mutable、書き込み、一般の権限やマウントを許可しません。異なるオブジェクトの交換では、実際の両親が mutable かつそれぞれ swap_rename 対応である必要があります。許可済みの通常要素で同名 SWAP する場合は親/デバイスを確認後、初回課金も別オブジェクト用交換能力も不要で無変更です。同一オブジェクトの dot/大文字小文字は不明のまま、欠落先と dot の順序を維持します。

初期の非空根同士、初期/作成根、ディレクトリ/ファイルを両方向に交換できます。両側のリンク済み・保持中の木を全検証して全名を取り出してから公開し、両根はリンクを維持します。置換や内容の控除、FD/inode/エントリ生成はありません。FD/dup/CWD/カーソル、親オブジェクト、リース、権限が元の物に従い、未変更の子孫は stat/スナップショットを保持します。同名の削除済み物と新しい物は別です。動的パス費用は初期ゼロ、最初だけ現パスを課金し、次回は旧費用を置換します。固定費用は返却せず、パス/予算失敗は両木を維持します。

SDK 非依存の original initial-directory-swap は既存 empty と data を交換し戻し、子孫、マッピング、CWD、カーソル、FD flags、移動後作成、清掃を確認します。前節 f98068c07 は別の凍結された通常移動の検証で、初期根 SWAP はこの宣言だけで拡張します。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```


macOS ARM64 Release の今回の検証は Darwin 登録 1,303 件、成功 823 件、利用不能バックエンドによるスキップ 480 件、失敗 0 件。必須 ARM64 HVF 120 件はすべて実行した。ファイル検査 361/361（新規 4K/16K 動作 16 件と受け入れ検査 3 件）、C/CLI 180/180、レポート解析 34/34、原カーネルプログラム 30/30、独立プローブの観測 35 件が成功。Python は 5 構成を 33.894 秒で実行し、純 API 71 件、一覧/参照ユニット 49 件、SDK 差分、書式、能力、来歴、文書検査も成功した。件数は重複する。

容量境界では同名操作と 16 回の往復交換が元のオブジェクトと課金を保持する。必要量 6 バイトに対して残量 5 バイトの場合、両方向の拒否で両ツリー、カーソル、後続作成予算が変わらない。独立した計画・最終コードレビューは承認済み。試行、ソース、バイナリ、結果を `build-hvf-arm64/initial-directory-swap/` に保存しコミットに結び付ける。重複実行したレポート検査は除外して逐次再実行し、翻訳マーカーを同期した。期限や負の対照は緩和していない。権限、未宣言マウント/大文字小文字、共有マップ/EOF、進行時計、Mach/スレッド/dyld/フレームワークは未完了。実機 iOS、停止中の Intel HVF、リモートマージ CI は別の検証である。

## 明示的な読み取り専用リソース制限値

五つの Darwin guest 構成で `getrlimit(194)` は `DarwinSystemOptions::ResourceLimits`（`darwin_system.resource_limits`）を読みます。キー 0..8 の `DarwinResourceLimit` は、オフセット 0/8 にある二つのリトルエンディアン uint64、計 16 バイトです。`0 <= current <= maximum <= 9223372036854775807` が必要で、ゼロも明示値、INT64_MAX は無限です。ホスト照会や FD/VM/ストレージ/実行予算の変更はありません。`setrlimit`、制限の適用、シグナル、スケジューリングは未完成です。

厳格 JSON は最大九つの一意なキーと、ちょうど `resource`、`current`、`maximum` を認め、正確な整数または符号なし十進文字列を使います。型/フィールド違反、重複、非正規キー、逆転した値はロード前に失敗します。空/省略配列は不明のままです。構成キーに syscall の切り詰めやフラグ除去を適用しません。

syscall だけが選択子の下位 32 ビットを取り `_RLIMIT_POSIX_FLAG=0x1000` を除きます。不正資源はメモリ照会前に EINVAL、未指定値は出力に触れる前に unsupported です。全体が書込不可なら EFAULT、一部だけ書込可能なペアは全バイトを保ち unsupported です。非整列/ページ境界を越す完全コピーは 16 バイトのみを変更し、backend エラーは転送エラーのままです。ARM64 ネイティブ probe は値/選択子 23 件と別個の fault 4 件を通過しました。このホストの部分プレフィックス不変は一般保証ではありません。

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

macOS ARM64 Release の登録/成功/未実行 skip/失敗は 1337 / 845 / 492 / 0、必須 HVF 123 件を全実行。新規 4K/16K 行動検査十二件と SDK capture oracle、C/CLI 190/190、parser 37/37、native workload 31/31 が成功。Python 五構成は 71.978 秒、API 71 件と runner 49 件が成功し、SDK drift・整形・機能・出典・文書検査も通過。件数は重複します。証拠は `build-hvf-arm64/resource-limit-observations/` に凍結しコミットに結び付けます。最初のテスト enum の build エラーや接続/フィルター試行を保存し、修正後の直列検証は期限や負控を緩めていません。制限適用、権限、変更後ディレクトリ情報/列挙、共有 map/EOF、進む時計、Mach/thread/dyld と framework は未完成です。実機 iOS、停止中 Intel HVF、remote merge CI は別途検証が必要です。

## 明示的な読み取り専用リソース使用量

五つの Darwin guest 構成で getrusage(117) は独立した任意の DarwinSystemOptions::ResourceUsageSelf / ResourceUsageChildren を読みます。厳格 JSON は darwin_system.resource_usage.self / .children。各 DarwinResourceUsage は int64 user_seconds/system_seconds、1000000 未満の uint32 user_microseconds/system_microseconds、十四個の int64 counters を要求します。正確な整数か符号付き十進文字列で全範囲を保持し、不正な型/フィールド/微秒/長さはロード前に拒否。未指定の相手は不明のまま、指定済みの照会を妨げず、全ゼロは有効です。

一度の完全な 144 バイト little-endian コピー：timeval は 0/16（秒8、微秒4、ゼロ padding4）、counter は32から各8バイト。Darwin の生値/単位を保持し ru_maxrss に Linux KiB 換算をしません。固定値はホスト性能、会計、fork/wait、スケジューリング、制限適用を実装しません。

選択子の下位32ビットのみで 0=SELF、-1=CHILDREN。0x1000 は無効で POSIX flag 除去なし。不正値はメモリ前 EINVAL、欠落値は出力前 unsupported。完全な非整列/ページ境界コピーは guard を保持。全書込不可 EFAULT、部分書込可能は一切コピーせず unsupported、backend エラーは転送エラーです。SDK は各選択子を一度だけ捕獲し、変化する後続 SELF と比較しません。native probe は13検査と独立 fault4件を五秒上限で通過。このホストの partial SELF は EFAULT 前64バイト書込を観測し、一般的 prefix 保証にはしません。

0..13: `ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

macOS ARM64 Release 登録/成功/未実行 skip/失敗 1371/867/504/0、必須 HVF 126 全実行。File 361/361、C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 成功。新規4K/16K十二件、准入/SDK検査、Python五構成 42.865秒、API71・runner/reference49、SDK drift/整形/機能/出典/文書検査が通過。件数は重複し独立レビューも通過。証拠は `build-hvf-arm64/resource-usage-observations/` に凍結し commit に結び付けます。初版x64 EINVAL断言を RDX 保持に修正、ARM64は X1 をゼロにし、失敗/空フィルター記録を保存。runtime/期限/負控は不変。制限適用、権限、変更後のディレクトリ、shared map/EOF、時計、Mach/thread/dyld、framework は未完成。実機iOS、停止中Intel HVF、remote merge CIは別途検証です。

初回全体検証は866成功、既存iOS ARM64 HVF renameの五秒timeout1件、504skip。同一binaryで該当caseは386msで成功し、後の全体直列検証も成功。両方保存し、timeout原因は未確定、latency保証なし。


## 明示的資格情報とグループ、作成所有者の整合

任意CredentialsはRealUID/EffectiveUID/RealGID/EffectiveGIDと独立任意GroupAccessList。省略時4照会1000、明示的0/rootは有効、ID0..INT32_MAX。グループ1..16、先頭EffectiveGID、順序/重複保持。欠落は未知でhost/EGIDから補完しません。厳密darwin_system.credentialsはreal_uid/effective_uid/real_gid/effective_gid必須、groups任意。無損整数/中央検証で形/項目/範囲/数/先頭不一致をロード前拒否、非Darwinも拒否。

getuid24/geteuid25/getgid47/getegid43/getgroups79は単一system所有者。新通常ファイルUIDは実効UID、device/GIDは直接親を継承。rename/保持FD/名前再利用で物体を維持、入力stat不変。rootは書込/ディレクトリ変更/権限/ACLを与えず、setuid/setgid/setgroupsやprocess/sessionは未実装。

getgroups容量は下位32bit符号付きint。負数は最初EINVAL、未知unsupported、既知0はpointerなしcount、正の不足はメモリ前EINVAL、十分なら4*count小端byteを一度コピー。0x1000は正容量、POSIX flag除去なし。非整列/跨頁guards保持、全不可書込EFAULT、部分はbyte前unsupported、backend errorはtransport。BSD errorはx64 RDX保持/ARM64 X1消去、成功は両方secondary消去、reportはraw引数保持。


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

登録/成功/利用不能skip/失敗: 1413/897/516/0; ARM64 HVF 129/129. System/SDK72/72, File365/365, Report43/43, C/CLI210/210; guest8/12skip; native33/33; Python5 82.918s; API71, runner/reference49. SDK drift / clang-format22.1.2 / capabilities23 / provenance / docs11+negative3: OK. Independent source review: OK.

21 value +5 native fault,5s; host16groups, positive short capacity: OK. Native partial32byte thenEFAULT: observation only. Counts overlap. `build-hvf-arm64/credential-observations/`.

初回8失敗（指令予算5、5秒deadline3）、skip12。新fixtureの全2頁scanを同じ跨頁132byte guard（前64、最大data64、後最低4）へ限定。直接ownerは全2頁を検証。予算/引数/障害負対照不変、source/binary/両run保存。事前レビューでtransport test跨頁修正、public設定は文字列内容比較。権限/ACL、links、変更後directory完全観測、shared maps/EOF、clock、Mach/thread/dyld/framework未完；実機iOS、停止Intel HVF、merge CI別途。

## 宣言したプロセスとカーネルの上限による記述子表照会

任意の `DarwinSystemOptions::MaxFilesPerProcess` / `darwin_system.max_files_per_process` は非負 int の観測値です。省略は未知、明示的ゼロは有効。名前 `kern.maxfilesperproc` と数値 MIB `[1,29]` は資源制限と独立に同じ四バイト値を読みます。無損失整数解析と中央検証は不正型、負値、範囲超過をロード前に拒否し、非 Darwin 設定も拒否します。

BSD `getdtablesize(89)` はこの上限と `ResourceLimits[8].Current` の両方を必要とし、小さい方を返します。完全な 64 ビット Current を先に制限するため、Current=`0x100000001`、cap=64 は 64、無限値も安全に制限されます。Maximum、ホスト値、実 FD 数、`DescriptorLimit` からは推測しません。片方が未知なら、既知側がゼロでも unsupported。六引数は無視し、ユーザーメモリに触れず、既存 BSD carry/副レジスター規約を使います。Mach timebase の trap 89 は独立です。

sysctl のコピー段階は維持します。EUID0 の実際の書き込みは名前/MIB と oldlenp 検査後、観測/出力前に unsupported、非 root は EPERM。新ポインターの長さゼロは読み取りです。制限の実施や書き込み権限は付与しません。必須資源 workload は両 cap 照会と低/高 syscall number を検査し、`l` / 144 バイト出力を維持します。後の unsupported でも既出力は残ります。独立スカラー fixture は未知、ゼロ、広い Current と DescriptorLimit=3 の独立性を検証します。

```json
{"darwin_system":{"max_files_per_process":64,"resource_limits":[{"resource":8,"current":"4294967297","maximum":"9223372036854775807"}]}}
```

[XNU getdtablesize](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU proc_limitgetcur_nofile](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [XNU MIB constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/sysctl.h).

検証: Darwin (登録/成功/利用不可/失敗) 1441/913/528/0; ARM64 HVF 132/132; System/SDK 80/80; File 365/365; C/CLI 211/211; ProcessReport 45/45; native 33/33; Python 5 (42.254s); API 71; runner 44 + reference 5. clang-format 22.1.2; capabilities/provenance; docs 286 / locales 10 / negative controls 3.

試行記録：最初の runner 検査では、新スカラーメソッドの必須登録がなく ARM64 と x86-64 の一覧検査が失敗しました。全メソッド一致規則を維持して登録を追加しました。初回の必須129件のゲートと失敗記録は保持し、新しい最終 ARM64 ゲートは132件を要求します。

件数は重複します。`build-hvf-arm64/descriptor-table-observations/` は実行時ベースラインとソース/バイナリー/ログの正確なハッシュを保持し、過去証拠は変更しません。原生 ARM64 macOS の使い捨て子プロセス五個、40 検査、各五秒期限：既定 Current=1048575/cap=245760 は245760、子 Current=0/1/32/245777 は0/1/32/245760。親とシステムの上限は変更しません。実機 iOS、停止 Intel HVF、remote merge CI は別検証です。権限、変更後のディレクトリー観測、共有 map/EOF、進行時計、Mach/thread/dyld、framework runtime は未完成です。

## 明示的なプロセス観測値

`DarwinSystemOptions::ProcessGroupID`、`SessionID`、`ProcessTainted` は独立した任意入力で、JSON 名は `process_group_id`、`session_id`、`process_tainted` です。ID は正数かつ INT32_MAX 以下、汚染状態は JSON の Boolean `true`/`false` のみです。省略は不明、明示的な `false` は既知のゼロです。ホスト、PID1000、資格情報や別の観測値から推定しません。

生の `getpgrp(81)` はプロセスグループを読みます。`getpgid(151)` と `getsid(310)` は符号付き下位32ビットの `pid_t` を使い、ゼロまたは固定の現在 PID1000 を自身として扱います。`0xffffffff000003e8` も自身です。負の下位32ビット PID は観測値の検索前に ESRCH3 を返し、読み取り専用ネイティブプローブと XNU のプロセス割り当て・検索で確認しています。`0x1000` を含む不明な正の他プロセスは UnsupportedService で停止し、ESRCH やフラグマスクを推測しません。選択した自身の値がない場合も未対応で停止します。`getpgrp` と `issetugid(327)` は全引数を無視し、四つのスカラー問い合わせはゲストメモリにアクセスしません。BSD carry と第2戻りレジスタの既存規則を維持します。

`process_tainted` は固定の `P_SUGID` 観測値で、実 ID と実効 ID の一致・不一致とは独立です。EUID、ファイル所有権、sysctl 書き込み権限、エンタイトルメント、リーダー状態や端末状態を変更しません。`setpgid`、`setsid`、資格情報の変更は未対応です。元の読み取り専用 `process-observations` は自身の PID を取得し、高位キャリア、負数エラー、戻り状態を検証します。`virtual-process-observations` は設定したグループ・セッション・汚染状態のバイト列を出力します。他プロセス、欠落値、設定用呼び出しのモデル専用ケースはネイティブ実行一覧に含めません。macOS の参照実行は実機 iOS の検証ではありません。

```json
{"darwin_system":{"process_group_id":7,"session_id":16909060,"process_tainted":false}}
```

[XNU process queries](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c), [XNU service numbers](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/syscalls.master), [XNU PID allocation](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_fork.c), [XNU PID lookup](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_proc.c).


## 明示的なセッションログインバッファ

`DarwinSystemOptions::LoginNameBytes` はセッションの255バイト全体（`MAXLOGNAME`）を独立に宣言する任意の観測値です。JSON `login_name_hex` は大文字・小文字を許す510文字ちょうどの ASCII 十六進数です。埋め込みNULや終端後の非ゼロバイトも有効です。省略は未知、明示的な全ゼロは既知です。短い名前を補完せず、ホスト、資格情報、プロセスグループ、セッションID、汚染状態から推測しません。ログインやファイル権限は与えません。

生の `getlogin(49)` は長さの符号なし下位32ビット `u_int` を使い、min(length,255) バイトだけをコピーします。文字列の解釈、NULの追加、必要サイズの出力はありません。長さゼロは観測値やゲストメモリなしで無効ポインタでも成功し、`0xffffffff00000000` もゼロを選びます。非ゼロでは宛先検査より先に完全な観測値が必要です。全体が書き込めなければ EFAULT14、部分的に書き込める範囲はコピー前に未対応として停止します。事前検査エラーはバイトを公開しません。既存のコピー層がバックエンド書き込みエラーを伝え、一般的なロールバックは保証しません。BSD carry と第二戻り値規則、エラー時の x64 RDX を維持します。

`setlogin(50)` は明示rootや全ゼロでも未対応です。独自の読み取り専用 `login-buffer` は完全な長さ引数、接頭部分、隣接バイト、ゼロ長ポインタ、EFAULTを検査し、`virtual-login-buffer` は宣言された255バイトを出力します。欠落値と設定操作のモデルテストをネイティブ実行に含めません。macOS ARM64の参照は実機iOSやIntelの受け入れではありません。例は255個のゼロを明示し、空ログインを推測しません。

ゲスト検証器は各コピーの前に出力の265バイトすべてを初期化し、昇順で重ならない三つの範囲 [0,3)、[3,3+n)、[3+n,265) を検査します。n は従来と同じ上限処理済みコピー長です。最初と最後の範囲は全ガードバイトを、中間は全コピー済みバイトと元のスナップショットを比較します。12個の完全長引数、21回のゼロ長呼び出し、42回のEFAULT、carry・第2レジスタ検査、raw/virtual経路は既存の実行上限で維持します。検証処理を減らしても全バイトの網羅と最初のエラー順序は保ちます。製品ランタイムの性能は別途測定が必要です。

```json
{"darwin_system":{"login_name_hex":"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}}
```

[XNU getlogin / MAXLOGNAME](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c#L1561).


## 現在のプロセス優先度の明示値

`DarwinSystemOptions::ProcessNice` / JSON `darwin_system.nice` は -20 から 20 の固定符号付き nice 値を独立に宣言します。省略は未知、明示的な0と -1 は既知です。既存の損失のない整数解析で整数値と十進整数文字列を受け入れ、不正な型、小数、指数文字列、空白、範囲外値はイメージ読み込み前に拒否します。正確な整数の数値 JSON は有効です。Darwin 以外では拒否します。ホスト、資格情報、グループ、セッション、汚染、ログイン、資源、CPU から推測せず、スケジューリング、権限、実行予算を変えません。

生の `getpriority(100)` は int 選択子と符号なし `id_t` 対象の下位32ビットを使います。対象が INT32_MAX を超えると最初に EINVAL22。不明な選択子（GPU5、0x1000 を含む）とスレッド選択子3の非ゼロ対象も観測値の前に EINVAL です。`PRIO_PROCESS`0 は0または現在の PID1000 のみを受け入れ、自己選択後に nice が必要です。他の正の PID は ESRCH を推測せず未対応です。グループ1、ユーザー2、スレッド3/対象0、拡張4,6,7,8 は関連観測値があっても未対応です。スレッド対象の上位だけが非ゼロなら未知状態であり EINVAL ではありません。結果は64ビットに符号拡張され、-1 は carry がクリアされた成功 UINT64_MAX です。ゲストメモリを使わず未使用引数を無視します。BSD の第二戻り値規則は x64 エラーで RDX を保持、成功で0、ARM64 は両方で X1 を0にします。

`setpriority(96)` は明示 root/nice でも未対応です。独自の読み取り専用 `process-priority` は自己引数、不正引数、成功/エラー/成功の遷移を検査し、`virtual-process-priority` は設定の符号付き8バイトを出力します。他プロセス、集約、欠落、設定操作はモデルのみです。新しい ARM64 macOS 探針は nice0 で191項目に成功しました。非負サンプルでは負数のハードウェア拡張は証明できず、固定版の符号付き入口宣言と独立モデル境界を使います。実機 iOS と Intel のネイティブ検証は別です。

```json
{"darwin_system":{"nice":-1}}
```

[XNU getpriority](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_resource.c), [XNU signed INT entry](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/dev/arm/systemcalls.c).


## リンク追跡を禁止する open フラグとディレクトリ事前検査

通常および nocancel の `open` / `openat` は、符号なし下位32ビットの O_NOFOLLOW=0x100 または O_NOFOLLOW_ANY=0x20000000 を受け付けます。検索フラグは F_GETFL に含まれず、アクセス、追記、切り詰め、作成、FD 固有の CLOEXEC を変えません。両方を指定すると FD 容量確認後、完全なパスの読込み前に EINVAL22 を返します。満杯なら先に EMFILE24 です。未知のフラグは未対応です。

AT_FDCWD 以外の `openat` は、アクセスモードと FD 容量より先にパスの最初の1バイトだけを読みます。読めなければ EFAULT14。相対接頭辞（NUL を含む）は FD が保持するディレクトリを先に検査し、未知の FD は EBADF9、通常ファイルは ENOTDIR20、未知のストリーム vnode 型は未対応になります。`/` は dirfd 検査を省き、その後は既存の open 順序でパス全体を読みます。通常の `open` と AT_FDCWD はこの事前検査を行わず、他の nameiat も同じ1バイト/相対 FD 検査を共有し、AT_FDONLY はパスを無視します。拒否時は FD や新しい inode を消費せず、転送エラーをそのまま伝え、名前空間を変更しません。

既存の `file-access` は NOFOLLOW_ANY を相対ディレクトリ FD で検査し、原生 `/var`、`/tmp` の別名リンクを避けます。直接テストは最初と後続のバイト障害、スラッシュ/NUL、ユーザー領域とページ境界、FD 満杯、削除済みディレクトリを区別します。ARM64 macOS の独立した生呼出しプローブは従来の5秒制限で30件成功しました。最後の満杯時絶対パス検査は、元のラベルにかかわらず有効なディレクトリ FD を使っています。実機 iOS と Intel の原生検証は別途必要です。

[XNU open1at / open1](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU vn_open_auth](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_vnops.c), [XNU open flags / FMASK](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/fcntl.h).

## 固定初期シンボリックリンク

以下の初期名は既定で保護されます。明示的な変更権限を付与した場合の動作は末尾の節を参照してください。 `DarwinFileOptions::SymbolicLinks` / `darwin_files.symbolic_links` は正規絶対 `path` と生の16進 `target_hex`、任意のリンク自身の `metadata` を指定します。目標は1..1023非NULバイトで、非UTF-8、連続スラッシュ、ドット、存在しない目標も保持します。`files` は空配列でも必須です。名前の衝突/リンク配下の宣言は禁止。名前/NUL/目標は256項/16 MiBを共有。S_IFLNK、目標長と同じsize、DT_LNK=10、inodeの一致が必要で、設定CWDは実際のディレクトリです。

リンクを展開してからドットを処理します。相対目標は実際の親、絶対目標はゲスト根から開始。各展開で末尾スラッシュを再解析し、消費した入力スラッシュは引き継ぎません。32展開まで許可し、33回目はELOOP62。目標+残り+NULが1024バイトを超えるとENAMETOOLONG63。FD/CWD/F_GETPATH/mmapは解決した対象を保持します。

stat64/open/access/truncate/chdirは末尾リンクを追跡し、lstat64/readlinkは保持します。O_NOFOLLOWはELOOP、O_DIRECTORY併用は先にENOTDIR20。O_NOFOLLOW_ANYは必要な展開を拒否。O_CREAT|O_EXCLは存在する末尾リンクにEEXIST17（循環/未存在目標も）。AT0x20は末尾保持、AT0x800も末尾保持し中間/末尾スラッシュの展開を拒否、併用可。AT_FDONLYはフラグ検証後パスを無視します。

readlink(58)は符号付き下位32ビットcount、readlinkat(473)は完全なsize_t。intを返し、INT32_MAX超過はパス/FDより先にEINVAL22。min(count,目標長)だけコピーしNULなし、実際の範囲だけ検査。ゼロ長もパス/型を検証後に出力を無視。非リンクEINVAL22、全域書込み不可EFAULT14、部分書込みはコピー前に停止。転送/メモリ予算エラーは伝播します。

固定リンク名と生の対象バイトは不変です。MutableDirectories はルートや固定リンク名のパス区切り上の祖先になれません。/work は /workspace/link を含みません。別の可変ディレクトリ内の対象は実行中に作成・移動・削除・置換できます。親、マウント、別名、フラグ、SWAP 対応、作成ポリシーの既存検証は維持され、新しい inode は保護リンクを含む全メタデータ/スナップショット inode より大きい必要があります。 固定名WritableFiles/MutationPoliciesは対象の通常ファイルを変更できます。リンクunlink/renameは作用前に停止。実行時リンク作成は次節で説明します。ハードリンク、ACL、個別の権限のない初期リンク変更一覧は未対応。ARM64 macOS独立プローブは元の5秒以内に189観察/115全バッファを通過しました。物理iOS/Intel HVF/完全OSの証明ではありません。

追加の ARM64 macOS DELETE/RENAME 60 ケースは元の5秒制限内で完全な stat バッファ、変更前後の名前空間、保持 FD/CWD の識別を記録します。末尾スラッシュは固定リンクを展開して実際の対象を変更でき、必要な展開を NOFOLLOW_ANY は ELOOP で拒否します。SDK 不要の symbolic-link-mutations は作成、存在しない対象、移動/削除/置換、保持 CWD の親、FD 終了前の元のファイル10バイト全体と、終了後にも保持されるマッピング10バイト全体を検証します。実機 iOS やネイティブ Intel の証明ではありません。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"symbolic_links":[{"path":"/link","target_hex":"64617461"}],"working_directory":"/"}}
```

[XNU namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c), [XNU readlink / AT](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU open authorization](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_subr.c).

## 実行時のシンボリックリンク作成

生の `symlink(57)` と `symlinkat(474)` は、変更を許可したディレクトリ内にプロセス内リンクを作成し、intを返します。dirfdは下位32ビット、絶対宛先はFDを無視します。宛先より先に対象を最初のNULまで読み込みます。0..1023バイトの対象は空、非UTF-8、点、連続スラッシュを許可します。1024バイトにNULがなければENAMETOOLONG63、先に読めないバイトがあればEFAULT14です。初期JSON対象は引き続き1..1023バイトです。

現在のリンク表が実際の名前、親、対象バイトを保持します。既存末端はEEXIST17。ダングリングリンクの消費済み末尾スラッシュは対象名での作成を許し、元リンクは変わりません。空対象の展開はENOENT2。空対象のreadlinkは正の容量でも出力ポインタに触れず0を返しますが、count/パス/型の検査は先に行います。

名前/NULと対象を一度だけ課金し、256エントリ/16 MiBを共有します。拒否はノード、親、FD、通常ファイルinodeに影響しません。新リンクの完全メタデータは未知で、通常ファイルCreationPolicyや再利用名の古い観察を引き継ぎません。作成後の親stat/スナップショットは未知です。対象を削除・置換してもFD/CWD/マッピングは元の物体を保持します。rmdirとディレクトリ置換はリンク子を検出します。移動/SWAPのどちらかに保護された初期リンクがあれば作用前に停止します。リンクと実ディレクトリの置換、ハードリンク、ACL、個別の権限のない初期リンク変更一覧は未対応。別名は実際の親の権限を移しません。

ARM64 macOSの150件の原生記録は4件の観察器失敗を保持し、別の10件で実際の新対象と空リンク境界を確認します。SDK不要の `symbolic-link-creation` は両入口、対象/バッファ境界、親、置換ファイル、旧FD/マッピングの全10バイトを確認します。物理iOS、原生Intel、完全OS互換の証明ではありません。

[XNU symlink / symlinkat](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU empty-link expansion](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c).

## 実行時に作成したシンボリックリンクの削除

`unlink(10)` と `unlinkat(472)` は実際の親に変更権限がある実行時リンクを削除します。初期の固定リンクは保護されたままです。裸の `AT_SYMLINK_NOFOLLOW_ANY` は末端リンクを保持し、欠落・循環・空の対象でも削除できます。中間や末尾スラッシュで展開が必要なら ELOOP62 です。フラグなしの `a → b → target` で `a/` を削除すると、`b` のみ消え、`a` と最終対象は残ります。既存のフラグ・パス・dirfd エラー順序を保持します。

成功時は動的1項目と現在のパス/NUL/対象バイト分を一度だけ返却し、実際の親の完全な stat/列挙観察を無効にします。FDや生成inodeは不要です。対象の内容・変更ポリシー・記述子・共有カーソル・CWD・マッピングは元の物体を保持します。名前再利用で旧リンクは復活せず、拒否は状態や予算を変えません。実行時に作成したリンクの完全なメタデータは引き続き不明です。リンクと実ディレクトリの置換、保護された初期リンクを含む子ツリーの移動/SWAP は未対応です。

ARM64 macOS の独立40例は元の5秒期限で28削除成功、12エラー、17回の再削除 ENOENT と FD/CWD/私有マッピングを記録しました。客体・実機iOS・Intelの検証ではありません。`symbolic-link-unlink` と公開APIの検査は別に行います。

## 実行時に作成したシンボリックリンクの改名

`rename(128)`、`renameat(465)`、`renameatx_np(488)` は通常の改名と `RENAME_EXCL=4` を扱います。実行時リンクから未使用名、リンク同士、リンクから通常ファイル、通常ファイルからリンクへの置換を認めます。実際の両親に同じ確立済みマウント内の変更権限が必要で、初期リンクは不変です。共有リゾルバーが実際の節点を選び、空・欠落・循環・非UTF-8の対象バイトを保持します。相対対象は移動後の親から解決します。裸の `RENAME_NOFOLLOW_ANY=16` は末端リンクを保持し、中間展開が必要なら ELOOP62。異なる既存EXCL宛先は EEXIST17。同じ物体へのEXCLは大小文字区別の契約がなければ未対応です。

公開前に新しいパス/NUL費用を予約し、NUL込み1024バイト以内に制限します。置換された実行時リンクの現在のパス/NUL/対象費用は参照先FDやマッピングに関係なく一度だけ返却します。置換された通常ファイルの内容/動的パスは全記述子・マッピングのリース解放まで保持し、即時回収可能な通常宛先だけ予約額を供給します。追加項目、FD、通常ファイル生成inodeは不要です。拒否は両節点を保持し、成功は実際の親の完全なstat/列挙観察を無効化します。実行時リンクの完全メタデータは未知です。リンク/実ディレクトリ置換、ハードリンク、保護された初期リンクを含む子ツリー移動/SWAPは未対応です。

独立したARM64 macOSの19例は元の5秒期限で14成功と5件のEEXIST/ELOOPを記録し、リンクinode/対象バイト、相対対象の再結合、保持したFD/dup/カーソル/CWD/私有マッピングを確認します。SDK不要の `symbolic-link-rename` と公開SDK/CLI検査は別に行います。原生参照だけでは実機iOS、原生Intel、完全OS互換を証明しません。

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 実行時に作成したシンボリックリンクの交換

`renameatx_np(488)` は `RENAME_SWAP=2` でリンク同士・リンク/通常ファイル・通常ファイル/リンクを交換します。実際の両親に同じ確立済みマウントで変更とSWAP権限が必要です。同じ名前なら追加SWAP宣言なしで作用なく成功します。対象欠落はENOENT2、`SWAP|NOFOLLOW_ANY=18` は末端リンクを保持して必要な中間展開をELOOP62で拒否し、`SWAP|EXCL=6` はパス読取り前にEINVAL22です。対象バイトは変えず、相対対象は両方の新しい親から解決します。

両パス/NUL費用を名前の公開前に予約します。両物体はリンク状態を保ち、内容やマッピングのリースから置換控除を得ません。初期ファイルは最初の交換で動的パス費用を持ち、戻すときも再利用します。項目・FD・生成inodeは消費しません。ファイルの同一性・nlink・記述子・カーソル・CWD・マッピングを保持し、親の完全な観察だけ未知になります。実行時リンク完全メタデータ、実ディレクトリ/リンク、保護された初期リンクを含む子ツリー移動/SWAPは未対応です。ARM64 macOSの22例は元の5秒期限で14交換・同物体2成功・6エラーを記録しました。`symbolic-link-rename` とSDK/CLI/Pythonが交換とエラーを検査しますが、実機iOS・原生Intel・完全OS互換は証明しません。

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 実行時リンクを含むツリーの移動

通常/EXCLのディレクトリ移動とSWAPは実行時リンクの子孫も扱い、ディレクトリ/ファイル交換にも対応します。既存トランザクションはディレクトリ・ファイル・リンクの全新名とNUL込み1024バイト制限を検査し、三つの表を全て抽出してから名前を公開します。対象バイトは変更せず計上を維持し、現在のパス/NUL費用だけ更新します。SWAPは置換控除を与えず、新項目・FD・生成inodeも不要です。

リンクは移動する実際の親を保持し、相対対象は新しいパスから解決します。記述子、共有カーソル、CWD、削除済み節点、マッピングは元の物体を保持します。保護された初期リンクとそのツリー、根の実ディレクトリ/リンク対、リンク完全メタデータ、ハードリンク、ACLは未対応です。ARM64 macOSの25対照は元の5秒期限で5移動・10交換・同物体2成功・名前を変更しない8拒否を記録しました。親間の対照は生のバイト保持と相対対象の再解決を確認し、`symbolic-link-rename` がC++/SDK/CLI/Pythonを検査します。実機iOS・原生Intel・完全OS互換は証明しません。

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 作成したリンクとディレクトリのメタデータ

任意の `creation_policy.namespace_policy`（C++ `DarwinFileCreationPolicy::Namespace`）は従来の必須5項目に厳密なオブジェクトを追加します。項目は `symbolic_link_allocation_unit`、`directory_entry_size`、`directory_blocks` のみ。リンク単位は512..16 MiBの2の累乗、ディレクトリエントリサイズは正で最大16 MiB、ブロック数はINT64_MAX以下のuint64です。十進文字列が使えます。親のメタデータ、初期umask、新inode条件は同じです。省略時は前節の未知リンク/ディレクトリメタデータと通常ファイルのみのinode消費が既定です。

有効時は通常ファイル、symlink、mkdirの成功挿入が一つのinode列を共有し、UINT64_MAXで永久に枯渇します。失敗や既存名openは消費しません。Device/GIDは実際の親、UIDは実効ゲストIDから得ます。リンクのmodeはS_IFLNKと `0777 & ~umask`、nlinkは1、sizeは空や非UTF-8を含む対象の生バイト数、blocksは宣言単位で切り上げた512バイトブロック数です。ディレクトリmodeはS_IFDIRと `mode & 0777 & ~umask`、nlinkは2と全種類の直下の連結名数の和、sizeはnlinkと宣言エントリサイズの積、blocksは固定です。保持された削除済み空ディレクトリにも適用します。これは明示仮想契約でAPFSの推測ではありません。

初期時刻はcreation_time。子の名前変更は作成親のmtime/ctimeを、直接移動はリンク/ディレクトリのctimeのみをmutation_timeで更新します。祖先移動は子孫のメタデータを保持し、dup/CWD/置換/SWAP/削除/名前再利用でも記録は物体に属します。作成拡張だけでは初期親の完全な stat と固定スナップショットは保持されません。下記の独立方針が stat とライブ列挙を提供します。ACL、個別の権限のない初期リンク変更、一般のディレクトリ/リンク根トランザクションは未対応です。ネイティブ `created-namespace-metadata` は共通のmode/所有者/同一性/寿命を確認し、`virtual-created-namespace-metadata` はC++/SDK/CLI/Pythonで144バイトの完全な定数記録を比較します。対象検証はモデル/受入11、厳密JSON1、元の5秒制限のネイティブ43、実行可能ゲスト8（利用不能12スキップ、必須HVF3実行）、公開10が成功。ポインタ表による失敗と修正後の静的ARM64記録は保持。ネイティブIntelと実機iOSは未検証です。

```json
{"namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}}
```

## 名前空間変更後の仮想ディレクトリ列挙

任意の `directories[].enumeration_policy` は現在の名前を `getdirentries64` で列挙します。C++ は `DarwinFileOptions::DirectoryEnumerationPolicies` と `DarwinDirectoryEnumerationPolicy` を使用します。厳密なオブジェクトは `minimum_buffer_size`、`initial_minimum_buffer_size`、`seek_offset` の3項目を要求し、最初は正、両最小値は128 MiB以下、seek_offset は損失のない uint64 です。初期ディレクトリ自身の非ゼロ inode メタデータが必要で、不変 `contents` とは併用できません。参照ごとにパスと NUL を一度計上します。mkdir は実際の親の方針を継承し、既存の子孫は自身の宣言を保持します。列挙方針と変更権限は独立です。

仮想順序は `.`、`..`、符号なしバイト順の直接リンク名です。inode は観測済みまたはプロセス生成のオブジェクトから取得し、`..` は保持された実際の親に従います。子/親の識別情報が不明またはゼロなら出力前に停止します。完全な初期 stat が無効でも inode のみ保持し、他の古いフィールドは再利用しません。カーソルは1からのローカル序数、d_seekoff は宣言定数です。dup は共有、open は独立です。確定したメンバー変更と直接移動後はゼロへの巻き戻しが必要で、拒否と同一物体の無操作は無効化しません。祖先移動は子孫カーソルを保持し、版の枯渇は明示停止します。保持された削除済み空ディレクトリは名前再利用後も0レコードです。

この段落は列挙方針のみを指定し、下記の初期 stat 方針を指定しない場合の記録です。 既存の記録符号化、完全レコードの格納、バッファ下限、ペイロード上限、EOF末尾とデータ/カーソル/位置/フラグの順序を再利用します。初期親の完全 stat と固定スナップショットは無効化され、方針省略時は従来の未対応動作です。APFSの世代は再現しません。独立ARM64準備は変更のない5秒期限内に25イベント/16ビューを記録しました。`directory-enumeration-mutations` はネイティブ識別と保持物体、`virtual-directory-enumeration` は客体、C/CLI、Python経由の160バイト定数ビューを検証します。Intelネイティブ、実機iOS、ACL、ハードリンク、完全OS/フレームワークは未検証または未対応です。

## 初期ディレクトリの明示的な stat 変更方針

任意の `directories[].mutation_policy` は、受け入れた初期ディレクトリの完全な stat を名前空間変更後も保持します。C++ は `DarwinDirectoryMutationPolicy` と `DarwinFileOptions::DirectoryMutationPolicies` を使います。厳密なオブジェクトのフィールドは `directory_entry_size` と `mutation_time` のみです。項目サイズは正で最大 16 MiB、時刻は損失のない符号付き 64 ビット秒と [0,1000000000) のナノ秒です。ディレクトリ自身の完全な metadata と非ゼロ inode が必要です。参照ごとにパスと NUL の費用を一度計上し、投影 size はファイルのバイトを確保しません。この方針は名前空間操作や権限を付与せず、作成・列挙方針も要求しません。

最初の実際の変更確定までは観測記録全体を保持し、確定時にスカラーをディレクトリへ割り当てなしでコピーします。子の名前変更では nlink を 2 と全種類の直接リンク名の数の和、size を nlink と directory_entry_size の積、mtime/ctime を mutation_time に設定します。直接の移動、SWAP、削除では ctime のみ変更し、祖先の移動は子孫を保持します。拒否と同一オブジェクトへの無操作は変更しません。Device、inode、mode、所有者、blocks、ブロックサイズ、flags、generation、atime、birthtime は観測値のままです。明示的な仮想規則であり、APFS の割り当て、リンク数、時計の推測ではありません。

記録と方針は dup、保持 FD、CWD、置換、削除、名前再利用でも元のオブジェクトに属します。新しい mkdir は、指定された場合は別の作成方針を使い、親や同名の旧オブジェクトの初期 stat 方針を継承しません。不変スナップショットは変更後も不明となり、独立した enumeration_policy がライブビューを提供できます。本方針を省略すると、変更された初期ディレクトリの完全なメタデータは不明のままです。

独立した ARM64 ネイティブ準備では、変更のない 5 秒制限内で保護付き生 stat ビュー 27 件と操作 15 件を保存し、144 バイト SDK ABI と保持された同一性を確認しました。時刻や割り当ての一般規則は導きません。独自の `initial-directory-metadata` は共通ネイティブ観測を、`virtual-initial-directory-metadata` は guest、C/CLI、Python の完全な 144 バイト定数記録を検証します。モデルは初回削除、変更許可の欠如、作成方針の省略、スナップショットと列挙の独立性も扱います。ネイティブ Intel、実機 iOS、ACL、ハードリンク、個別の権限のない初期リンク変更、完全な OS/フレームワークは未検証または未対応です。

```json
{"mutation_policy":{"directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}}}
```

## 初期シンボリックリンクの明示的な変更権限

`symbolic_links[].mutable:true` と C++ `DarwinFileOptions::MutableSymbolicLinks` は元のリンク物体の名前空間変更を許可します。実際の親にも独立した変更権限が必要です。既知の flags、特殊 mode、link_count≠1、識別子の別名、親デバイスの矛盾は拒否します。省略した名前は保護され、変更可能な祖先領域に置けません。権限は固定の path/NUL 参照を予約し、エントリや作成 inode を増やしません。初期ターゲットは不変です。

unlink、通常/EXCL rename、葉リンク/ファイル/link SWAP、宣言済みディレクトリ部分木の処理は、実際の親・mount・SWAP 条件を保持します。相対ターゲットは新しい親から再解決され、FD/dup、CWD、mapping lease は元の参照先を保持します。初期名前/ターゲット/参照の固定費用は削除後も残り、初回の改名には新しい動的名前費用が必要です。返却するのは物体が所有する動的名前/作成ターゲットのみです。動的エントリ数は新規リンクだけです。拒否と同物体の空操作は変更しません。

`symbolic_links[].mutation_policy` は `DarwinSymbolicLinkMutationPolicy` と `DarwinFileOptions::SymbolicLinkMutationPolicies` を使います。唯一の `mutation_time` は損失のない signed 64-bit 秒と [0, 1000000000) のナノ秒で、権限と完全な非ゼロ inode 観測が必要です。最初の直接移動/SWAP は割当なしでスカラーを複製し ctime だけ変更します。blocks を含む他の値、祖先移動、拒否、空操作は保存されます。省略すると直接移動後の完全 stat は未知ですが、列挙用 inode と既知のデバイス矛盾は保持します。方針の path/NUL 費用は一度予約します。名前の再利用や新 symlink は初期記録/方針を継承せず、別の namespace policy を使います。

独立 ARM64 準備は 14 guarded raw-stat、11 操作、144-byte SDK ABI、compile120s/native5s/drain1s/reap1s と私有領域の削除を記録します。`mutable-initial-links` はネイティブ識別子、相対再解決、参照先寿命を確認し、`virtual-mutable-initial-links` は五つの guest、C/CLI、Python で完全 stat を確認します。モデルは二つのページサイズ、部分木 SWAP、厳密な費用と entry/inode 枯渇を検査します。Intel と実機 iOS は未検証で、hard link、ACL、完全 OS/runtime/framework は未対応です。

```json
{"mutable":true,"mutation_policy":{"mutation_time":{"seconds":-13,"nanoseconds":456}}}
```

## ディレクトリとシンボリックリンクのルート交換

`renameatx_np(RENAME_SWAP)` は空でない部分木を含む実ディレクトリとリンクを両方向で交換します。初期ディレクトリは `exchangeable`、初期リンクは `mutable`、両方の実親は同じ確立済み mount の変更/SWAP 権限が必要です。保護された初期子孫は拒否します。既存の通常移動権限の下でディレクトリ→リンクは ENOTDIR20、逆は EISDIR21、異なる既存名への EXCL は EEXIST17、自身の子孫リンクとの交換は両方向とも変更前に EINVAL22 です。リンク自体を交換するため自己参照を作る成功は可能で、後の追跡は ELOOP62 になります。

既存の三つの名前表の取引はルート、接続中または保持された子孫、全パスと動的容量を公開前に検査します。初期名/ターゲット/参照、ファイル内容、mapping lease は SWAP の控除になりません。初期名の最初の再キーは別の動的パス/NUL を課金し、再交換は旧費用を一度置換します。entry/FD/作成 inode を追加消費せず、同じ綴りの古い孤立物体も実親が異なれば新木に属しません。生ターゲットは不変で相対解決は新親に結合します。FD/dup cursor、CWD、参照先、mapping は保持されます。直接ルートは自身の既存 stat 方針で ctime だけを更新し、祖先移動は子孫記録を保持します。方針省略時の完全 stat は未知ですが inode はライブ列挙に使え、関連列挙バージョンは既存のゼロ巻戻し契約に従います。JSON フィールドや権限の追加はありません。

独自 ARM64 準備は 36 guarded 144-byte stat、16 raw rename、compile120s/native5s/drain1s/reap1s、回収と私有削除を記録します。`directory-link-roots` と `virtual-directory-link-roots` は五つの guest、C/CLI、Python でネイティブ識別と完全 stat 定数を確認します。二つのページモデルは厳密な予算、未オープン子孫の溢れ、孤立物体、FD/entry/inode 枯渇を検査します。本節は上記権限の範囲で以前の制限を拡張します。Intel ネイティブ、実機 iOS、hard link、ACL、完全 OS/runtime/framework は別の未検証または未実装項目です。

## 固定カーネル pathconf クエリ

生の pathconf(191) / fpathconf(192) は XNU 固定 vnode クエリを扱います。15/16/17→1、19/25→0、20/22/23→4096、21→65536、24→255。リンク、割り当て、I/O 宣言、転送推奨値の観測であり、非同期実行や権限を有効にせず、ページサイズやカタログ予算から値を推定しません。

パスの完全なリンク追跡と FD 検索が low32 セレクタより先です。CWD、リンク、EFAULT/ENOENT/ENOTDIR/ELOOP を維持し、無効/閉じた FD は EBADF。保持した通常ファイル/ディレクトリは stat がなくても dup、移動、削除、名前再利用後に照会できます。入力/捕捉 FD のネイティブ種別は不明として停止。BSD int/キャリー/第2レジスタ契約を維持し、出力コピー、カーソル、メタデータ、列挙、エントリ/FD/inode を変更しません。NAME_MAX、大小文字属性や未知セレクタは検索後も未対応で、ホスト値や EINVAL を推測しません。

kernel-pathconf、kernel-pathconf-values、kernel-pathconf-unsupported は共通動作、独立80バイト値、既存出力を保持する停止報告をゲスト/C/CLI/Python で検証します。私有 ARM64 ネイティブ準備は250照会、249 SDK 対照を0.262秒で完了し、5秒期限を維持。ネイティブ Intel、実機 iOS、ACL、ハードリンク、完全なランタイム/フレームワークは未検証または未対応です。

[XNU vn_pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU fpathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_descrip.c).

`pathconf`: `file/` → ENOTDIR; `alias/`, `alias//` → 1 (selector15); `alias/.`, `alias/child` → ENOTDIR.

## 固定共通属性リスト

getattrlist(220)、fgetattrlist(228)、getattrlistat(476) は明示カタログの固定共通属性11項目を照会します：デバイス、型、4種類の時刻、所有者/グループ、完全なモード、フラグ、ファイルID。stat64 と完全メタデータの有効性判定を共有し、欠落・無効化された観測は未知のままです。型と空選択は stat 不要です。ルート/マウントの NAME、ボリューム、ディレクトリ/ファイル/fork 固有マスク、ACL、未知オプションは返却マスク要求時も明示的に未対応です。ホスト情報やマウント名を推測しません。

パス/at は24バイト要求を先に読み、FD は low32 FD とネイティブ型を先に検証します。reserved は無視します。共有 CWD/相対FD/リンク解決はサイズ・ビットマップ検証より前のエラーを保持します。リトルエンディアン・4バイト整列で完全な st_mode と符号付き秒を格納します。返却マスク付きは120バイト、通常は100バイト。短いバッファには指定した接頭部のみ書き、必要長は全体のままです。部分アクセスはそのコピー前に停止し、符号付き uio 範囲外の長さは有効な対応要求後に EINVAL。カーソル、メタデータ、列挙、項目/FD/inode予算は変わらず、dup/削除/名前再利用の既存寿命を保ちます。

3モードが guest/C/CLI/Python で共通動作、独立した設定バイト、既存出力を保つ未対応 ATTR_CMN_EXTENDED_SECURITY を検証します。ARM64 私有準備は187 raw照会、176 SDKパス/FD比較に成功し、native5秒は不変です。SDK15.5 に getattrlistat 宣言がなく raw476 は別記。新guest/Python は5,000,000us/quantum1024、既存公開テストは10s。ネイティブIntel・実機iOS・FS固有情報・ハードリンク・権限/ACL・完全なランタイム/フレームワークは未検証または未完成です。

```text
ATTR_CMN_RETURNED_ATTRS=0x80000000
FSOPT_NOFOLLOW=1, FSOPT_REPORT_FULLSIZE=4
FSOPT_PACK_INVAL_ATTRS=8 (requires ATTR_CMN_RETURNED_ATTRS)
FSOPT_NOFOLLOW_ANY=0x800
common-attributes
common-attributes-values
common-attributes-unsupported
```

[XNU attrlist](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_attrlist.c), [XNU attr.h](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h).


## 明示された拡張属性の読み取り

getxattr(234)、fgetxattr(235)、listxattr(240)、flistxattr(241) は darwin_files の完全な順序付き観測を読み取る。ファイル・ディレクトリ・リンクの extended_attributes は厳密な {name,bytes_hex} 配列であり、省略は不明、[] は既知の空リストを表す。名前はスラッシュを含められる 1..127 バイトの UTF-8、値は不透明なバイト列である。重複を拒否して宣言順を保つ。上限は 4096 属性。名前と NUL と値を既存の 16 MiB 予算に計上し、既存オブジェクトのパスを二重計上しない。

観測は dup、移動、削除、CWD、名前の再利用でもオブジェクトに属し、stat や列挙の有効性とは独立する。新規オブジェクトは不明。内容書き込み、成功した切り詰め、不確定な非空コピー失敗は観測を無効化するが、コピー前の部分バッファ拒否は保つ。問い合わせはカーソル・入力・メタデータ・条目/FD/inode 予算を変更しない。

ABI は低32ビット FD/options/position、全64ビット size、BSD user_ssize_t/carry/secondary を使う。NULL 問い合わせは position を無視する。非空値に対するパスの非NULL size0 は ERANGE、FD size0 は長さ照会。パス get の UINT32_MAX/UINT64_MAX だけが旧式照会で、FD get は INT32_MAX に制限する。正の短いリストは完全な名前の接頭辞を出力して ERANGE、非空リストの非NULL負64ビット長は ERANGE。原生の空リストを確認できなかったため、明示的な空リストの負長は UnsupportedService。出力が完全に書けない場合はコピー前に停止する。NOFOLLOW1 と NOFOLLOW_ANY64 は独立。8/16 は検索前、FD1/64 は FD/名前の前に拒否する。読み取りで CREATE2/REPLACE4 は無視し、SHOWCOMPRESSION32 と未知のビットは未対応。com.apple.system.*、ResourceFork、FinderInfo、decmpfs、設定/削除、権限/ACL、ファイルシステム推定は範囲外。

extended-attributes / extended-attributes-values / extended-attributes-unsupported は共通の原生挙動、仮想の明示バイト、不明時に既存出力を保持する停止を検証する。guest/Python5,000,000us/quantum1024、公開API10s、原生5s は不変。ARM64 私有準備では320組の raw/SDK、全288ガードバイト、carry/secondary が一致。自動 com.apple.provenance は観測であり空リストの既定値ではない。原生 Intel と iOS 実機は未検証。完全な dyld、Mach IPC、Objective-C/Swift とフレームワークは未完成。

Sources: [XNU syscall ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU xattr calls](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ordinary attributes](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_xattr.c), [XNU xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code/probes; Apple implementation is not copied.

## 有界のオブジェクト名

ATTR_CMN_NAME=1 は getattrlist220/fgetattrlist228/getattrlistat476 で明示カタログ内の名前が一意な非ルートオブジェクトに対応します。葉名は合法 UTF-8 の1..255バイト。F_GETPATH と共通の実際のオブジェクトパスが dup、CWD、移動、SWAP、削除、名前再利用を越えて最後のリンク名を保持し、呼出し側の別名は使いません。名前と型は stat 不要ですが、選択された stat 項目には完全な有効観測が必要です。ルート/マウント名、不正名、ハードリンクや大小文字の別名、正規化、完全パス属性は未知です。

8バイトの attrreference_t は他の共通項目より前にあり、attr_dataoffset は参照自身からの相対値、attr_length は NUL を含み、名前領域は4バイトに整列します。短い出力も完全な必要長と UTF-8 途中を含む正確な前置バイトを保ちます。attribute-names / attribute-names-values / attribute-names-unsupported は guest/C/CLI/Python で原生動作、独立バイト、既存出力を保つルート名停止を検証します。ARM64 準備は601 raw照会、453保護バッファ全体の SDK 比較、384前置チェックに成功。SDK15.5 は raw476 の型付き宣言を持ちません。native5s、guest/Python5,000,000us/quantum1024、既存公開10sは不変。原生 Intel、実機 iOS、完全なランタイム/フレームワークは未検証または未完成です。

## 有界ディレクトリ一括属性

getattrlistbulk(461) は明示的な enumeration_policy.bulk_attributes=true を必要とし、省略や false は権限を与えない。この仮想 TYPE 契約は直接の子の現在の名前を符号なしバイト順で返す。ドット項目を含まず、ネイティブのファイルシステム cookie ではなくローカル序数を使う。初期ディレクトリの権限は dup、移動、SWAP、削除、名前の再使用を通じてオブジェクトに残り、新規ディレクトリには継承されない。minimum_buffer_size、initial_minimum_buffer_size、seek_offset は getdirentries64 専用である。

NAME|OBJTYPE|RETURNED_ATTRS (0x80000009) が必須で、選択した観測が有効なら既存の十一個の共通フィールドを使える。Options0/8 を許可し、bulk は二つの16ビット bitmap/reserved ワードを独立した attrlist 検証とは別に無視する。唯一の属性エンコーダーが attrreference_t と stat64 の有効性を共有する。完全なレコードだけを返し、収まる場合は8バイト、最終グループだけは4バイト単位のサイズも許可する。最初のグループが収まらなければ ERANGE で出力とカーソルは変わらない。必要な出力の一部しか書けない場合はコピー前に明示停止する。実際に返すバイトだけが書き込み可能であればよい。

dup は進行を共有し、別々の open は独立する。非ゼロの完了した走査は名前空間変更後も EOF を保持し、要求検証後にサイズと出力検査を省く。初期の空ディレクトリ offset0 は再確認する。ゼロ lseek が反復をリセットする。EOF 前の構成変更、任意の非ゼロ seek、getdirentries64/bulk の混用は明示停止する。NAME-only フォールバック、ERROR 項目、スナップショット、ACL/権限、ホスト順序、その他の mask/options は未対応。bulk-attributes / bulk-attributes-values / bulk-attributes-unsupported はネイティブ共通動作、仮想リテラル、既存出力を残す未対応選択を確認する。ARM64 の私有準備で728組の保護付き raw/SDK 比較が成功した。native5s、guest/Python5,000,000us/quantum1024、public10s は不変。Intel ネイティブ、実機 iOS、完全なランタイム/フレームワークは未検証または未完成。

Sources: [XNU bulk ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/man/man2/getattrlistbulk.2), [XNU attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h). Original implementation and probes; no Apple implementation copied.

## 明示的に許可された通常の拡張属性の変更

setxattr(236)、fsetxattr(237)、removexattr(238)、fremovexattr(239) は初期オブジェクトへの独立した許可を使います。C++ は `MutableExtendedAttributes`、JSON は厳密な真偽値 `mutable_extended_attributes=true` です。許可されたファイル、ディレクトリ、リンクには、既知の空リストを含む完全な通常の `extended_attributes` リストが必要です。内容の書き込みや名前空間の変更の許可では代用できません。許可と値は dup、移動、削除、マッピングの保持期間を通じて保持されたオブジェクトに属します。新規オブジェクトや再使用された名前の属性は未知です。既知の別名、競合するメタデータフラグ、保護されたシステム属性、ResourceFork、FinderInfo、圧縮の意味論は対象外です。

置換は仮想リスト内の位置を保ち、削除は項目を除き、新規作成は末尾に追加します。この順序はプロセス内で宣言するもので、APFS の順序を推測しません。初期属性のバイト数、件数、許可のパス参照は予約されたままです。実行時の増加分は既存の 16 MiB/4096 上限で一元管理し、削除、内容の無効化、最終解放では増加分だけを回収します。容量、転送、期限の失敗は一時状態を公開しません。必須入力が全く読めなければ EFAULT、部分的にしか読めなければ公開前に未対応として停止します。成功した変更は時刻を推測せず完全な stat を無効化しますが、識別情報、ディレクトリの構成、列挙のバージョン/スナップショット、カーソルを保ちます。

変更 ABI は low32 FD/options/position と full64 size を使います。特権および FD リンクオプションの早期検査は名前の取り込みより先、名前の取り込みはオブジェクト検索より先です。set は巨大な VFS 入力（E2BIG7）を検査する前に、非ゼロ長の NULL を拒否します。検索は通常名、position、競合の検査より先です。set は完全な値を取り込んでから、CREATE の既存項目に EEXIST17、REPLACE の欠落項目に ENOATTR93 を返します。CREATE と REPLACE の同時指定は EINVAL、削除はこの二つのビットを無視します。長さゼロの set は値ポインタを読みません。他のフラグ、未知の権限、観測していないプロバイダ動作は明示的に停止します。

xattr-mutations / xattr-mutations-values / xattr-mutations-unsupported は独自のネイティブ/ゲスト対照、独立した仮想バイト定数、既存出力を保つ許可不足の停止を検査します。ARM64 の非公開準備では726回の raw/SDK 呼び出し、完全な544バイトのガード付き観測、読み取り可能なページ全体を確認しました。native5s/compile120s/drain1s/reap1s、guest/Python5,000,000us/quantum1024、public10s は不変です。ネイティブ Intel、実機 iOS、dyld、Mach IPC、スレッド/シグナル、Objective-C/Swift ランタイム、完全なフレームワークは未検証または未完成です。

主要 ABI 資料：[XNU システムコール宣言](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master)、[xattr 定義](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h)。コードとプローブは独自作成で、Apple の実装はコピーしていません。

## 範囲を限定した Darwin ハードリンク

link は末尾のシンボリックリンクを追跡する。linkat の flags=0 はリンク自体、AT_SYMLINK_FOLLOW は対象を選ぶ。low32 の 0/0x40 のみを受け入れ、他の下位ビットは入力前に EINVAL。ソース検索とディレクトリ EPERM は宛先入力に先行し、既存の宛先は EEXIST。宛先の変更権限と明示された同一マウント領域が必要。初期識別の別名や既知のデバイス・モード・フラグの矛盾は未対応。

別名はエントリとパス/NUL の使用量のみを増やし、新しい inode を使わない。バイト、属性権限、メタデータ有効性、マッピングのリースは共有オブジェクトが保持する。明示ポリシーがリンク数と ctime を更新し、欠落時の完全な stat は未知。属性変更は stat を、内容変更は属性観測を無効化する。削除名と最終マッピングの使用量は保持され、置換は直ちに解放できる分だけを差し引く。部分木は正確な識別と親で移動し、外部の別名は動かず、相対ターゲットは選択エントリの親から解決する。

複数名を持った履歴のあるオブジェクトの F_GETPATH/ATTR_CMN_NAME は、残りが一つやゼロでも未対応。APFS キャッシュを一般化しない。bulk NAME は実エントリを使い、同一オブジェクトの通常 rename/SWAP は両名を保持する。EXCL の大小文字、Intel HVF、実機 iOS、ACL、マッピング整合性/EOF シグナル、dyld、Mach IPC、スレッド、完全なフレームワークは別の課題。この契約の範囲内だけで以前の除外を拡張する。

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## 有界 O_SYMLINK 記述子

O_SYMLINK=0x00200000 は、読取専用・書込専用・読書きの各モードで最後のシンボリックリンク自体を保持します。切れたリンクや循環リンクも対象ですが、参照先の内容を書き換える権限は付与しません。O_CREAT は最後の参照先をたどり、NOFOLLOW の ELOOP、排他的作成の EEXIST、保持したリンクに対する O_DIRECTORY の ENOTDIR の順序を保ちます。途中や末尾スラッシュの展開は既存の名前解決と NOFOLLOW_ANY に従い、F_GETFL は選択ビットを返しません。

実際の LinkNode と選択した NameIdentity を保持します。dup はフラグと位置を共有し、独立した open は別の記述を持ちます。改名・削除・名前再利用・親ディレクトリ削除後も元の物体を保持します。一意名の F_GETPATH と ATTR_CMN_NAME は保持した選択名を使い、複数名の履歴がある物体は全名削除後も vnode 名の推測を拒否します。初期予約は固定で、動的な名前・参照先・項目・属性増分は最後の所有者まで保持されます。置換はまだ所有されている最後の別名を費用控除に使えません。

I/O はリンクの参照先文字列をファイル内容として公開しません。既存のスカラー/ベクトル取込、アクセス、数の検査後、負の位置は EINVAL です。読取は INT64_MAX で零、それ以外の許容位置は長さ零でも EPERM です。書込は INT64_MAX で EFBIG、それ以外は長さ零の成功・APPEND・データアクセスより前に EPERM です。pwrite/pwritev の既存の早期負位置規則を保ちます。DATA/HOLE seek は非負で ENXIO、負で EINVAL、位置は変わりません。

書込可能な記述子の非負 ftruncate と許容 open TRUNC は WasWritten だけを設定し、参照先、完全 stat、属性、位置、記憶容量や inode を変更しません。読取専用または負長は EINVAL です。許容 F_SETFL は APPEND|NONBLOCK を変更してから ENOTTY25 を返し、dup に共有されます。未知の引数は効果前に停止します。

固定 fpathconf、fgetattrlist、独立した通常 FD 属性権限はリンク物体に作用します。相対ディレクトリ FD と fchdir は ENOTDIR。属性変更による stat 無効化を truncate は復旧しません。整列した非実行の旧式 private/shared mmap は物体種別で EINVAL となり、映射やリースを作成しません。通常 shared 映射、未知フラグ、実行権限などの既存拒否を保ちます。原生 mmap の対象は length16384、offset0、protection1/2/3 の18例です。

独自 ARM64 検証は15 truncate 例の完全144バイト stat、極端位置/数の I/O、疎 seek、F_SETFL の失敗時効果を確認します。SDK 非依存の共通プログラムは O0/O1/O2 で実行し、実際の親記述子パスを比較します。仮想経路は独立した stat/type リテラルまたは出力を保持する複数名拒否を検証します。Intel HVF、iOS 実機、ACL/権限、映射 EOF/シグナル、dyld、Mach IPC、スレッド、完全なランタイム/フレームワークは未検証または未完成です。

解釈の一次資料：[同版 XNU の映射境界](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_mman.c)。実装とプローブは独自作成で Apple 実装をコピーしていません。

```text
O_SYMLINK=0x00200000; ENOTTY=25
open O_RDONLY|O_SYMLINK|O_TRUNC: WasWritten only
LinkNode, NameIdentity, HadMultipleNames, DetachedNames
INT64_MAX read=0 / write=EFBIG; other admitted offsets=EPERM
SEEK_DATA/SEEK_HOLE nonnegative=ENXIO / negative=EINVAL
F_SETFL: APPEND|NONBLOCK effect before ENOTTY; dup shares / independent open separate
symbolic-descriptors
symbolic-descriptors-values
symbolic-descriptors-name-unsupported
SymbolicDescriptor*, SymbolicDescriptorsRetainObjectsAndNativeErrorOrder
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 transport parameters / 15 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## 有限の非ブロッキング記述子状態

通常ファイル、ディレクトリ、O_SYMLINK の open は O_NONBLOCK=4 を許容し、F_GETFL に保持します。dup は状態と位置を共有し、独立 open は別の記述を保持します。アクセス、close-on-exec、WasWritten、メタデータ、バイト、位置の既存規則が適用されます。明示した有限 stdin は EOF とポインターエラーの順序を保持し、省略入力は未知です。出力捕捉もコピーエラーと共有予算を保持します。

F_SETFL は効果前に許容低32ビットを検査し、ネイティブ open フラグ変換で一を加え、APPEND|NONBLOCK のみを変更します。高32ビットは無視し、アクセスや入力 WasWritten ビットは権限や書込実績を作りません。独立した原生値では要求3/7/11/15が状態4/8/12/0を選びます。シンボリック記述は状態変更後に ENOTTY25 を返します。ASYNC0x40 等の未知フラグは効果前に停止します。

独自 ARM64 macOS 準備は122観察で各有効オブジェクト/アクセス組合せの16要求、保持 dup、独立 open、クリア、実書込、FD 別 CLOEXEC を確認します。SDK 非依存共通プログラムは O0/O1/O2 でバイト、状態、位置、原始 BSD carry/errno ABI を比較します。来賓、C/CLI、Python は未知フラグ拒否も検証し、ARM64 HVF の三 profile を必須実行します。準備状態の待機、パイプ、ネットワーク、kqueue、非同期シグナル、ホスト I/O は追加しません。O_EVTONLY のプロセスポリシーは未対応で、Intel HVF、iOS 実機、完全な macOS/iOS 環境は未検証または未完成です。

```text
O_NONBLOCK=4; F_SETFL raw low32 mask=0x1000f
requests3/7/11/15 -> APPEND|NONBLOCK status4/8/12/0
nonblocking-descriptors / nonblocking-flags-unsupported
Nonblocking*, NonblockingDescriptorsKeepNativeControlState
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
8 model cases / 20 transport parameters / 10 public cases / 5 Python profiles
67 mandatory workloads per platform / ARM64 201 / Intel 134
```

## 明示的で有限な getentropy 観測

生の BSD getentropy500 は順序付き `DarwinSystemOptions::EntropyReads` を使用し、JSON は `darwin_system.entropy_reads` です。空でない偶数長の16進文字列を最大256件、各1..256バイト指定します。モデルの上限であり、既存の65536バイト JSON転送上限は変えません。省略は不明、`[]` は明示的な枯渇です。画像やバックエンドの変更前に両入力を検証し、他 OSの設定は拒否します。

完全な64ビット長を先に確認し、256超はメモリアクセスや消費なしで EINVAL22、ゼロは全ポインターで入力不要の成功です。非ゼロは次の正確な長さの記録を先に受理します。欠落、枯渇、不一致は不正アドレスでも効果前に UnsupportedServiceで停止します。これは再生の受理順序です。成功または完全に書き込めない EFAULT14は1件を消費します。部分書込み先はコピーとカーソル更新前に拒否し、転送エラーでも進めません。後続の返却レジスターエラーは完了済み効果を保ちます。同じ設定でも各実行は先頭から開始します。

各実行の DarwinEntropyがカーソルを所有し、入力バイトは不変です。既存の BSD分派と returnServiceが両 ISAとキャリー、副レジスターを統一します。SDK不要の再生は5客体と3 ARM64 HVF設定を検証します。固定バイトはネイティブ RNGの決定的一覧に含めません。ARM64の O0/O1/O2探査は594呼出しで、番兵変化数は正確なコピー長ではありません。ホスト乱数、暗号品質、/dev/random、libc取込み、フレームワークは未提供です。Intel HVF、物理 iOS、完全 OS互換性は未検証または未完了です。

```text
BSD getentropy500 / DarwinEntropy / EntropyReads / darwin_system.entropy_reads
full64 size>256 -> EINVAL22; zero ->0; whole EFAULT14 consumes one record
1..256 bytes per record / at most256 records / JSON transport65536 bytes
entropy-replay / entropy-missing / entropy-exhausted / entropy-mismatch / entropy-partial
15 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
68 mandatory workloads per platform / ARM64 204 / Intel 136
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU getentropy ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU generation/copyout boundary](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/dev/random/randomdev.c).

## 明示的な現在のスレッド識別子

BSD thread_selfid372 は不変の省略可能な `DarwinSystemOptions::ThreadID` / JSON `darwin_system.thread_id` を読み取ります。ゼロを含む全 uint64 ビット列を既知の観測として扱い、省略時は UnsupportedService で停止します。十進文字列は64ビットすべてを保持し、JSON 数値は2^53-1以下の正確な整数に限られます。ホスト、PID、Mach ポートから ID を推測しません。引数なし呼び出しは六つの引数を無視し、メモリにアクセスしません。既存の下位32ビット解析はイベントの元の番号全体を保持し、BSD 戻り値処理は64ビット結果と carry、RDX/X1 のクリアを担当します。Mach 番号は未対応です。

再実行でも入力観測を保持し、別のオプションは独立です。ID の割り当て、一意性、スケジューラのイベント識別、スレッドのライフサイクル、pthread、TLS、Mach IPC は実装しません。ARM64 O0/O1/O2 の原生プローブは24回の呼び出しで SDK の現在の pthread ID、任意引数と番号の上位32ビットを確認します。共通の原生プログラムは同じプロセス内の関係だけを比較し、指定 ID のバイト列は決定的な原生参照一覧から除外します。Intel HVF、実機 iOS、完全な OS 互換性は未検証または未完成です。

```text
BSD thread_selfid372 / Wide / ThreadID / darwin_system.thread_id
known uint64 including0 / missing -> UnsupportedService / no memory
full64 return / low32 resolution / carry clear / RDX-X1 zero / raw event number
thread-identity / thread-identity-value / thread-identity-missing
4 model cases / 20 transport parameters / 16 public cases / 5 Python profiles
69 mandatory workloads per platform / ARM64 207 / Intel 138 unverified
original ARM64 O0/O1/O2 probes24 / native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU thread_selfid ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [libpthread current-thread owner](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/kern/kern_support.c).
