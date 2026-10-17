**言語**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 9fc13e22f8389ee3a151e7b2f022bbf58f728e5597159984b0585f1b88cbb48e -->

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

`LC_MAIN` は `argc`、`argv`、`envp`、apple ベクターを四つの整数引数で受け取り、戻り値の下位八ビットが終了ステータスになります。`/usr/lib/dyld` は下記の有限インポート契約を満たす `LC_MAIN` の引き渡しで許可し、ホストの dyld は実行しません。非ゼロの `stacksize` は拒否します。予算を決めるのは呼び出し元の `stack_size` です。

`LC_UNIXTHREAD` は PC だけが設定された完全なネイティブ 64 ビット汎用レジスタレコードを一つだけ許可します。初期スタックは argc、終端付き argv/envp、`executable_path=<input filename>` を含む終端付き apple ベクターで構成します。独自 SP/フラグ、他のレジスタ、追加 flavor、矛盾するエントリを拒否し、ホスト環境や Linux 補助ベクターを継承しません。[dyld の設計](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md)を参照してください。

以下の有限 libSystem、明示的な起動 dylib、起動コンストラクタ、メインスレッド C/C++ TLS、プロセス終了時の C/C++ デストラクタに対応します。選択的 DSO 終了処理、guard を使うローカル静的変数の初期化、旧 TLS 初期化セクション、arm64e/PAC、未対応 CPU サブタイプ、暗号化、未モデル化コマンドは実行前に拒否します。主実行形式は優先アドレス、dylib は決定的な独立アドレスを使います。署名メタデータは AMFI や entitlement を実装しません。

## Darwin サービス

BSD 呼出しの ARM64 は X16、X0–X5 と `svc #0x80`、x64 は BSD クラス `0x02000000`、RAX、RDI/RSI/RDX/R10/R8/R9 を使います。成功は carry を消し、エラーは carry と正の errno を返します。ARM64 は X1 を消し、x64 は成功時に RDX を消してエラー時には保持します。SYSCALL が変更するレジスタは明示されています。レポートの `result` と `error=true` は BSD エラーを表します。戻らない要求や未対応要求は JSON で `result: null` を持ち、`error` を省略します。XNU の [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c)、[x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c) の規則に基づき、Apple の実装コードは取り込んでいません。

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

任意CredentialsはRealUID/EffectiveUID/RealGID/EffectiveGIDと独立任意GroupAccessListとGroupMembershipUID。省略時4照会1000、明示的0/rootは有効、4個のスカラーIDとグループ項目0..INT32_MAX。GroupMembershipUIDの独立範囲は後述。グループ1..16、先頭EffectiveGID、順序/重複保持。欠落は未知でhost/EGIDから補完しません。厳密darwin_system.credentialsはreal_uid/effective_uid/real_gid/effective_gid必須、groupsとgroup_membership_uidは独立任意。無損整数/中央検証で形/項目/範囲/数/先頭不一致をロード前拒否、非Darwinも拒否。

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

## 明示的な Mach 自己ポート観測

生の Mach thread_self_trap27、task_self_trap28、host_self_trap29 は独立した任意の uint32 観測 DarwinSystemOptions::ThreadSelfPort、TaskSelfPort、HostSelfPort を読みます。JSON は darwin_system.thread_self_port、task_self_port、host_self_port です。各問い合わせは選択したフィールドだけを必要とします。欠落は未知として UnsupportedService で停止し、ゼロ、同じ名前、全32ビットパターンは明示値です。UINT32_MAX までの正確な整数と十進文字列を受け入れ、process_group_id/session_id の正の pid_t 制約は適用しません。

システム所有層は符号付き int32 のカーネル結果を通して raw64 に変換します。0x80000001 は0xffffffff80000001、UINT32_MAX はUINT64_MAX になります。既存 Mach バインドはフラグと X1/RDX、x64 RCX/R11 の上書き規則を保ち、引数を無視してメモリに触れません。低32ビットで解決してもイベントには完全な生番号が残ります。Mach 問い合わせは BSD error を省略し、スケジューラ ThreadID を生成しません。再実行と独立した設定は観測を変更しません。

元の ARM64 O0/O1/O2 準備は432回の生観測、全16 NZCV、高32ビット番号、レジスタ種子、SDK 比較を保存します。原生 bit31 ポート名は観測していません。高位の符号拡張は固定 XNU 戻り経路に基づき、独立したリテラルのモデル・ゲスト・公開テストで検証します。共通ネイティブプログラムは同一プロセス内の関係だけを比較し、仮想名と欠落モードは確定的ネイティブ参照に含めません。名前・送信参照の割り当て、権利認証、一意性、IPC・寿命・スケジューリングは提供しません。権限/ACL、準備待ち、進行時計、実際の Mach IPC/スレッド、dyld/TLS、完全なランタイム/フレームワークは未完成です。ネイティブ Intel HVF と実機 iOS は未検証です。

```json
{"darwin_system":{"thread_self_port":2147483649,"task_self_port":0,"host_self_port":"4294967295"}}
```

```text
Mach thread_self_trap27 / task_self_trap28 / host_self_trap29
ThreadSelfPort / TaskSelfPort / HostSelfPort / uint32 / signed-int32 -> raw64
known0 / missing -> UnsupportedService / no arguments or memory
low32 resolution / complete raw number / flags and RDX-X1 preserved / no BSD error
mach-self-ports / mach-self-port-values / mach-self-port-missing
MachSelfPortsPreserveExplicitBitsAndIndependentRuns
6 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
70 mandatory workloads per platform / ARM64 210 / Intel 140 unverified
original ARM64 O0/O1/O2 probes432 / no native bit31 name observed
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU Mach trap table](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/syscall_sw.c), [self-port name owners](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_tt.c), [host-port owner](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_host.c), [ARM64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [x64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/i386/bsd_i386.c).


## 静的な通常所有者の権限照会

darwin_files.authorization="static-owner-queries"（DarwinFileAuthorization::StaticOwnerQueries）は不変の通常ローカル環境を明示します。ACL、MAC、追加 kauth リスナー、entitlement と他の権限迂回を除外し、所有権が有効な書込可能・実行可能・非 opaque マウント、flags=0、特殊モードビットなしを宣言します。従来の観測だけでは権限を与えず、対象は access/faccessat の照会です。

実際の検査には明示した darwin_system.credentials と実際のオブジェクトのメタデータが必要です。access は real_uid、AT_EACCESS は SEARCH と最終 R/W/X の両方で effective_uid を使います。選択 UID は非ゼロの所有者で、要求した全 owner ビットが必要です。既知の拒否は EACCES13 と従来の BSD carry。未知の資格情報/メタデータ、選択 UID0、非所有者、拡張動作、保持した末端リンクの R/W/X は UnsupportedService。選択しない UID0 は有効です。ホスト、UID1000、グループ/その他、root 例外は推測しません。

親ディレクトリの X/SEARCH は子検索、欠落名、該当するドット/ドットドット、リンク再開より前です。F_OK と無視ビットは実際の SEARCH のみを要求します。スラッシュだけの root と root で制限されたドットドットは SEARCH なし。消費した末尾スラッシュは最終 SEARCH を追加しません。フラグ、コピー、相対 dirfd、空名のエラー順を維持。Name255 は利用可能性の上限で、許可 SEARCH 後の超過は Unsupported、拒否は先に EACCES、未知は先に停止し、ファイルシステム errno を推測しません。

他の全ファイル操作（open、stat、chdir、readlink、属性、列挙、名前空間変更）は効果前に停止。実際の Input/Output/Error と dup 別名だけが既存 I/O、close、dup/dup2、lseek、fcntl を維持し、FD0/1/2という数値では免除されません。ファイル mmap/mappingSource は閉じ、匿名メモリは独立。共有 C++/JSON は変更/作成権限、既知 flags/特殊ビット、inode/device 別名を拒否し、未知データと256項/16MiB計上は保持します。

例は root SEARCH と0400所有者ファイルを示し、読取り検査は成功、書込みは EACCES、open は非対応です。ARM64 O0/O1/O2 は2472 raw/SDK組、2439独立リテラル、33観測のみを compile120s/native5s で保持。fstatx/filesecで ACL 欠省を確認。初回 NULL/ENOENT 誤判定は権限照会前で記録を保持。ネイティブの real/effective は同一で、異なる選択は固定ソース/モデルの証拠です。全グローバルフック/opaque 内部は未検証。owner-queries は58決定的ネイティブ参照に含めません。グループ/root/ACL/MAC、動的資格情報、一般 vnode 権限、待機、時計、Mach IPC/スレッド、dyld/TLS、完全なランタイムは未完成。Intel HVF と実機 iOS は未検証です。

```json
{"darwin_files":{"authorization":"static-owner-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":33024,"link_count":1,"uid":501,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16832,"link_count":2,"uid":501,"gid":20,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20}}}
```

```text
DarwinFileAuthorization::StaticOwnerQueries / authorization=static-owner-queries
access33 / faccessat466 / real_uid / effective_uid / AT_EACCESS0x10
owner R/W/X / all requested bits / directory SEARCH / EACCES13
no-action root LOOKUP / root-clamped dotdot / consumed terminal separators
unknown credentials-metadata-root-nonowner -> UnsupportedService
all other vnode routes closed / typed standard streams and dup aliases only
anonymous memory independent / file-backed mmap and mappingSource closed
Name255 availability stop after allowed SEARCH / no guessed filesystem errno
owner-queries / owner-query-stop / owner-query-open / owner-query-map
OwnerQueriesKeepPermissionAndUnknownBoundaries
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 pairs2472 / literal2439 / capture-only33
native5s / compile120s / owner-build1200s / guest-Python5,000,000us
```
[XNU access and subject selection](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [real credential copy](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [owner authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [pathname SEARCH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c), [cached lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_cache.c), [Libc ACL properties](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/gen/filesec.c), [fstatx ACL absence](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/statx_np.c).

## グループ情報が部分的に既知の静的な通常クエリ

`darwin_files.authorization="static-ordinary-queries"`（DarwinFileAuthorization::StaticOrdinaryQueries）は、前節の不変な通常環境、マウント・セキュリティ・メタデータ条件、閉じたファイル操作、標準ストリーム例外、独立した匿名メモリを維持します。旧 static-owner-queries は所有者限定のままです。実際の検査には明示的な資格情報、メタデータ、非ゼロの選択 UID が必要です。root、拡張権限、最終リンクの R/W/X は未対応です。

所有者は要求した全所有者ビットを使います。非所有者では要求マスク全体についてグループとその他の結果を比較し、一致すればグループ照会なしで許可または EACCES13 を確定します。異なるビット集合も同じ拒否になり得ます。不一致なら既知のメンバーはグループ、証明された非メンバーはその他を使い、未知なら検索や効果の前に UnsupportedService で停止します。権限クラスは混合しません。

credentials.groups は順序付きのカーネル資格情報リストで、先頭は EffectiveGID、重複を保持します。SDK getgroups の拡張リゾルバリストとは別です。選択された主グループと明示的な正の所属は既知ですが、欠落やリスト省略は通常、非所属を証明しません。実 UID/GID の両組が有効値と一致すれば元の文脈を保持し、そうでなければ先頭を RealGID に置換し、最初に一致する補助 RealGID の位置へ旧 EffectiveGID を移します。一致がなければ旧主グループを除いて memberd を無効化します。この証明された置換または明示的な元の KAUTH_UID_NONE と、明示的な完全リストを組み合わせると、欠落の否定が確定します。GID が等しく UID が異なる場合も変換し、主グループの重複は外部所属を未知のままにできます。AT_EACCESS は元の有効文脈を使い、入力は不変です。

以下では GID20 の置換により実クエリが拒否され、有効主グループ20を使う AT_EACCESS は成功します。ディレクトリ SEARCH はグループとその他の結果一致で確定し、選択 UID0 や open を許可しません。

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","metadata":{"device":7,"inode":2,"mode":32816,"link_count":1,"uid":700,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}},"bytes_hex":"00"}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":502,"real_gid":30,"effective_gid":20,"groups":[20,40]}}}
```

読み取り専用の原生 ARM64 O0/O1/O2 検証は270組の raw/SDK 呼び出しを保持し、非所有オブジェクト、SEARCH、エラー、同一性、独立した ACL プロパティ不在を確認します。raw 資格情報グループは16、SDK 拡張リストは17です。初回の SDK 長制限の誤りは権限照会前に停止し、失敗記録を保持します。実/有効 ID は同じため、異なる ID と完全なグループ変換は固定 XNU ソースと独立モデルの証拠であり、全外部フックの原生証明ではありません。5ソフトウェア/3 ARM64 HVF 構成で実ゲスト、C/CLI/Python、未知の ID/メタデータ/root/所属を検証します。供給モデルは元の58原生共通ケースに追加しません。完全なグループ解決、root、ACL/MAC、一般 vnode、動的資格情報、準備状態/ネットワーク、進む時計、Mach IPC/スレッド、dyld/TLS、完全なフレームワークは未完成で、原生 Intel HVF/実機 iOS は未検証です。期限は不変です。

```text
DarwinFileAuthorization::StaticOrdinaryQueries / authorization=static-ordinary-queries
owner bits / whole-mask group-world outcomes / EACCES13
credentials.groups / in-credential16 / EffectiveGID index0 / duplicates retained
real credential copy / first supplementary match / displacement disables memberd
missing membership usually unknown / original NONE or displaced real plus complete list proves negatives
all43 other file routes and direct/file-backed mappings closed / typed streams only
ordinary-queries / ordinary-query-unknown / ordinary-query-open / ordinary-query-map
OrdinaryQueriesPreserveGroupKnowledgeAndSelectedSearch
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 nonowner pairs270 / raw-groups16 / SDK-extended-groups17
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU ordinary mode authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [real credential and group membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [raw in-credential getgroups](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c), [SDK extended getgroups](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/getgroups.c).

## 明示的な元のグループ所属文脈

任意の `DarwinCredentials::GroupMembershipUID` / `darwin_system.credentials.group_membership_uid` は元の cr_gmuid を宣言し、4個の身分 ID と groups から独立しています。0..INT32_MAX または正確な KAUTH_UID_NONE=4294967195（0xffffff9b、UINT32_MAXから100を減算）を許可します。既存の無損十進文字列・正確な数値整数デコーダを使い、不正な型、小数、負数、その他の範囲外値をロード前に拒否します。通常 UID/GID とリストにはこの番兵を許可しません。省略と他の有効 UID は外部所属の否定を与えず、リゾルバを実装しません。

選択主グループと明示的な正の所属を先に判定します。元の NONE と明示的な完全リストがあれば欠落は非所属ですが、リスト省略は未知です。実資格情報へのコピーは最初の補助一致で旧有効主グループを保持する場合も元の NONE を保持し、証明された除去も外部解決を無効化します。スカラー照会、raw getgroups、作成所有権と従来モードは変わりません。同じ普通照会所有者がその他の権限を選び、子の検索前に SEARCH を確認し、他の vnode 操作を閉じます。

例の呼出元は GID50 の非所属で、両身分は /data を読めますが、書込権限照会は EACCES13 です。group_membership_uid を省略すると異なるグループ/その他の結果は未対応です。

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":32772,"link_count":1,"uid":700,"gid":50,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20,"groups":[20],"group_membership_uid":4294967195}}}
```

ローカル O0/O1/O2 SDK 実行は番兵と4バイト uid_t のみを検証し、ホスト cr_gmuid やリゾルバ状態を観測しません。先の実 raw/SDK 非所有者270組とは別です。ordinary-queries-closed-groups は173イベント、有効非所属の拒否、欠落・ドット・ドットドット・リンクの SEARCH を5ソフトウェア/3必須ARM64 HVF設定と C/CLI/Python で検証し、58 native-common 参照には含めません。root、ACL/MAC、完全グループ解決、一般 vnode 認可、動的資格、待機/ネットワーク、進行時計、Mach IPC/スレッド、dyld/TLS、完全フレームワークは未完成です。Intel HVF/実機 iOS は未検証、既存制限は不変です。

```text
GroupMembershipUID / group_membership_uid / original cr_gmuid
0..INT32_MAX or KAUTH_UID_NONE=4294967195 / 0xffffff9b / not UINT32_MAX
positive entries first / original NONE plus complete list proves negatives
omitted list unknown / first-match real copy preserves original NONE
ordinary-queries-closed-groups / 173 events / stdout GN
OrdinaryQueriesUseExplicitMembershipUIDWithoutResolver
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
SDK constant O0/O1/O2 only / prior actual nonowner pairs270 remain separate
58 native-common references unchanged / Intel and physical iOS unverified
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU KAUTH_UID_NONE](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/kauth.h), [XNU credential membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c).

## 通常ファイルの即時準備状態

raw BSD poll(230) と poll_nocancel(417) は timeout low int32=0 を扱います。nfds は uint32 で、OPEN_MAX10240 超過はポインタ参照前に EINVAL22。正数には明示的 darwin_system.resource_limits の resource8 Current が必要です。Current 超過かつ FD_SETSIZE1024 超過は EINVAL、それ以下は元の明示的な実効 UID を要求し、非ゼロは EINVAL、ゼロは許可します。root 例外はソース/モデル証拠のみ。DescriptorLimit、kern.maxfilesperproc、FD 数値、実行予算はこの観測を与えません。ゼロ件は配列に触れず、非ゼロ timeout は件数判定後に未対応です。

構築された仮想通常ファイルは非撤回、通常読書きフィルタの登録成功、MAC/提供元拒否なしを明示する契約です。native vnode の種別だけでは登録成功を証明できません。この前提で IN/RDNORM と OUT/WRBAND は EOF や読み取り専用 open でも準備完了で、残量・アクセスフラグ・O_NONBLOCK・書込み許可から推測しません。数値 FD と読/書別に最後の行を保持し、異なる dup FD は別です。HUP 単独は読フィルタを登録しますが準備ビットを出しません。負 FD/無視ビットのみはゼロ、閉じた登録は各行 POLLNVAL32。live OOB/vnode、ストリーム、ディレクトリ、リンクは出力前に未対応です。8バイト記録を全入力取得し、不完全入力/全出力不可は EFAULT14、部分出力は接頭部コピーなしで拒否。全コピーは fd/events を保存し revents 全体を置換。静的権限照会2モードはゼロ件も前処理前に Poll を拒否し、共有閉鎖一覧は42経路です。

SDK 不要の immediate-poll は86イベントと元の59件一覧を保持し、immediate-select の追加でselect 段階の共通ネイティブ一覧は60件です。独立した ARM64 O0/O1/O2 の poll582件と select792件は、リテラル ABI/エラー制御と提供者の観測を別々に保持し、普遍的定数ではありません。過去58/59件のソース識別、失敗、期限を保持します。Intel HVF と実機 iOS は未検証です。失効/MAC/提供者拒否、待機、pselect のシグナルマスク変更、非同期、ネットワーク、進行する時計、実際の Mach IPC/スレッド、dyld/TLS、完全なフレームワークは未完成です。

```text
poll230 / poll_nocancel417 / timeout low int32=0 / nfds uint32
ResourceLimits[8].Current / OPEN_MAX10240 / FD_SETSIZE1024 / explicit original effective UID
constructive regular provider: nonrevoked / successful ordinary filter attachment / no MAC-provider refusal
numeric FD + read/write filter / independent last requested index / distinct dup aliases
IN1 RDNORM64 OUT4 WRBAND256 / HUP16 trigger only / closed registrations POLLNVAL32
negative and ignored-only rows zero / whole input snapshot / whole output / partial output unsupported
EFAULT14 EINVAL22 / no ready prefix on unsupported row / zero count no pointer
static-owner-queries and static-ordinary-queries / all57 other file routes closed before preflight
immediate-poll86 events / five software + three mandatory ARM64 HVF profiles / C CLI Python
64 current native-common cases / prior58/59 source identities preserved / 582 mixed controls and captures
native Intel and physical iOS unverified / select waits pselect signal-masks revoked-MAC-provider failures networking unfinished
```

[XNU poll ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/poll.h), [poll registration and copy order](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [vnode registration and regular filters](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## 通常ファイルの即時 select

BSD `select`93 と `select_nocancel`407 は、明示したゼロ timeval と符号付き下位32ビットの非負件数だけを受け付けます。ポインターは64ビットを保持します。16バイト timeval はオフセット0の符号付き64ビット秒、8の符号付き32ビットマイクロ秒、12の無視するパディングです。秒0..100000000、マイクロ秒0..999999を検証してから負件数の EINVAL22 を処理し、不完全な読み込みは EFAULT14 です。null または正のタイムアウトは未対応、件数0では集合にアクセスしません。最大の現行 FD 加1は `fd_nfiles` の下限だけを証明します。高い FD の close は証明を狭めますが、ネイティブ容量の縮小を意味しません。NOFILE、予算、vnode の種類は容量や提供者の証明になりません。

ビット集合はリトルエンディアン32ビット語に丸めます。全入力のコピー、全選択 FD の検査、提供者の解釈の順序です。不在 FD は EBADF9、選択したストリーム、ディレクトリー、シンボリック記述子は未対応です。構築された通常記述子は、副作用や拒否がなく読取り・書込み・例外で非ゼロを返す select 提供者を宣言します。poll とは独立した前提です。dup の数値別、集合別のビットを数え、読取り専用・非ブロッキング・EOF でもオフセット、フラグ、内容、メタデータを変えません。コピー語の末尾ビットを消し、範囲外は保持します。

出力同士と16バイト timeval との重複は禁止します。全出力の権限を先に検査し、書込み不能な先頭と書込み可能な末尾を含む部分出力は全コピー前に拒否します。書込み可能な先頭長0だけでは全域不能を証明できません。全域コピーは読取り・書込み・例外の順で、全域不能なら EFAULT14、後続の書込み可能集合は更新し、最初のエラーを保持します。静的権限照会の両モードは事前検査前に Select を閉じ、合計43経路を閉じます。SDK 不要の115イベントを五つのソフトウェア、必須三つの ARM64 HVF、C・CLI・Python と完全なガードで検証します。待機、非 NULL の `pselect` シグナルマスク、非同期提供者、完全な OS は未完成です。

```text
select93 / select_nocancel407 / zero supplied user64_timeval16 / signed low32 count
current live FD lower bound / no guessed fd_nfiles allocation or NOFILE capacity
read write exception / bit count across sets / distinct dup numbers
all copyins before all descriptor checks / all checks before provider interpretation
partial outputs and overlaps unsupported before all output / zero prefix is not whole unwritable
ordered read write exception copyout / first EFAULT14 after later whole writable copies
792 mixed controls and captures / 540 literal controls and 252 provider captures / 225 provider projections
immediate-select115 events / five software + three required ARM64 HVF / C CLI Python
64 current native-common cases / 76 mandatory workloads per platform / ARM64228 / Intel152 unverified
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU select and copy order](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [FD mask ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/_types/_fd_def.h), [user64 timeval](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h), [ordinary select provider](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

即時 `pselect394 / pselect_nocancel395` は、指定されたゼロ値の `user64_timespec16` と null のシグナルマスクポインタを受け付けます。両フィールドは signed64 で、秒は 0..2147483647、ナノ秒は上限を含む 0..1000000000 です。有効な非ゼロタイムアウトの待機は未対応です。timespec の読み込みと検証、4 バイトのマスク読み込み、カウントとビットマップの検査の順に処理します。読み込めないマスクは EFAULT14、完全な非 null マスクはスレッドのシグナル状態の置換と復元が未実装のため停止します。null マスク経路は select の容量、提供者、ビットマップ、出力権限と順次コピーの規則を共有します。シグナル、待機、dyld、Apple フレームワーク、実機 iOS の検証は未完了です。

```text
pselect394 / pselect_nocancel395 / zero supplied user64_timespec16 / null signal mask
signed64 seconds0..2147483647 / signed64 nsec0..1000000000 inclusive
time import validation before mask4 import before signed low32 count
complete nonnull mask unsupported / no guessed signal state
shared select bitmap capacity provider output owner
918 mixed controls and captures / 729 literal controls and 189 provider captures
```

## 明示的な通常ファイルのモード変更

chmod15、fchmod124、fchmodat467 は mode_mutations の初期通常ファイルと固定仮想 ctime を使用します。空の objects は環境のみを宣言し、provider 成功を付与しません。省略は未知で、静的クエリ環境と併用できません。ACL、MAC、追加リスナー、特権の回避、失効、ファイル/ディレクトリ lease と変更前の待機・通知を除外します。VM mapping lease と他の操作の権限は独立です。

唯一の認可層は完全な現在 stat と要求を比較し、明示された非 root の実効資格情報を使います。非所有者の異なるモード要求は EPERM、同じモードは所有権だけでは拒否できません。ただし成功には常に物体固有の provider grant が必要です。パスは実際の現在ディレクトリごとに SEARCH を検査し、拒否の EACCES は欠落名より先です。未知のグループ・資格情報・記録は未対応です。読み取り専用 FD はパス SEARCH が不要です。成功は通常権限と ctime だけを変更し、バイト、その他の記録、カーソル、属性、保存量、VM 寿命を保ちます。grant は dup、ハードリンク、rename、保持された unlink 物体に追従し、新規物体や再利用名に継承されず、無効な記録を復元しません。

不正な低32フラグの EINVAL はポインタ/FD より先で、独立した入力エラーと絶対パスの dirfd 無視は共通順序を保ちます。NOFOLLOW は終端リンクを保持し ANY は前段の展開を禁じます。ディレクトリ、リンク、特殊モード、選択 root は未対応です。54の原生呼び出しイベントと31の保護 statを検査します。mainの戻り値37は原始exitイベントを追加しません。所有者の450観測は非所有者の原生証拠ではありません。63/78一覧、ARM64一致234件は実行成功が必要です。Intel原生、実機iOS、一般権限、完全なOS/フレームワークは未完です。

```text
chmod15 / fchmod124 / fchmodat467 / low32 mode & ALLPERMS07777
mode_mutations / objects / path / time / fixed virtual ctime
effective explicit credential / actual current directory SEARCH
ordinary security attributes / no ACL MAC file-directory leases or lifecycle effects
same current permissions: no WRITE_SECURITY / nonowner not denied solely for ownership
different current permissions: WRITE_SECURITY / nonroot nonowner EPERM1
object provider grant still required for every success / no grant inference
stat current-validity owner / invalidated records never restored
type bytes offsets xattrs storage and VM mapping leases preserved
AT_SYMLINK_NOFOLLOW0x20 / AT_SYMLINK_NOFOLLOW_ANY0x800
flags EINVAL22 before path EFAULT14 / relative bad FD EBADF9 / absolute FD bypass
empty declaration supplies environment without object provider grants
missing names ENOENT2 only after all required SEARCH / denied SEARCH EACCES13
SDK-free54 service records / 54 native calls / 31 guarded stat records / main return is not a raw exit event
OrdinaryModesKeepCurrentMetadataAuthorityAndObjectState
64 current native-common cases / 79 mandatory workloads per platform
ARM64 matching native mandatory237 / native Intel and physical iOS unverified
450 original owner observations / 414 literal controls and 36 final-link provider captures
nonowner authorization is source/model evidence, not owner-native evidence
all57 other file routes closed before preflight / static query declarations incompatible
native guest5s / compile120s / build-C++1200s / Python-config300s / docs-caps120s
```

[XNU chmod/fchmodat/fchmod](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU current-mode authorization and lease lifecycle](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c).

## 明示的な通常ファイルの所有権変更

ownership_mutations は初期通常ファイルごとの provider と固定仮想 ctime を宣言します。空の objects は環境だけを宣言し、省略は未知です。mode_mutations とは独立で、静的クエリ環境とは併用できません。現在の完全な属性と明示的な非 root 実効資格情報を使い、第三者 UID の拒否、対象グループの既知の所属、TAKE_OWNERSHIP、所有者の WRITE_SECURITY の順に判定します。未知の所属は未対応です。低32 UINT32_MAX は非アクティブです。両方が非アクティブでも provider が必要ですが ctime を含む記録は不変です。アクティブな ID は同じ値でも宣言 ctime を設定します。実際の現在ディレクトリの SEARCH を確認し、fchownat は NOFOLLOW0x20 のみを受け付け、ANY0x800 は EINVAL です。共有物体は grant を保持し、新規物体や再利用名は継承せず、無効な記録は復元しません。他のデータ、名前空間、属性、保存量と VM lease は独立です。以下は境界と検証要件です。所有者の原生観測は非所有者の証拠ではなく、Intel 原生、実機 iOS と完全な OS/フレームワークは未完です。

```text
chown16 / fchown123 / fchownat468 / low32 UID GID / UINT32_MAX inactive
ownership_mutations / objects / path / time / independent provider authority
explicit effective nonroot credentials / actual current directory SEARCH
changed third-party UID EPERM1 before target-group lookup
changed GID: known membership required / unknown membership unsupported
TAKE_OWNERSHIP == CHANGE_OWNER / no ordinary POSIX fallback
changed UID to caller EPERM1 / group-only WRITE_SECURITY requires current owner
same IDs: no change-derived security rights / provider still required
both IDs inactive: provider still required / entire record and ctime unchanged
any active ID: fixed virtual ctime even for numerically same IDs
AT_SYMLINK_NOFOLLOW0x20 only / ANY0x800 EINVAL22 before path or FD
initial regular object grants / dup hardlink rename held unlink retain identity
creation and name reuse do not inherit grants / invalidated records never restored
mode data namespace xattr storage and VM mapping leases keep independent contracts
SDK-free77 service records / 77 authored raw calls / 41 guarded stat records
separate main return38 / OrdinaryOwnershipKeepsCurrentAuthorityAndObjectState
64 current native-common cases / 79 mandatory workloads per platform
ARM64 matching native mandatory237 / actual successful execution required
720 owner observations / 702 literal controls and 18 final-link provider captures
nonowner authorization is source/model evidence, not owner-native evidence
all57 other file routes closed before preflight / static query declarations incompatible
no ACL MAC listener bypass revocation native file-directory leases or setattr lifecycle
native Intel and physical iOS unverified / full macOS-iOS environment incomplete
```

[XNU ownership routes](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ownership actions](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [XNU vnode_setattr provider](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/kpi_vfs.c).


## 明示的な通常ファイル全体の助言ロック

flock131 は advisory_locks の一意な objects 文字列で初期通常ファイルの provider を宣言します。省略は未知、空配列は物体を許可しません。ロックは開いた記述に属し、dup、fcntl 複製と dup2 は所有者を共有します。最後の記述子を閉じると解除され、VM マッピングはロックを保持しません。UN > EX > SH、非ブロッキング競合 EAGAIN35 は元の SH を保持します。競合する待機はモデル変更前に未対応で停止します。リンクや名前変更は物体の許可を保持し、新規物体や再利用名は継承しません。以下は検証の境界です。Intel 原生、実機 iOS と完全な OS は未検証または未完です。

```text
flock131 / Int ABI / low32 FD and how / raw high carriers retained
advisory_locks / objects / explicit initial ordinary regular-object provider
missing environment unknown / empty objects no provider / initial locks empty
complete initial metadata / flags0 / ordinary mode / link_count1 / no initial inode alias
path+NUL shares input byte budget / strict unique string array / unknown keys rejected
FD lookup first / known vnode without action EBADF9 / unrelated action bits ignored
UN8 > EX2 > SH1 / NB4 selects nonblocking / readonly EX allowed
whole-file description owner / dup fcntl duplication dup2 share one owner
separate open descriptions arbitrate / shared-shared compatible / exclusive conflicts
nonblocking conflict EAGAIN35 before old-lock removal / failed upgrade keeps SH
success replaces upgrade or downgrade / uncontended blocking completes immediately
contended blocking unsupported before model mutation / no wait or unlock fabrication
current descriptor membership owns lock lifetime / last duplicate close releases lock
VM mapping Contents leases do not retain description locks
runtime hardlink rename held zero-name unlink keep object grant
created or reused names do not inherit provider grants / path registry not lock authority
flock preserves bytes cursors stat xattrs and allocation / successful lock sets persistent FWASLOCKED0x4000
mode ownership data namespace policies independent / invalidated records never restored
all57 other file routes closed before preflight / either static query rejects even empty declaration
SDK-free77 service records / 13 guarded full stat observations / separate main return39
OrdinaryAdvisoryLocksKeepDescriptionAndObjectLifetime
64 current native-common cases / 79 mandatory workloads per platform
ARM64 matching native mandatory237 / actual successful execution required
720 literal native lock observations / 522 action controls and198 lifetime controls
native O0 O1 O2 / raw SVC and syscall / three high-carrier variants
no external or interprocess peers / no POSIX OFD range locks / no fork or descriptor transfer
no MAC listener revocation provider error native-lease or blocking-wait lifecycle
native Intel and physical iOS unverified / full macOS-iOS environment incomplete
```


## 通常ファイルの明示的な整合性同期完了

fsync95 と fsync_nocancel408 は、取消要求のない既存の単一スレッド設定で Fsync/Int の所有者を共有します。DarwinFileOptions::Synchronization または厳格な darwin_files.synchronization={"objects":["/data"]} は、成功済みのモデル変更が確定し、保留作業や取消要求のない即時成功プロバイダーを宣言します。ゼロはその宣言された完了を確認するだけで、ホスト記憶装置のフラッシュや物理的な永続性を保証しません。XNU の通常入口は先に取消を確認し、nocancel 入口は確認しません。一般的な取消、プロバイダー障害、リース、非同期待機は未対応です。

```text
fsync95 / fsync_nocancel408 / Fsync Int owner / low32 FD / raw high carriers retained
synchronization / objects / explicit immediate successful file integrity completion
successful modeled mutations already committed / no deferred provider work or requested cancellation
zero acknowledges declared provider / no host storage flush / no physical durability claim
normal XNU entry tests cancellation / nocancel entry does not / full cancellation unsupported
complete ordinary initial regular objects / flags0 / link_count1 / no initial inode alias
unique strings / at most256 objects / canonical path+NUL shares catalogue byte budget
missing environment unknown / empty objects no completion grant
FD lookup first / absent or closed descriptor EBADF9 even without environment
opaque streams unsupported kind / directory and symlink descriptions need independent providers
readonly regular description accepted only with explicit object completion grant
Contents owns grant / dup fcntl dup2 select current description and object
runtime hardlink rename held zero-name unlink retain original grant
created and reused names do not inherit initial completion grants
VM mapping and retained Contents cannot make closed descriptors valid
bytes cursors flags stat xattrs allocation namespace and advisory locks unchanged
unknown metadata and xattrs remain unknown / no observation restoration
all57 other logical file routes closed before preflight / both static queries reject empty declaration
one alias shares owner / no extra query route for fsync_nocancel
SDK-free68 service records / 20 guarded full stat observations / separate main return40 / stdout S
OrdinaryFileSynchronizationKeepsProviderAndCommittedState
68 current native-common cases / 83 mandatory workloads per platform
ARM64 matching native mandatory249 / actual successful execution required
504 original native literal observations / O0 O1 O2 / raw SVC and syscall / three high carriers
176-byte guarded native stat and cursors unchanged / actual native times
file/data grants never supply F_FULLFSYNC51 F_BARRIERFSYNC85 / global sync remains unsupported
no provider error resource-fork shadow-flush lease revocation wait or full cancellation lifecycle
native Intel and physical iOS unverified / full macOS-iOS environment incomplete
```

[XNU fsync paths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU descriptor lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU syscall declarations](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master).


## 通常ファイルの明示的なデータ整合性同期完了

fdatasync187 は独立した Fdatasync/Int 操作です。XNU は取消を確認して MNT_DWAIT を要求し、fsync95/fsync_nocancel408 は MNT_WAIT を要求します。DarwinFileOptions::DataSynchronization または厳格な darwin_files.data_synchronization={"objects":["/data"]} は、成功したモデルのデータ変更が確定し、保留作業も取消要求もない即時成功を宣言します。ファイル Synchronization とデータ DataSynchronization の許可は相互に代用できません。ゼロは宣言された完了だけを確認し、ホストのフラッシュや物理的永続性を保証しません。一般の取消、プロバイダー障害、リースと非同期待機は未知のままです。

```text
fdatasync187 / distinct Fdatasync Int purpose / low32 FD / raw high carriers retained
XNU tests cancellation and requests MNT_DWAIT / fsync95 and fsync_nocancel408 request MNT_WAIT
data_synchronization / objects / explicit immediate successful data integrity completion
DataSynchronization and Synchronization grants never supply each other's purpose
successful modeled data mutations already committed / no deferred provider work or requested cancellation
zero acknowledges declared provider / no host storage flush / no physical durability claim
complete ordinary initial regular objects / flags0 / link_count1 / no initial inode alias
unique strings / at most256 objects / canonical path+NUL shares catalogue byte budget
missing environment unknown / empty objects no data completion grant
FD lookup first / absent or closed descriptor EBADF9 even without environment
opaque streams unsupported kind / directory and symlink descriptions need independent providers
readonly regular description accepted only with explicit data object completion grant
Contents owns separate grants / dup fcntl dup2 select current description and object
runtime hardlink rename held zero-name unlink retain original grant
created and reused names do not inherit initial completion grants
VM mapping and retained Contents cannot make closed descriptors valid
bytes cursors flags stat xattrs allocation namespace and advisory locks unchanged
unknown metadata and xattrs remain unknown / no observation restoration
all57 other logical file routes closed before preflight / both static queries reject empty declarations
fsync_nocancel408 remains one Fsync alias / fdatasync187 is not that alias
SDK-free41 service records / 11 guarded full stat observations / separate main return41 / stdout D
OrdinaryDataSynchronizationKeepsProviderAndCommittedState
68 current native-common cases / 83 mandatory workloads per platform
ARM64 matching native mandatory249 / actual successful execution required
252 original native literal observations / O0 O1 O2 / raw SVC and syscall / three high carriers
176-byte guarded native stat and cursors unchanged / actual native times
directory and pipe captures do not expand ordinary initial regular-object provider
file/data grants never supply F_FULLFSYNC51 F_BARRIERFSYNC85 / global sync remains unsupported
no provider error resource-fork shadow-flush lease revocation wait or full cancellation lifecycle
native Intel and physical iOS unverified / full macOS-iOS environment incomplete
```

[XNU synchronization purposes](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU descriptor lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU syscall declarations](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master).


## 新規通常ファイルの明示的な同期完了プロバイダー

各独立宣言は created_regular_objects Boolean（C++ CreatedRegularObjects）を受け付けます。true は完全な CreationPolicy を要求し、通常ファイルの作成成功時だけ該当する即時完了を許可します。省略と false は従来の既定値を保ちます。必須 objects は初期オブジェクトだけを許可し、名前の再利用で旧許可を移しません。ファイルとデータの許可は独立です。

```text
CreatedRegularObjects / created_regular_objects / defaultfalse / mandatory objects array
synchronization={"objects":[],"created_regular_objects":true}
data_synchronization={"objects":[],"created_regular_objects":true}
true requires complete CreationPolicy / current umask / parent metadata / valid inode and mutation policy
ordinary regular creation commit only / independent File95+408 and Data187 grants
initial grants and creation grants never supply each other / existing O_CREAT receives no new grant
name reuse never transfers old authority / fresh explicit creation grant may authorize a replacement
false or omission retains old default / missing and empty environments remain distinct
single set-provider parser and input byte owner / no extra path entry FD inode or byte charge
failed creation preserves admission resources and grants / directories links streams remain unsupported
Contents lifetime through dup fcntl dup2 runtime links rename held zero-name and mapping leases
closed FD returns EBADF9 / retained Contents or VM cannot revive a descriptor
bytes stat cursor flags xattrs allocation namespace locks unchanged / no observation restoration
completion grants supply no mode ownership xattr flock or general DAC authority
both immutable query environments reject even false declarations / all57 other logical routes remain closed
SDK-free147 service records / 33 guarded full stat observations / separate main return42 / stdout C
CreatedRegularSynchronizationKeepsIndependentProvidersAndState
68 current native-common cases / 83 mandatory workloads per platform / ARM64 matching native mandatory249
702 original native literal observations / O0 O1 O2 / raw SVC and syscall / three upper carriers
freshly created target without preparatory sync / own176-byte guarded native stat and content preserved
native and model post-write F_GETFL65538 / existing FileWasWritten0x10000 / own initial flags captured and preserved
immediate successful declared provider / committed model mutations / no deferred work or requested cancellation
no host flush or physical durability / general cancellation provider errors leases waits remain unknown
native Intel and physical iOS unverified / complete macOS iOS frameworks remain incomplete
```


## 新規通常ファイルの明示的ロック許可

独立した advisory_locks は created_regular_objects（C++ CreatedRegularObjects、既定 false）を受け付けます。true は完全な CreationPolicy を要求し、作成に成功した通常ファイルだけを許可します。必須 objects は初期オブジェクトを別に許可し、既存ファイルの O_CREAT や名前の再利用で許可は移りません。成功した flock は F_GETFL に見える開いた記述の持続状態 FWASLOCKED0x4000 を設定します。解除しても履歴は残り、F_SETFL では生成も消去もできません。

```text
advisory_locks={"objects":[],"created_regular_objects":true}
CreatedRegularObjects / created_regular_objects / defaultfalse / mandatory unique objects array
true requires complete CreationPolicy / regular creation commit only / initial objects independent
existing O_CREAT and reused names transfer no authority / explicit replacement grant is fresh
single set-provider parser and admission / shared path+NUL budget / no extra entry byte FD or inode
failed creation preserves budgets and next inode / directories links streams receive no regular grant
Contents grants follow hardlink rename held zero-name and mapping / current FD lookup first
description owns lock and history / dup fcntl dup2 share / independent opens own separate history
successful flock sets FileWasLocked FWASLOCKED0x4000 / F_GETFL includes persistent description status
unlock preserves history / failed and unsupported actions do not set it
F_SETFL accepts observed history for roundtrip but cannot manufacture or clear it
symbolic recognized history returns ENOTTY25 / opaque streams zero / actual history unchanged
written created F_GETFL65538 becomes81922 after successful flock / readonly0 becomes16384
nonblocking conflict35 before old-lock removal / failed upgrade retains shared lock
last description close releases lock / VM Contents lease cannot retain lock or revive closed FD9
flock preserves bytes stat cursor xattrs allocation namespace and observation validity
mode ownership completion xattr permissions remain independent / no general DAC grant
both static query declarations reject even false environment / all57 other file routes closed
SDK-free131 service records / 12 guarded full stat observations / separate main return43 / stdout L
CreatedRegularAdvisoryLocksKeepProviderAndDescriptionStatus
68 current native-common cases / 83 mandatory workloads per platform / ARM64 matching native mandatory249
486 original native flock observations / 72 fcntl history controls / O0 O1 O2 / raw SVC and libc
162 additional native symbolic and pipe status controls / O0 O1 O2 / two APIs three carriers
whole176-byte guarded native stat / dirty bytes and current cursors preserved / literal flag transitions
failed first flag-preservation assumption retained / sole semantic owner repaired from native and pinned XNU
native5s guest5000000us quantum1024 public10s unchanged / no timing stability claim
no external peers range locks blocking waits cancellation revocation leases or host provider qualification
native Intel and physical iOS unverified / complete macOS iOS frameworks remain incomplete
```


## Explicit mode and ownership providers for created files

新しく作成する通常ファイルには、明示的な宣言によりモード変更と所有者変更の権限を個別に与えられます。両方のフラグは既定で無効であり、有効にするには完全な作成ポリシーが必要です。既存の権限は名前の再利用で移りません。成功した変更は mutation_time を仮想 ctime に使用します。所有者とグループを変更しない要求もプロバイダーを必要とし、全バイトを保持します。不明なメタデータやグループ所属は未対応のままです。


```text
mode_mutations={"objects":[],"created_regular_objects":true}
ownership_mutations={"objects":[],"created_regular_objects":true}
two independent timed creation providers / defaultfalse / mandatory objects array
created ModeTime and OwnershipTime use CreationPolicy.Mutation.Time as explicit virtual ctime
initial fixed times remain independent / existing O_CREAT grants nothing / fresh replacement only
both UINT32_MAX IDs require provider execution and preserve every byte including ctime
explicit effective groups20/31 / real GID31 alone does not prove effective membership
attribute grants cannot restore invalidated metadata or supply write namespace lock completion xattr authority
SDK-free235 service records / 137 guarded full stat observations / separate main return44 / stdout A
virtual-only nine service records / separate main return45 / 320 independent literal guarded bytes
CreatedRegularAttributesKeepIndependentProvidersAndObjectState
69 current native-common cases / 84 mandatory workloads per platform / ARM64 matching native mandatory252
450 original native mode observations / 720 original native ownership observations
native trace not captured / virtual literal bytes not a native timestamp oracle
native Intel and physical iOS unverified / complete macOS iOS frameworks remain incomplete
```


## Explicit full and barrier completion providers

明示的な完全同期とバリア同期のプロバイダーは通常ファイルの操作を独立して完了します。既存オブジェクトと新規作成オブジェクトの権限は別です。作成フラグは既定で false で、有効にするには完全な作成ポリシーが必要です。完了はモデルの全観測を保持し、未使用の引数を無視します。バリアは直接対応し、マウントによる昇格や代替処理はありません。ホストのストレージ書き出しや物理的な永続性は保証しません。


```text
full_synchronization={"objects":[],"created_regular_objects":true}
barrier_synchronization={"objects":[],"created_regular_objects":true}
F_FULLFSYNC51 / F_BARRIERFSYNC85 / existing raw fcntl92 and406 / low32 FD and command
two independent initial and new-regular providers / strict objects array / defaultfalse / complete CreationPolicy
full and barrier grants never borrow file data or each other / reused names transfer no authority
direct barrier support / no mount promotion or fallback / no host flush or physical durability
bad FD before opaque kind query scope regular target environment and exact object grant
unused argument ignored without reading memory / complete model observations unchanged
directory link and failed creation publish no regular grant / VM mappings never revive closed FDs
FullAndBarrierSynchronizationKeepIndependentObjectState
51 private test identities / pages4096 and16384 / commands51 and85
SDK-free148 authored service records / 38 guarded full stat observations / separate main46 / stdout F
virtual-only ten services / separate main47 / 320 independent literal guarded bytes / not native metadata
70 current native-common cases / 85 mandatory workloads per platform / ARM64 matching native mandatory255
216 original native raw calls / 648 guarded176-byte stat comparisons / native pipe EBADF9
native observed completion does not distinguish direct barrier from mount promotion
native Intel and physical iOS unverified / full Apple frameworks remain incomplete
```

The native constants and descriptor behavior are checked against [Apple XNU xnu-11417.140.69 kern_descrip.c](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c) and [fcntl.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h). Native non-vnode EBADF observations do not identify opaque model-native kinds.


## Explicit ordinary timestamp mutations

明示的な時刻変更プロバイダーは通常ファイルを独立に認可します。既存オブジェクトと新規作成に成功した通常ファイルの権限は別です。作成フラグは既定で false で、true には完全な作成ポリシーが必要です。NULL 要求は別途宣言した現在時刻のサンプルだけを使います。要求時刻の保存規則と固定の仮想 ctime は別の観測です。ホストの時計やファイルシステムは参照せず、完全な macOS/iOS フレームワーク環境は未完成です。


```text
DarwinFileTimestampMutationOptions / timestamp_mutations / objects / created_regular_objects defaultfalse
utimes138 and futimes139 / LP64 two timeval records32 bytes / signed64 seconds signed32 microseconds ignored padding
virtual request-storage rule / signed wide nanoseconds saturated to int64 / floor-normalized seconds nanoseconds
birth reduction only for earlier nonnegative resulting mtime including zero / negative mtime preserves birth
fixed virtual ctime is separate from request conversion and current-time sample / no host filesystem identity inferred
NULL uses only explicit TimeOfDay / zero sample valid / missing sample stays unknown / no timezone or Mach prerequisite
utimes pathname before input / futimes input before low32 FD / fully unmapped EFAULT14 / partial input unsupported
explicit effective ownership even equal times EPERM1 / current-time owner or proved ordinary POSIX write / group knowledge preserved
all57 other file routes closed before preflight / both static queries reject even empty timestamp declaration
ordinary regular Contents grants retain actual dup link rename held-zero-name lifetime / replacement names inherit none
opaque kind unsupported / native pipe EINVAL22 does not identify model kind / directories and symbolic vnodes outside provider
62 private identities / pages4096 and16384 / full160-byte stat guards / independent dirty bytes locks xattrs and real VM
TimestampMutationsKeepExplicitClockAndObjectAuthority / 124 authored common services / main48 stdout T
virtual-timestamp-creation / ten authored services / main49 / literal320 guarded bytes / explicit plus NULL request
351 accepted original ARM64 O0 O1 O2 observations / signed micros saturation and birth limits belong to captured host
native-common71 / mandatory86 per platform / matching ARM64 native required258 identities
native Intel and physical iOS unverified / original five-second native and guest bounds unchanged / full Apple frameworks incomplete
```

The original model references [pinned XNU syscall/vnode ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [ordinary WRITE_ATTRIBUTES authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). The discovered futimes number is139; the rejected206 probe is retained as failure evidence. No XNU implementation was copied.

## Explicit filesystem observations

明示的なファイルシステムのスナップショットは通常ファイルとディレクトリの容量、種類、マウント名を独立に提供します。ゼロ値と空の名前は有効で、ホストのファイルシステムを参照しません。複製した記述子、移動、削除後も開いている実際のオブジェクトに結び付きを保持し、置換された名前には継承しません。新規通常ファイルは明示的な作成フラグと完全な作成ポリシーがある場合だけ、実際の親に結び付いたスナップショットを受け取ります。ファイル環境の問い合わせは利用できますが、完全な macOS/iOS フレームワーク環境は未完成です。


```text
DarwinFileSystemSnapshot / DarwinFileSystemOptions / FileSystemObservations / filesystem_observations
statfs64 BSD345 and fstatfs64 BSD346 / LP64 record2168 bytes / legacy157158 unsupported
block_size uint32 io_size int32 / five uint64 counters / filesystem_id two int32 words
owner type flags subtype uint32 / type_name16 mount_point1024 mounted_from1024 / extended_flags uint32
reserved seven uint32 words zero / bounded UTF8 NUL-free names / all sixteen JSON members required / zero and empty valid
explicit exact immediate mount-stat provider / no host query mount identity quota live refresh MAC refusal wait or process mountpoint rewrite
initial regular directory and symbolic bindings / actual Contents DirectoryNode LinkNode / no target filestat prerequisite or recovery
created_regular_objects defaultfalse / true requires complete CreationPolicy / successful new regular uses actual bound parent snapshot
initial children unbound parents default-created directories and links receive none / no fsid device inode or pathname prefix inference
effective ordinary ancestor search uses shared authorizer and group knowledge / FD query borrows no target DAC credentials or access flags
pathname and low32 FD kind before output / ENOENT2 EBADF9 EFAULT14 / individually partial output unsupported before effects
both immutable query domains reject even empty filesystem declaration / all57 other logical file routes closed before preflight
67 private identities / both4096 and16384 pages bothroutes / full2200 guarded literal / shared16MiB budget
dup independent opens hardlinks rename directory moves CWD held-unlink reuse / dirty bytes locks xattrs and real VM independent
FileSystemSnapshotsPreserveLayoutAndIndependentObjectLifetime / 49 authored common services / main50 stdout W
virtual-filesystem-creation / nine authored services / main51 / independent4400 guarded literal bytes
114 original ARM64 O0 O1 O2 observations / readonly writeonly directory and held-zero-name FD controls / native pipe EINVAL22 opaque kind unsupported
native-common72 / mandatory87 per platform / matching ARM64 required261 identities / no captured native service trace
constant guest validation loops unrolled with every assertion retained / native5s guest5000000us quantum1024 public10s original owner bounds unchanged
native Intel physical iOS full dyld Mach IPC ObjC Swift Foundation UIKit unqualified / full Apple environment incomplete
```

Raw BSD statfs64 345 and fstatfs64 346 use the independent FileSystemObservations provider. JSON places a unique array of {"path":canonical_initial_path,"snapshot":complete_record} entries under darwin_files.filesystem_observations.objects. Complete records require block_size, io_size, blocks, free_blocks, available_blocks, files, free_files, filesystem_id, owner, type, flags, subtype, type_name, mount_point, mounted_from and extended_flags. Integer fields accept the established exact integer/string-integer forms and enforce their widths. filesystem_id is exactly two signed32 words. The three strings contain no NUL, are valid UTF8 and fit15/1023/1023 bytes; empty values are explicit. Unknown members, incomplete records, duplicate paths, unknown objects and excess shared input budgets fail before image loading. Every snapshot plus its path reference is charged2168 bytes and NUL-inclusive path cost against the shared16MiB catalogue budget. Otherwise implicit observed directories consume one catalogue entry; directories already declared by metadata, enumeration, attributes or namespace authority are counted once within the shared256-entry limit.

Path lookup follows symlinks and shares effective ordinary ancestor search, requiring only relevant current ordinary ancestor metadata and credentials. Root/special attributes and unresolved external group membership remain unsupported; no target filestat or access-mode authority is inferred. Known invalid/closed FD returns9 and missing/empty path returns2 before output preflight. Known regular, directory and symbolic FD observations need no unrelated credentials or read/write flags. Opaque standard streams remain unsupported; symbolic-vnode FDs require their own independent snapshot. A valid complete output record contains zeroed string padding and seven reserved words; fully unmapped output returns14, while individually partial output stops before effects.

Snapshots bind to actual initial regular/directory/symbolic objects, including implicit directories, and survive dup, independent opens, runtime hardlinks, rename, moved directories/CWD and held zero-name lifetime. Different or reused names inherit no initial grant. Optional created_regular_objects defaultsfalse. True requires complete CreationPolicy and explicitly binds each successful new regular file to its actual parent's already-bound snapshot, if one exists. Unbound parents, initial children, mkdir/symlink and children through new unbound directories receive none. This relation comes from the declared provider, never equality of filesystem_id/device/inode or a pathname prefix. Counters remain exactly declared observations and are not derived from bytes, allocation units or virtual storage limits.

The original model follows the [pinned XNU namei/file-vnode/mount-stat ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and the native SDK LP64 layout. No XNU implementation was copied. Native probes verify captured ABI and ordering on their owned test filesystem; dynamic capacity counts and IDs do not establish universal constants, real mount identity, quota or lifecycle. The SDK-free common workload compares stable fields and guards while deliberately excluding all five dynamic capacity counters. The independent virtual workload checks all4400 emitted bytes against authored literals. The first native virtualized round timed out in validation loops; fixed loops are unrolled without changing a single service, assertion, quantum or deadline. Broader default-fatal cleanup and framework execution are separate unfinished work.

## Declared mount enumeration

宣言されたマウント一覧は順序付きキャッシュと明示的な更新結果を提供します。プロセスごとにキャッシュを保持し、重複した記録と空の一覧を正確に扱います。未宣言の一覧は不明のままです。Apple アプリが使うファイルシステム照会を追加しましたが、dyld、Mach IPC、完全な Objective-C/Swift と Foundation/UIKit 環境は未完成です。

```text
DarwinFileSystemEnumerationOptions / DarwinMountObservation / FileSystemEnumeration / filesystem_enumeration
getfsstat64 BSD347 Int / shared LP64 record2168 / independent ordered live accessible mounts
mounts required array / cached complete16 fields required / refreshed complete16 fields optional
empty table known empty / missing table unsupported / duplicate entries retain order / no inferred mount identity
NULL output returns full count without refresh / short buffer capacity zero without memory access
low32 signed byte size promoted to LP64 unsigned before division and narrowed int32 / negative size large capacity
NOWAIT2 uses current cache unless WAIT1 or DWAIT4 / other bits follow same test / low32 flag carriers
process cache starts Cached / refresh selects Refreshed before complete EFAULT14 / new process resets cache
copy complete records sequentially / later fault retains prior output and refresh / partial current record unsupported before its effects
missing refresh stops before current effects / earlier records stay committed / path and FD snapshots independent
one catalogue entry per mount / each snapshot2168 bytes / shared256 entries16MiB / public JSON64KiB unchanged
both static query domains reject empty enumeration / all57 other logical file routes closed before preflight
MountEnumerationKeepsOrderedSnapshotsAndRefreshCache / common16 authored services main52 stdout M
virtual-mount-enumeration / 19 authored services main53 / independent19609 full guarded output bytes
270 ARM64 O0 O1 O2 native observations / count stability and literal guard checks / no universal host counters
native-common73 / mandatory88 per platform / matching ARM64 required264 identities
native5s guest5000000us quantum1024 public30s existing method and owner bounds unchanged
no dead unmount MAC refusal failed refresh asynchronous wait host query or complete Apple framework claim
```

Raw BSD getfsstat64 347 reads only the explicitly declared mount table. JSON retains the required darwin_files.files array (which may be empty) and adds filesystem_enumeration: {"mounts":[{"cached":complete_snapshot,"refreshed":complete_snapshot}]}. Each snapshot uses all sixteen fields documented for filesystem_observations. Refreshed is optional; its presence declares immediate successful refresh for that mount. Empty mounts is a known empty table. Absence remains unsupported even for count-only requests. Entries model live, accessible mounts in exactly the declared order, including duplicates, without MAC denial, dead or unmounting state, refresh failure or asynchronous provider work.

NULL output returns the full count without refresh. NonNULL output copies only complete 2168-byte records, returning the smaller of table size and capacity. The low signed32 buffer length is promoted to unsigned64 before division and the quotient is narrowed to signed32, as in the pinned XNU ABI; negative lengths therefore have a large positive capacity. Zero capacity does not access the pointer. Flags use the low32 bits: NOWAIT2 selects the current cache unless WAIT1 or DWAIT4 is also present. Otherwise the current mount needs an explicit Refreshed snapshot. Unknown bits follow this same bit test.

Each process begins with Cached. A successful declared refresh selects Refreshed before copyout, so even a completely unmapped destination returning EFAULT14 updates that mount’s cache. Later NOWAIT observes the selected record. New process preparation resets all selections. Sequential records preserve earlier output and cache changes when a later copy faults or lacks a refresh provider. An individually partially accessible record is explicitly unsupported before its own refresh or output; this is not a modeled native partial-copy result.

Each mount consumes one shared catalogue entry and each cached/refreshed snapshot consumes2168 bytes within the existing256-entry and16MiB budgets. The public JSON envelope stays64KiB. Both static query environments reject even an empty table. Enumeration supplies no statfs/fstatfs object binding and derives no identity from equal filesystem IDs, devices, inodes or path prefixes.

Original implementation and tests use [pinned XNU getfsstat64 ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [mount-stat cache updates](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c); no implementation was copied. ARM64 O0/O1/O2 probes retain270 observations with stable before/after counts, high carriers, size and flag boundaries, refresh/fault sequences and guarded output. Actual host counters are evidence for those runs, not virtual constants. Native and virtual workloads remain within native5 seconds, guest5,000,000 microseconds, quantum1024 and public30 seconds.

## Explicit ordinary directory modes

明示的な mode provider は通常のディレクトリとファイルを扱います。初期ディレクトリの権限と新規ディレクトリの独立した設定は、名前空間の変更権限を代替せず、子オブジェクトにも継承されません。完全な dyld、Mach IPC、Objective-C/Swift、Foundation/UIKit 環境と実機 iOS の検証は未完成です。

```text
directory mode: chmod15 fchmod124 fchmodat467 / ordinary initial regular or directory objects
CreatedDirectoryObjects=false / created_directory_objects exact Boolean / separate CreatedRegularObjects
created directory mode requires complete CreationPolicy.Namespace / resulting ctime uses Mutation.Time
DirectoryNode ModeTime separate from MutationTime / CurrentMetadata never supplies namespace authority
directory nlink greater than1 valid / equal known Device-Inode aliases rejected / ownership and timestamps require separate providers
mode changes only permissions and ctime / independent namespace time preserves mode / missing namespace time permanently invalidates stat
dup independent opens chdir fchdir direct rename exchange held rmdir and reused names / no child grant inference
directory-mode-mutations59 services / created-directory-mode60 services / 33 full guarded stat144 records / main53 stdout Q
five software profiles C CLI Python / three matching ARM64 HVF required / native common75 mandatory90 ARM64270
60 native directory observations plus3 separate unexpanded-symlink controls / ARM64 O0 O1 O2 / native5s guest5000000us quantum1024 public30s
```

Raw BSD chmod15, fchmod124 and fchmodat467 accept initial ordinary directory objects through the same required mode_mutations.objects array of {path,time} declarations. Complete ordinary directory metadata, flags0, no special permission bits, a valid independent ctime and no known Device/Inode alias are required. Directory nlink may exceed1; regular-only nlink1 is not applied to directories. Equal Device/Inode declarations under different paths remain rejected. A directory grant needs no creator or mutable namespace. Neither a parent grant nor namespace mutability grants mode changes to initial children.

The separate optional created_directory_objects Boolean defaultsfalse. True requires a complete creation_policy including namespace_policy; successful mkdir/mkdirat attaches mode authority to the actual new directory and uses creation_policy.mutation_policy.mutation_time for resulting ctime. created_regular_objects remains independent and never grants directories. Unknown fields, non-Booleans and incomplete policy fail during strict import. Failed creation, existing names, initial objects and name reuse transfer no grant.

The existing effective-credential authorizer and current ancestor SEARCH owner are shared. Ordinary changed-mode nonowner requests return EPERM1, while an equal request can reach the independently required provider. FD lookup precedes authorization; readonly descriptions are eligible. Mode changes preserve all other complete stat fields and all namespace, enumeration, byte/entry/FD/inode accounting, xattrs and VM observations. Directory ownership and explicit timestamp mutation remain outside these mode providers. Special/security/root-caller and symbolic-vnode effects remain unknown.

Namespace changes require their own metadata time and shape. With a directory mutation policy, mkdir/rmdir/direct rename/exchange preserve current mode while applying declared namespace effects. Without it, even a chmod-produced CurrentMetadata becomes permanently unknown and is cleared. A later mode provider cannot recover it or borrow its mode time as a namespace time. Dup, independent opens, chdir/fchdir, direct moves/exchanges and held removed directories follow the actual object; reused names get separate authority.

Original ARM64 macOS O0/O1/O2 probes retain60 directory observations and3 separate final-unexpanded-symlink controls outside the directory provider. The common SDK-free workload performs59 authored services and33 guarded complete stat144 observations, including high carriers, path/fd/*at, CWD, held rename, closed descriptors and unchanged error effects. A native execution returning53/Q validates the authored common behavior without a captured service trace. Virtual initial and created variants independently check every stat byte, including declared mode and namespace times; created execution adds mkdir for60 services. Those virtual values are not native constants. Native nonowner/special security, physical Intel and physical iOS remain unqualified. Native5s, compile120s, per-component build1200s/parallel2, guest5000000us/quantum1024 and public30s remain fixed.

The implementation and original probes use [pinned XNU chmod ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [current-mode authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). No implementation was copied.

## Explicit ordinary directory timestamps

明示的なディレクトリ時刻プロバイダーは、個別に許可された初期ディレクトリと新規ディレクトリの utimes/futimes をサポートします。名前空間、モード、所有権の権限は独立しています。完全な Apple ランタイムとフレームワーク、実機 iOS 検証は未完了です。

```text
directory timestamps: utimes138 futimes139 / ordinary initial regular or directory objects
TimestampMutations CreatedDirectoryObjects=false / created_directory_objects strict Boolean / separate CreatedRegularObjects
created directory timestamp provider requires complete CreationPolicy.Namespace / fixed ctime Mutation.Time
DirectoryNode TimestampTime independent of ModeTime MutationTime DirectoryEntrySize / unknown metadata never restored
explicit timestamps require owner even when equal / NULL uses declared TimeOfDay and shared WRITE_ATTRIBUTES authorization
LP64 timeval32 signed64 seconds signed32 microseconds ignored padding / int64 nanosecond saturation floor normalization / earlier nonnegative mtime reduces birthtime
directory-timestamp-mutations46 services / created-directory-timestamps47 services / 24 complete guarded stat144 records / main54 stdout T
five software profiles C CLI Python / three matching ARM64 HVF required / native common75 mandatory90 ARM64270
141 owned native directory timestamp observations / ARM64 O0 O1 O2 / native5s compiler120s build1200s parallel2 guest5000000us quantum1024 public30s
```

Raw BSD utimes138 and futimes139 now accept independently granted ordinary directories through timestamp_mutations.objects: [{path,time}]. The time is a separately fixed virtual resulting ctime. Initial grants need complete ordinary metadata, flags0, no special mode bits and no known Device/Inode alias; ordinary directory nlink can exceed1. Initial directory grants do not require creation or mutable namespace authority.

The optional strict created_directory_objects Boolean defaultsfalse and requires a complete creation_policy including namespace_policy when true. Only successful mkdir/mkdirat attaches this independent directory timestamp provider, using creation_policy.mutation_policy.mutation_time for ctime. created_regular_objects remains separate. Regular creation flags, mode providers, parents and children supply no timestamp grant to an initial or newly reused directory name.

utimes resolves its path before importing the request. futimes imports the32-byte LP64 timeval array, or the separately declared NULL sample, before low32 FD lookup. Explicit requests require current ownership even when values are equal. NULL uses only darwin_time.time_of_day and shared WRITE_ATTRIBUTES authorization: an ordinary owner is allowed, while a nonowner needs the shared known group/world write decision. Missing time samples, credentials, metadata or providers remain unsupported; bad paths, pointers and descriptors preserve the established raw errno/carry order.

The existing declared storage rule is unchanged: signed64 seconds and signed32 microseconds with ignored padding form signed wide nanoseconds, saturate to int64 and floor-normalize. The fixed provider ctime is independent of requested values and the NULL sample. An earlier nonnegative resulting mtime reduces the known birthtime; negative mtime does not. These virtual rules do not identify APFS or synthesize native filesystem time ranges. Success preserves every unrelated stat field, guards, input bytes, enumeration positions, FD flags and accounting.

Direct moves/exchanges and held rmdir retain actual object grants, while reused names receive none. Namespace changes need their own MutationTime and DirectoryEntrySize. A timestamp-produced CurrentMetadata never supplies these premises; without them, namespace changes permanently invalidate complete metadata and later timestamp calls cannot restore it. Timestamp authority grants no mode, ownership, data or namespace mutation.

Original owned ARM64 macOS O0/O1/O2 probes retain141 guarded directory observations, including low/high syscall/FD carriers, read-only/dup descriptors, errors, link following, direct moves, held removal and name reuse. Native secondary results were zero in these observations. The common SDK-free directory-timestamp-mutations workload checks46 authored calls and24 complete guarded stat144 records, returning54/T; it captures no native service trace. Initial and created virtual variants independently check every stat byte and separately declared times; creation adds mkdir for47 calls. Native nonowner/special-security effects, Intel hardware and physical iOS remain unqualified.

The original implementation and probes use [pinned XNU utimes/futimes ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [timestamp authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). No implementation was copied.

## Explicit ordinary directory ownership

明示的な所有権プロバイダーは、個別に許可された初期および新規の通常ディレクトリの chown/fchown/fchownat をサポートします。グループ変更は子オブジェクト作成に使う実際の親ディレクトリの識別情報も更新します。名前空間、モード、時刻の権限は独立しています。完全な Apple ランタイムとフレームワーク、実機 iOS 検証は未完了です。

```text
directory ownership: chown16 fchown123 fchownat468 / ordinary initial regular or directory objects
OwnershipMutations CreatedDirectoryObjects=false / created_directory_objects strict Boolean / separate CreatedRegularObjects
created directory ownership provider requires complete CreationPolicy.Namespace / fixed ctime Mutation.Time
DirectoryNode OwnershipTime independent of ModeTime TimestampTime MutationTime DirectoryEntrySize / unknown metadata never restored
active low32 UID GID including equal values set fixed ctime / both UINT32_MAX inactive require provider and preserve all observations
actual DirectoryIdentity.GID and CurrentMetadata.GID commit together / newly created regular directory symbolic objects inherit current actual parent group
directory-ownership-mutations61 services / created-directory-ownership62 services / 29 complete guarded stat144 records / main55 stdout O
five software profiles C CLI Python / three matching ARM64 HVF required / native common76 mandatory91 ARM64273
219 owned native directory ownership observations / ARM64 O0 O1 O2 / native5s compiler120s build1200s parallel2 guest5000000us quantum1024 public30s
```

Raw BSD chown16, fchown123 and fchownat468 now support explicitly granted ordinary directories through ownership_mutations.objects: [{path,time}]. Initial directory grants require complete ordinary metadata, flags0, no special mode bits and no known Device/Inode alias, without requiring mutable namespace authority. Ordinary directory nlink may exceed1; regular nlink1 remains required.

The optional strict created_directory_objects Boolean defaultsfalse. True requires complete creation_policy with namespace_policy; only successful mkdir/mkdirat attaches the independent new-directory provider. Its fixed virtual ctime comes from creation_policy.mutation_policy.mutation_time. created_regular_objects remains separate. Child ownership authority comes only from a child's own initial grant or the independently selected creation flag.

Existing effective ordinary authorization and error order are unchanged: FD lookup precedes object checks; pathname routes authorize actual ancestor SEARCH; fchownat admits only low32 NOFOLLOW0x20 before pathname/FD lookup. Low32 UINT32_MAX is inactive independently for each ID. Both inactive IDs require a provider and preserve the whole record. Equal active IDs require a provider and set declared ctime. Changed third-party UID is denied before group lookup; changed GID needs known membership and current-owner WRITE_SECURITY. Unknown credentials/security/group information remains unsupported. Readonly descriptions and duplicates retain actual-object authority.

Directory GID changes also update the actual parent's retained group identity before any child can be published. New regular files, directories and symbolic links inherit this current GID, including after a direct parent move. A later namespace invalidation preserves only this independently known identity; stat and subsequent ownership mutations remain unsupported and cannot restore complete metadata. Ownership supplies no mode, timestamp, data, namespace or filesystem-observation grant.

Original owned nonroot ARM64 macOS O0/O1/O2 probes retain219 observations, including high syscall/FD/ID/flags carriers, active equal and inactive IDs, errors, final-link following, NOFOLLOW directory requests, dup/readonly descriptions, moves, held removal/reuse and positively observed alternate-group changes. Created regular, directory and symbolic objects inherited that actual group. Complete144-byte stat records include8-byte guards on both sides. Native secondary results were zero in these observations. These owner-operated probes do not establish native nonowner, special-security, Intel or physical iOS behavior.

The SDK-free directory-ownership-mutations workload checks61 authored calls and29 guarded whole stat records, returning55/O. Virtual initial and created variants independently check every stat byte and fixed times; creation adds mkdir for62 calls. The common native workload captures no native service trace and compares actual native ctime rather than inventing its value. All four ID patterns use the base carrier per route, while upper carriers repeat active-both requests. The independent native219 observations and private model checks retain full Cartesian typing. Namespace shape uses the separately declared virtual all-child policy, not inferred native filesystem link counts.

Original implementation and probes use [pinned XNU ownership ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [ordinary setattr authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). No implementation was copied.

## Filesystem snapshots for created directories

独立した作成フラグにより、新しいディレクトリは実際の親に宣言されたファイルシステムのスナップショットを保持できます。移動と削除後も開いた記述子は元の関連を保持します。完全な Apple ランタイムと実機 iOS 検証は未完了です。

```text
FileSystemObservations CreatedDirectoryObjects=false / filesystem_observations.created_directory_objects strict Boolean / independent CreatedRegularObjects
created directory filesystem binding requires complete CreationPolicy.Namespace / successful mkdir binds actual parent DirectoryNode.FileSystem before publication
unbound parent snapshot remains unknown / creation succeeds without manufacturing a snapshot / zero snapshot explicitly known
actual snapshots survive moves exchanges dup held rmdir and metadata invalidation / replacement objects bind their own actual parent
filesystem owner and directory stat UID GID independent / no attribute namespace mount identity or global fsid inference
created-directory-filesystem33 authored services / virtual-created-directory-filesystem35 services / main56 stdout D / independent4400 guarded payload bytes
102 owned ARM64 native filesystem observations O0 O1 O2 / native common77 mandatory92 per platform ARM64276
125 initial private and strict JSON checks / five software C CLI Python profiles / three matching ARM64 HVF profiles required
```

Set darwin_files.filesystem_observations.created_directory_objects to the strict Boolean true to opt in. The provider still requires objects (an empty array is valid); entries retain all sixteen snapshot fields and the existing shared2168-byte/path charge. CreationPolicy must include namespace_policy, with the existing explicit mutable parent, complete parent identity and umask prerequisites. False or omission preserves the default closed new-directory domain.

Each successful complete mkdir uses its actual parent’s bound snapshot if present. A zero snapshot is known; absence remains unknown and statfs64/fstatfs64 refuse a missing provider before output preflight. A regular child of a bound created directory needs the independent created_regular_objects flag; another new directory needs the directory flag. Initial children and symbolic vnodes receive no implicit grant. Filesystem owner remains independent of stat UID/GID, including after chown. Mode, timestamp, namespace, quota and mount identity authority are not inferred.

Queries preserve every declared2168-byte payload and zero padding, descriptor state and unrelated full stat bytes. An existing descriptor retains its actual snapshot through parent/child moves, exchange, invalidated stat and held deletion. A replacement binds its own actual parent. Counters stay fixed declared observations; no advancing live mount is claimed. Original probes use the previously pinned XNU statfs64/fstatfs64 declarations and native SDK LP64 layout; no implementation was copied.

## Filesystem snapshots for symbolic vnodes

明示的なファイルシステムのスナップショットは、参照先が存在しない場合や循環する場合もシンボリックリンクの記述子をサポートします。パスの照会は参照先をたどります。新規リンクの親への関連付けは独立したフラグで指定します。完全な Apple 環境と実機 iOS 検証は未完了です。

```text
symbolic filesystem: fstatfs64 BSD346 O_SYMLINK own LinkNode.FileSystem / statfs64 BSD345 follows target snapshot
CreatedSymbolicLinkObjects=false / FileSystemCreatedSymbolicLinkObjects created_symbolic_link_objects strict Boolean / filesystem purpose only
created symbolic snapshot requires complete CreationPolicy.Namespace / actual parent binding before successful publication / independent regular directory flags
initial symbolic snapshots need no target stat metadata or valid target / unbound unknown and explicit zero distinct / shared2168-byte plus path reference budget
link object snapshots survive dup parent moves exchanges rename held unlink and name reuse / invalidated stat never restored
symbolic-filesystem-observations32 created-symbolic-filesystem33 virtual-symbolic-filesystem35 authored services / main57 stdout Y / independent6600 guarded bytes
114 owned ARM64 O0 O1 O2 native filesystem observations / common native79 mandatory93 per platform ARM64279
166 initial private strict JSON checks / five software C CLI Python and three matching ARM64 HVF profiles / public30s guest5000000us quantum1024
```

Declare an initial link snapshot with the existing filesystem_observations.objects {path,snapshot} entry. All sixteen snapshot fields remain required; zero/empty values are explicit. The link may have a dangling, cyclic or opaque byte target and no stat observation. A descriptor opened with O_SYMLINK queries the link's own declaration. A normal path query follows its target and queries the resulting object; it does not borrow the link declaration. Missing/empty environments and absent per-object snapshots remain distinct unsupported completions before output preflight.

For new links, set filesystem_observations.created_symbolic_link_objects to Boolean true and supply complete creation_policy.namespace_policy. Successful symlink creation uses the actual parent's already-bound snapshot if present, independently of regular/directory flags. Initial children and equal filesystem_id/device/inode values grant nothing. An unbound parent permits creation but leaves the new link query unknown. Incomplete/failed/existing-name creation publishes no new binding. Name reuse never recovers an old initial observation.

Descriptor lookup narrows FD before kind/environment/provider/output checks. Valid symbolic snapshots need no target stat, credentials or access flags. Whole2168-byte records, padding and guards retain the existing copyout contract: unmapped output returns EFAULT14; partial output is unsupported before effects. Other pointer/FD/path errors preserve ordering. Raw native references use the cached pinned XNU fstatfs64 file_vnode/mount-stat implementation and native SDK O_SYMLINK/LP64 declarations. No implementation was copied; dynamic native counters and real mount identity are not literal virtual facts. Full framework execution remains incomplete.

## 通常のシンボリックリンクの明示的な所有権

明示的な所有権設定は、シンボリックリンクの記述子と NOFOLLOW パスに対応します。リンク先には独立した権限が必要です。完全な Apple 実行環境と実機 iOS の検証は未完了です。

```text
symbolic ownership: chown16 fchown123 fchownat468 / ordinary initial regular directory symbolic objects
fchown O_SYMLINK and fchownat NOFOLLOW0x20 select actual LinkNode / chown and default fchownat follow target
OwnershipMutations CreatedSymbolicLinkObjects=false / FileOwnershipCreatedSymbolicLinkObjects created_symbolic_link_objects strict Boolean
initial exact Objects grant / created symbolic ownership requires complete CreationPolicy.Namespace and uses Mutation.Time / independent regular directory flags
LinkNode OwnershipTime independent of MutationTime and FileSystem / unknown complete metadata never restored
symbolic-ownership-mutations59 created-symbolic-ownership60 virtual-symbolic-ownership67 virtual-created-symbolic-ownership68 authored services / main58 stdout W
literal variants each1281 output bytes / eight complete stat144 records with two8-byte guards / separate link target and replacement inodes
318 owned native ownership observations O0 O1 O2 / six common native runs / native common81 mandatory94 per platform ARM64282
74 direct ownership plus6 strict JSON checks / five software C CLI Python profiles / three matching ARM64 HVF required
native5s compiler120s build1200s parallel2 guest5000000us quantum1024 public30s / native Intel and physical iOS unqualified
```

An initial symbolic ownership grant names the exact symbolic vnode in `ownership_mutations.objects`. Admission requires complete ordinary metadata, flags0, no special mode bits, link_count1 and no known Device/Inode alias. Grant presence, namespace mutability, target metadata and filesystem snapshots are independent. Mode and timestamp declarations still reject symbolic grants. The optional `created_symbolic_link_objects` Boolean defaults false; true requires complete `creation_policy.namespace_policy`. Successful new symlink creation alone attaches its own fixed ctime provider from `creation_policy.mutation_policy.mutation_time`. A parent's grant or another creation flag cannot supply it.

`fchown` on an `O_SYMLINK` descriptor and `fchownat` with low32 `NOFOLLOW0x20` select the actual link. Ordinary `chown` and default `fchownat` follow its target, whose separate grant and complete metadata are required. Dangling and cyclic targets do not affect retained-link or NOFOLLOW operations; followed routes retain ENOENT2 and ELOOP62. FD lookup and complete metadata checks retain their existing precedence; pathname routes authorize actual ancestor SEARCH before final-object checks, and invalid fchownat flags precede pathname import.

The shared effective ordinary authorizer remains authoritative. Both inactive low32 UINT32_MAX IDs require a provider and preserve every stat byte. Any active request, including equal IDs, commits requested UID/GID and the separately declared ctime together after authorization. Unknown credentials, security or group membership remain unsupported. A link GID change affects neither its target nor its parent's retained identity. Namespace and xattr changes without their own complete metadata policy permanently invalidate stat; ownership never repairs it.

Object grants survive duplicate and independent descriptions, runtime symbolic hardlinks, direct and parent moves, exchange and held unlink. A replacement name binds a new inode and receives only an independently selected creation grant. Darwin link9 follows the source link; symbolic hardlink construction uses linkat471 flags0. The original native probe corrected this distinction before production edits; its failed first epoch remains preserved.

Original owned nonroot ARM64 macOS O0/O1/O2 probes retain318 observations, including raw carry/secondary/high carriers, whole guarded stat records, libc controls, flags/cursors/target bytes, observed alternate groups, dangling/cyclic targets and object lifetime. The compact SDK-free common workload passes on both initial and created links at each optimization level. It captures no native service trace and assumes no fixed native ctime. Its literal virtual variants emit eight independent complete guarded stat records and verify every byte, while all variants verify authored raw service outcomes and fixed scalar arguments. Positive runs repeat from fresh state; omitted initial grants stop at the first ownership call, and a missing new-object grant stops at the replacement while preserving the preceding six literal records.

C, CLI and Python routes use all five image profiles without loading an analysis session, compare exact output and services, and reject malformed Booleans. Three matching ARM64 HVF identities execute under required-native policy. Current inventory requires94 methods per matching platform,282 on ARM64; the common native catalogue contains81 cases. Intel hardware, physical iOS, special-security/nonowner native effects, full dyld, Mach IPC and Apple frameworks remain unqualified or unfinished.

The implementation uses the existing shared ownership authorizer and [pinned XNU ownership and link ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), plus [ordinary setattr authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). No kernel implementation was copied.

## lchown によるパス末尾オブジェクトの所有権変更

`lchown`（BSD 364）は末尾のシンボリックリンクをたどらずに所有権を変更し、リンク先が存在しない場合や循環する場合もリンク自体を選びます。途中のリンクは解決し、実際の祖先ディレクトリには SEARCH 権限が必要です。通常ファイル、ディレクトリ、シンボリックリンクには既存の独立した `ownership_mutations` 許可を使います。有効な下位 32 ビット ID は要求 ID と固定 ctime を確定します。両方が無効値 `UINT32_MAX` の場合もプロバイダーを要求し、stat 全体を保持します。作成フラグ、オブジェクトの寿命、メタデータの永続的な無効化は既存の所有権規則に従います。

共通ワークロードはガード付き全レコード、BSD の生の結果、別名、移動、削除後の保持と名前の再利用を確認します。仮想版は独立して記述したバイトを出力し、参照先の権限を分離します。Intel ネイティブ、実機 iOS、完全な Apple ランタイムとフレームワークは未検証または未完成です。

```text
lchown364 / final symbolic vnode / existing ownership_mutations grants
lchown-ownership-mutations59 services / virtual-lchown-ownership67 services / main58 stdoutW / virtual1281 guarded bytes
native preparation489 observations including258 lchown / ARM64 O0 O1 O2
common native82 / mandatory95 per matching platform / ARM64 required285
```

## 有界 libSystem インポート

ベースラインの thin 実行形式と以下の明示的な起動 dylib は、強い依存 `/usr/lib/libSystem.B.dylib` を結合できます。主入口は `LC_MAIN`、名前空間は `MH_TWOLEVEL`、依存は通常の ordinal を使い、libSystem 互換要求は 1.0 以下です。元の classic/generic64 fixup と未定義シンボルの完全な結合を検証します。未知のシンボル、weak/threaded 結合、他の relocation、初期化コールバック、認証ポインタは拒否します。

インポートがある場合だけ認証済み入口と新しい主スレッド errno を配置します。ラッパーは Darwin カーネル効果を共有し、エラーを `-1` と errno に変換し、成功時は errno を保持します。C `_exit` は Mach-O `__exit`、C `exit` は未対応の `_exit` です。通常呼び出しは戻りフレームを検証し、終了専用呼び出しは戻り先を必要としません。レポートは提供元、正確な名前、引数数、全ビットを保持し、未完了または非復帰の結果は null です。従来の raw レポートは互換です。

`SG_READ_ONLY` は fixup 後の現在権限だけを読み取り専用にし、最大権限を保持します。O0/O1/O2 の実機観測で、最大 RW なら `mprotect` で書き込みを復元できることを確認しました。入口と errno の予約ページはゲストによる解除や再保護を拒否します。準備と実行は期限、イベント、既存メモリ予算を共有します。

同一の独立作成オブジェクトを system/classic/chained でリンクし、実際の libSystem で実行します。五つのプラットフォーム/ISA を process/C/CLI/Python、三つの対応 ARM64 HVF profile で確認します。任意 dylib、dlopen/dlsym、shared cache、コンストラクター、スレッド、Apple フレームワーク、Intel HVF と実機 iOS の完全対応は未完了です。

classic lazy ポインタは実行開始前に解決する。各プログラムは格納先、通常の ordinal、シンボルを独立して指定し、ポインタ型、ゼロ addend、一回の DO_BIND と直後の DONE のみを認める。状態を継承せず、ローカル実行可能バイトを指す一つの rebase と同じスロットの lazy bind のみ統合し、両レコードを予算に計上する。dyld_stub_binder は実際の lazy 呼び出しに伴う強い通常 classic bind、ゼロ addend に限る。その実行は C フレーム・引数の読取り、errno 更新、呼び出し記録より前に失敗する。実行時 resolver や dyld 内部状態は実装しない。六関数のネイティブ対照とゲストはポインタの安定性、出力、終了状態を確認する。

`open`、`lseek`、`pread`、`pwrite`、`dup`、`dup2` は既存のファイル、変更権限、記述子カーソルを共有します。`open` の記録は通常 2 引数で、flags の下位 32 ビットに O_CREAT があれば 3 引数です。可変の昇格 int は ARM64 では入口 SP の有効な 4 バイトだけを読み、パディングを読みません。x64 は第 3 汎用レジスタ引数を使います。記録は元の値を保持し、カーネル用 mode だけを 16 ビットに狭めます。O_CREAT がなければ mode はゼロで、任意引数の格納先は読みません。x64 の戻りアドレス検証は必要です。引数読取失敗は記録、errno、ファイル操作に先行します。[Apple ARM64 ABI](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms) と [XNU open](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/open-base.c) に基づきます。O0/O1/O2 のネイティブ対照は共有オフセット、位置指定 I/O、dup2 の失敗、4 GiB 超の位置、mode とバイナリ内容を検証します。スレッドキャンセルと互換パス変換は対象外です。

`mmap`、`mprotect`、`munmap` は既存のマッピング、保護属性、常駐予約領域、ファイル参照を再利用します。C ラッパーは保護値・フラグ・記述子を 32 ビットに変換し、レポートには元の値を保持します。`mmap` は長さゼロ、または MAP_PRIVATE/MAP_SHARED の欠落をカーネル処理より先に MAP_FAILED と EINVAL で拒否し、許可された呼び出しに MAP_UNIX03 を付加します。生の BSD 呼び出しは変わりません。[XNU UNIX03 ラッパー](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/libsyscall/wrappers/unix03/mmap.c)に基づき、独自 C プログラムで匿名ページのゼロ初期化、保護変更、エラー順序、記述子を閉じた後の存続、私的書き込み、EOF の末尾ページを検証します。共有・固定・実行可能マッピングやアロケーターは本変更の対象外です。

```text
provider: /usr/lib/libSystem.B.dylib (strong LC_LOAD_DYLIB; compatibility <= 1.0)
C -> Mach-O: read -> _read; write -> _write; close -> _close
C -> Mach-O: getpid -> _getpid; _exit -> __exit; __error -> ___error
C -> Mach-O: open -> _open; lseek -> _lseek; pread -> _pread; pwrite -> _pwrite; dup -> _dup; dup2 -> _dup2
C -> Mach-O: mmap -> _mmap; mprotect -> _mprotect; munmap -> _munmap
entry: LC_MAIN main + MH_TWOLEVEL images; ordinary positive ordinals; main preferred base
fixups: classic pointer rebase/non-lazy bind; independent lazy programs prepared eagerly; PTR_64 / PTR_64_OFFSET
unsupported: runtime lazy resolver; weak/threaded streams; authenticated/multi-start chains; other relocations
errno: fresh main-thread 4 bytes; error => -1 + errno; success preserves errno
SG_READ_ONLY: current R after fixup; original declared maximum is retained
gates: RX/max RX; errno: RW/max RW; both resident model reservations
budget: one preparation/execution deadline; one event per trap; bounded metadata/model memory
preparation timeout: typed Timeout, zero guest work; original format/backend errors propagate
report v1: optional darwin.native_calls[] = {pc,module,name,arguments,result}
call carriers: hex; full GP bits or defined ARM64 int vararg bytes; exact arity; null result if incomplete/nonreturning
open: 2 args or 3 with O_CREAT; ARM64 mode int at SP, x64 third GP; kernel mode_t 16 bits
basic: main status 58; stdout_hex 4100ff4221; named calls 14; events 15
classic lazy: eager stable slots; status 83; stdout_hex 41005a53; named calls/events 13
_exit: status 35; stdout_hex 58; named calls 2; events 2
file calls: main status 73; stdout_hex 460044; named calls 40; events 41
memory calls: main status 79; stdout_hex 4d0050; named calls 18; events 19
native common catalogue: 82; required methods/platform: 103; matching ARM64 identities: 309
```

```json
{"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["libsystem"],"darwin_files":{"files":[],"stdin_hex":"6162006364"}}
```

## 明示的な起動 dylib とメインスレッド TLS

`darwin.modules` は大文字小文字を区別する正確なインストール名と元の Mach-O パスを最大 32 件指定します。到達可能な依存だけを読み、`@loader_path` は文字どおり比較します。ホストのライブラリ検索は行いません。MH_DYLIB の ISA、プラットフォーム、識別名、バージョンを検証し、通常の ordinal が実際の提供元を選びます。同名エクスポートと共有依存の識別を保ち、入力全体と常駐データを共通の予算で制限します。

初期値あり/ゼロ初期化の通常 C TLS は最大 16 バイト整列をサポートします。準備時に各モジュールの領域を確保し、初回アクセスで再配置済みのライブテンプレートを連続コピーします。先行する BSS/隙間の変更も維持します。記述子と戻りフレームを検証し、TLS ページの再利用、解放、保護変更を拒否します。独自の O0/O1/O2 classic/chained ネイティブ対照と五つの C/CLI/Python プロファイルで比較します。循環、弱い/再エクスポート、選択的 DSO 終了処理と旧 TLS 初期化セクション、dlopen/dlsym、スレッド、完全なフレームワークは未実装です。Intel ネイティブと実機 iOS は未検証です。

```json
{"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"darwin":{"modules":[{"name":"@loader_path/libneverd-base.dylib","path":"/inputs/libneverd-base.dylib"},{"name":"@loader_path/libneverd-leaf.dylib","path":"/inputs/libneverd-leaf.dylib"}]}}
```

```text
graph: strong acyclic startup; exact install name; same ISA/platform; MH_DYLIB + LC_ID_DYLIB
catalogue: max 32; only reachable inputs; no host lookup or @ path expansion
budget: aggregate full files; aggregate resident mappings/TLS/metadata; 65536 records
exports: regular / thread-local; ordinary dependency ordinal; rebased actual provider
TLS: initialized / zero-only / alignment <=16; exact 24-byte descriptor; live contiguous first copy
resolver: __tlv_get_addr; x64 RDI -> RAX; ARM64 X0 -> X0; one named event; errno preserved
lifetime: one main-thread block/image/process; stable addresses; fresh subsequent processes
addends: checked signed ordinary provider offsets; data/TLS initializers; original provider spans only
unsupported: cycles; weak/reexports/resolvers; gate/TLV/lazy addends; selective DSO finalization/legacy TLS init; dlopen/dlsym; threads
workload: shared dependency; duplicate export spelling; TLS aliasing; template mutation; main 83; stdout 440054
native common catalogue: 82; required methods/platform: 103; matching ARM64 identities: 309
```

## Darwin dylib インポートのオフセット

通常の強い dylib インポートは、グローバルポインターと TLS テンプレートで符号付きバイト加算値を扱います。`DarwinImage` がエクスポート先アドレス、ライブラリの配置差分、加算値を一度だけ合成し、オーバーフローと選択した提供元の元セグメント内であることを検査します。ポインター値からアクセス幅は推測しません。libSystem 入口、インポートした TLV 記述子、遅延バインドの加算値はゼロに限ります。

元の O0/O1/O2 classic/chained ネイティブ対照は、空の出力で終了値 78 を返します。C・CLI・Python は全 5 プロファイルを検証します。Intel のネイティブ実行と iOS 実機は未検証です。

## 起動コンストラクターと TLS 終了コールバック

元の `S_MOD_INIT_FUNC_POINTERS`、読み取り専用 `S_INIT_FUNC_OFFSETS`、単一の `LC_ROUTINES_64` を `LC_MAIN` 前に実行します。依存先は宣言順の後順走査で共有イメージを一度だけ初期化します。入口は元の実行可能セグメント内に限定し、ARM64 では整列した4バイト全体を要求します。未実行テーブルの変更は拒否します。コンストラクターは argc/argv/envp/apple/`ProgramVars` を受け取り、プログラム名は argv0 と独立した実行ファイル名です。C++ `thread_local` の遅延構築は元のゲストコードが実行します。

`__tlv_atexit` は2引数の void ABI です。追加の DSO 引数を無視し、errno と結果レジスターを保持して null 結果を記録します。事前計上した最大1024件に null と重複登録を含み、オブジェクト値は64ビットのままです。main の復帰と C `exit` は逆順で処理し、処理中の新規登録を古い項目より先に実行します。現在の SP と x64 red zone より下にフレームを作り、生存中のスタックオブジェクトを保持します。元の実行予算を共有し、完了後に終了結果を公開します。C `_exit` と生の BSD exit は即時終了します。再入 exit、選択的 DSO 終了処理、旧 TLS 初期化セクション、スレッドは未対応です。五つのソフトウェア構成、三つの ARM64 HVF 構成と独立 O0/O1/O2 classic/chained 対照を使用し、対照ではスタック保護を無効化します。guard データインポート、Intel ネイティブ、実機 iOS は未対応または未検証です。

## C/C++ のプロセス終了処理

C `atexit` と C++ `__cxa_atexit` は整数ゼロを返し errno を保持します。C コールバックは引数なし、C++ は元のオブジェクト値を受け取り、DSO は不透明なイベント値です。通常終了は主スレッド TLS を一度だけ処理してから、C/C++ の共通登録順を逆順に実行します。新しいグローバル登録は古いものより先で、TLS 処理は再開しません。強い登録インポートだけが別の1024件を予約し、既存の raw・コンストラクター・TLS 予算を保持します。

元の二つの dylib はコンパイラー生成のデストラクター、局所 static の反復アクセス、入れ子登録、深いスタック、デストラクター内の `_exit` を検査します。局所 static は `-fno-threadsafe-statics` を使用し、`__cxa_guard_*`、`__cxa_finalize`、dlopen/dlclose、スレッド、完全な frameworks は拒否します。五つのソフトウェア構成、三つの ARM64 HVF 構成、C/CLI/Python が既存の形式と予算を共有します。Intel ネイティブと実機 iOS は未検証です。
