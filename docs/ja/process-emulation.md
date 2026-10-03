**言語**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← ドキュメント索引](README.md)

# ゲストプロセスのエミュレーション

`neverd emulate` は明示したゲスト OS プロファイルでイメージを実行します。CPU transport、イメージ解析、プロセス入口、OS サービスはそれぞれ別の責任範囲です。`NEVERD_ENABLE_CPU_EMULATION=ON` で有効にします。ドライバーエミュレーションにも含まれます。

最初の `linux-elf64-v1` profile は、x64/AArch64 の ELF `ET_EXEC` と自己再配置する static PIE `ET_DYN` を CPL3/EL0 で実行します。実際の ELF segment をロードし、初期 stack を構築し、命令 quantum ごとに再開し、明示的な Linux system call request を処理します。これは独立したプロセスモデルであり、完全な Linux distribution や任意の libc binary の実行保証ではありません。dynamic linking、signal、thread、filesystem、未対応 service は明示的に失敗します。

<!-- i18n-section: cli-sdk -->

## CLI と SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

対応する Linux host では KVM、Windows host では WHP を選びます。その他の host/guest ISA 組み合わせは Unicorn を使用します。選択 backend が利用不可なら error となり、暗黙 fallback はありません。Windows 上で実行しても ELF は Linux process model を使用します。命令範囲と制限は[CPU 実行](cpu-execution.md)を参照してください。

CLI は JSON report を一つ出力します。exit code は guest status が 0 なら 0、その他なら 2、未完了（fault/limit 含む）なら 3、setup/API error なら 1 です。実際の guest status は `exit_status` にあります。追加 C entry [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) は session、空でない input path、明示 profile、任意の options JSON を受け取ります。結果を `neverd_free_string` で解放します。NULL は setup error で、`neverd_last_error` に詳細があります。guest fault や resource stop も report を返します。session の分析用ロード済み image は不要で、変更もしません。

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## オプションと結果

options は最大 64 KiB の JSON object です。未知/null field、不正な型、文字列内 NUL、正でない limit は拒否します。

| Option | 既定値 | 契約 |
|---|---|---|
| `backend` | `auto` | `auto`、`unicorn`、`kvm`、`whp` |
| `arguments` | 入力ファイル名 | argv[0] を含む完全な argv。空なら既定値 |
| `environment` | `[]` | 明示的 guest string。host 環境は継承しない |
| `instruction_limit` | 100000 | 共有される許可済み命令試行数 |
| `event_limit` | 10000 | system-call event 数。OS service 前に課金 |
| `timeout_microseconds` | 5000000 | process setup 後に始まる単調 deadline |
| `memory_limit` | 67108864 | 物理／map 済みメモリ予算 |
| `stack_size` | 1048576 | 予算内の page-aligned stack |
| `output_limit` | 1048576 | stdout/stderr 合計の捕捉 byte 数 |
| `instruction_quantum` | 1024 | runtime に戻るまでの admission 間隔 |

`schema_version` は 1 です。report には profile、architecture、選択 backend と理由、`stop_reason`、nullable `exit_status`、診断、入口/現在 PC、counter、service record、最後の型付き CPU exit が含まれます。address、syscall number、引数 register、raw return bit は `0x` なしの hex string です。`stdout_hex`/`stderr_hex` は NUL と不正 UTF-8 を保持します。syscall 結果 null は model が戻り値を定義しないこと（exit や未対応 request など）を示し、成功値 0 とは異なります。

<!-- i18n-section: linux-semantics -->

## Linux profile の意味

OS policy は既存 ELF loader のデコード済み program header を使用します。ABI tag、segment alignment、map 済み PHDR table、user address 範囲を検証します。mapping plan は allocation 前に範囲、権限、重なり、budget を確認し、完全に準備できた private address space だけを公開します。file page の prefix/tail を保持し、BSS を zero 化し、segment 権限を守り、stack guard gap を予約します。ページが重なる layout や矛盾 header は推測せず拒否します。

Static PIE は少なくとも `0x40000000` の決定的 load bias を使い、大きな `PT_LOAD` alignment に応じて増加します。各 segment、entry PC、`AT_PHDR`/`AT_ENTRY` に同じ bias を使い、元の program header 値は変更せず、interpreter がないため `AT_BASE` は0です。mapping source は元の file bytes であり、analysis pointer fixup は混入しません。guest startup が relocation と初期化を実行します。loader は section header に依存せず、元ファイルの範囲検証済み record から `PT_DYNAMIC` を decode します。存在する場合、table は readable/terminated で最大4096 entries。`PT_INTERP`、外部 dependency/filter/audit tag は拒否し、dynamic linker、symbol resolver、constructor runner は提供しません。

初期 stack には aligned argc/argv/envp/auxv、PHDR/PHENT/PHNUM、entry、page size、identity value が入ります。model PID/TID/UID/GID は 1000 です。再現可能な実行のため `AT_RANDOM` は入力 SHA-256 の先頭16 byte です。暗号学的 entropy ではなくモデルの決定的 policy です。HWCAP/HWCAP2 は 0、vDSO はありません。

実装済み syscall は `write`、`exit`、`exit_group`、`getpid`、`gettid`, `mmap`, `mprotect`, `munmap`, `brk` で、番号は [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) と [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h) で別です。戻る x64 SYSCALL は RCX/R11 clobber、RAX、次 PC を反映します。ARM64 は番号に x8、結果に x0 を使います。未知の呼び出しは `unsupported_service` で停止し、host syscall は実行しません。

Static TLS template `PT_TLS` は loader fact として検証されます。template は1つ、file/memory extent は bounded、alignment は congruent、初期 byte は readable でなければなりません。guest startup が各 TLS block を allocate/init し thread pointer を設定します。Linux model は libc 固有の TCB/DTV を作りません。これにより freestanding program の compiler-generated local-exec TLS を許可します。dynamic TLS と OS thread は別の範囲です。

x64 の `arch_prctl` は `ARCH_SET_FS`、`ARCH_GET_FS`、`ARCH_SET_GS`、`ARCH_GET_GS` をサポートします。Set は未mapの user-range base も受け入れますが、後の dereference では権限を検査します。kernel-range base は guest `EPERM`、無効な Get destination は CPU fault なしで guest `EFAULT`。その他の操作は明示的に失敗します。ARM64 startup は `MSR` で `TPIDR_EL0` を設定します。`MRS`、FS/GS memory access、context restore は quantum/backend entry 間で thread pointer を維持します。thread scheduler は実装しません。

descriptor 1 と 2 は仮想 byte sink です。`write` は読取可能な user page を検証し、後続 page にアクセスできなければ読取済み prefix を返し、1 byte も読めなければ guest `EFAULT` を返します。不正 descriptor は `EBADF`。有効 descriptor への length 0 write は pointer を参照しません。Linux pipe atomicity や file object はモデル化しません。出力上限を超える write は公開前に停止します。

匿名メモリサービスは、イメージやスタックと同じプロセスアドレス空間と物理メモリ予算を使います。`mmap` が受け付けるのは `MAP_PRIVATE | MAP_ANONYMOUS` と、通常の `PROT_NONE`、`PROT_READ`、`PROT_READ | PROT_WRITE`、`PROT_READ | PROT_EXEC`、読み取り可能な RWX です。空いているページ境界のヒントを優先し、それ以外は `0x100000000`、次いで最小ユーザーアドレスから空きを探し、スタックガードを確保します。これは決定的配置であり Linux ASLR ではありません。新しいページは独立所有されゼロ初期化されるため、部分的な解除でも固定されていないページを回収できます。CPU 投影や保持中の backing view は、退役した割り当てを自身の寿命まで保持する場合があります。

長さはページ単位に切り上げます。`munmap` は穴や重複解除を許容し、`mprotect` は穴までのマッピングを変更してから `ENOMEM` を返します。`PROT_NONE` は割り当てと内容を保持しつつゲストアクセスを禁止します。生の `brk` は成功時に要求したバイト境界、失敗時に旧境界を返し、libc のゼロ／負一の規約とは異なります。初期 break はページ境界に揃えたイメージ終端です。拡張は他のマッピングと予算に従い、縮小は残る部分ページの内容を保持します。対象範囲の規則とエラー優先順位は Linux の[マッピング](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c)および[保護](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c)に従います。

ファイル／共有／固定マッピング、下方拡張、巨大ページ、メモリ固定、保護キー、実行専用／書き込み専用方針、その他のフラグは明示的に未対応です。効果の公開や戻り値の生成前に停止します。対応範囲内の通常の範囲・長さ・整列エラーはゲストエラーを返して実行を続けます。ゲストポインタやマッピング要求をホスト OS に転送することはありません。

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## Windows PE64 プロファイル

`windows-pe64-v1` は PEB/TEB、静的・動的 TLS、`DllMain`、名前付き Win32 API、明示的な非循環 DLL グラフを持つ有界 Windows x64/ARM64 コンソールプロセスに対応します。ゲストモジュールは名前／序数によるコード・データのインポート、DIR64 再配置、転送エクスポートと実際のローダーリスト識別子を扱います。`LoadLibraryA` / `LoadLibraryW`、`FreeLibrary`、`GetProcAddress` は設定済みモジュールカタログを使用します。CRT/GUI、ユーザー SEH、スレッド、一般的な Windows アプリ互換性は未完成で、ネイティブ ARM64 KVM/WHP の証拠も未取得です。

Windows 仮想メモリに `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` と現在のプロセスの `FlushInstructionCache` を追加しました。OS 層が予約領域を所有し、コミット済みページ、権限、物理記憶域は `AddressSpace` が一元管理します。動的コードの書き換え、アクセス違反、メモリ予算の再利用をテストします。

プライベート割り当ては `MEM_RESERVE`、`MEM_COMMIT`、`MEM_DECOMMIT`、`MEM_RELEASE`、`MEM_TOP_DOWN` に対応し、予約は 64 KiB 境界、ページは 4 KiB です。予約だけではゲスト RAM を消費しません。再コミットは内容を保持して権限を更新し、デコミットは各ページの記憶域を返します。範囲全体の検証と割り当ての準備により、通常の失敗で部分変更を残しません。クエリは 48 バイトの x64/ARM64 メモリ情報を返し、同一割り当て内で前方に結合します。初期イメージ、環境、ヒープ領域、API 入口、スタック境界も配置に含め、スタックの識別を TEB と一致させます。成功した `VirtualProtect` が旧権限の出力先を読み取り専用にした場合、新しい権限は適用されたまま、出力内容は変わらず、呼び出しは成功を返します。 未コミットページを含む範囲の保護変更は `ERROR_INVALID_ADDRESS` を返し、旧権限の出力に `PAGE_NOACCESS` を書き込みますが、ページ権限は変更しません。

対応する保護は `PAGE_NOACCESS`、`PAGE_READONLY`、`PAGE_READWRITE`、`PAGE_EXECUTE_READ`、`PAGE_EXECUTE_READWRITE` です。ガードページ、実行専用、コピーオンライト、キャッシュ修飾子、大きなページ、reset/write-watch/プレースホルダー、モデル所有の実行時マッピングの変更は明示的に未対応です。デコミットと解放はプライベート仮想割り当てだけが対象です。ユーザー例外の配送や ARM64 実機検証は含みません。

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

単一スレッドの PE32+ EXE は推奨ベースを維持し、任意の入口と静的 TLS を持つ明示的な DLL を受け入れます。`WindowsProcessOptions::Modules` または JSON `windows.modules` の `name` と `path` で最大 64 個のゲスト基本名とホスト入力を指定します。ホスト DLL の探索・実行はありません。ASCII 名は大文字小文字を区別せず、重複とシステム API 提供元の上書きを拒否し、到達可能なファイルだけを読みます。名前／序数の関数・データを実際のエクスポートに結び付けます。欠番、未定義、循環、bound/delay import、未対応の load configuration/CFG は失敗します。可動 DLL の衝突には DIR64 を適用し、固定衝突やリンク用メタデータへの再配置書込みは 対象イメージを公開する前に拒否します。

`readPEProgramExports` が元のエクスポートと読み取り範囲、`WindowsProcessModules` が依存グラフとプロセス共通の提供元／名前 API ゲートを所有します。`VirtualMemory` は全イメージを事前予約し、`AddressSpace` がページと権限を管理します。PEB/LDR は実イメージのみを示し、初期化リストはローダーへの登録順です。登録順と依存関係に基づく attach 呼び出し順を別々に保持します。`GetModuleHandleW` は NULL または ASCII 基本名に対応し、大文字小文字を無視し、拡張子なしでは `.dll` を追加します。パス、非 ASCII、末尾の点は未対応です。未検出はエラー 126、成功時は LastError を保持します。API モデルはインストール済み DLL ではありません。

入力総バイト数と全イメージ範囲はそれぞれ `memory_limit` に制限され、実行環境のマッピングも後者に含みます。準備は 65,536 レコード、64 MiB のメタデータ読み取り、名前長、共通期限で制限します。ホスト I/O の硬い時間保証はありません。独自 EXE→DLL→DLL は再配置、序数、共有データ、API ポインター、`MEM_IMAGE`、リスト、EXE TLS attach/detach を検証します。`NeverDWindowsProcessTests` はネイティブ Windows 対照、`NeverDPEProgramExportsTests` は不正メタデータと予算、`NeverDProcessPublicTests` は C ABI/CLI の一致を検証します。利用不能なバックエンドは明示的にスキップします。

`WindowsProcessLifetime` は同じ CPU と実行予算で依存順に DLL TLS コールバック、`DllMain`、続いて EXE TLS と入口を実行します。各モジュールに独立した TLS インデックスと整列済み領域を割り当て、再配置・リンク済みイメージから共有 64 KiB 領域へコピーします。TLS の予約引数はゼロ、起動／プロセス終了時の `DllMain` は不透明な非 NULL 値です。明示的なプロセス終了は初期化完了 DLL をローダーリストの逆順に切り離し、その後 EXE TLS を呼びます。EXE 初期化前でも同様です。起動時の `DllMain(FALSE)` は detach 通知なしで `0xc0000142` 終了します。障害や予算切れは後処理を捏造しません。ゲスト DLL がある PE 入口の return は未対応のスレッド終了を必要とするため明示停止します。非ゼロの `SizeOfZeroFill` は未対応ですが、実際の TLS テンプレート内のゼロ初期化バイトは対応します。 入口なし DLL は TLS attach を受けますが、プロセス detach 通知は受けません。

`WindowsProcessExports` は静的インポートと `GetProcAddress` で名前／序数の解決を共有し、コード、データ、別名、連鎖転送を扱います。 実際に参照する起動時の転送だけがカタログのモジュールと初期化依存関係を追加し、未使用の転送はファイルを読みません。 名前は大小文字を区別し、名前がなければ NULL／エラー 127、直接照会で序数がなければ穴を含め NULL／エラー 182、照会引数が NULL ならエラー 87、成功時は LastError を保持します。 未知のモジュールハンドルは未対応です。 有界 API 登録から提供元／名前ごとの入口を一度だけ確保します。 各イメージの現在の PE ヘッダーとエクスポートメタデータを検査し、変更や読み取り不能を拒否します。 連鎖は最大 64 項で、準備段階の残りのメタデータ予算と実行期限を共有します。 穴への転送は対象イメージのベースを返し LastError を保持します。 序数ゼロへの転送はエラー 87 です。 ベースはデータアドレスであり、イメージヘッダーの実行権限は与えません。 実行時の転送は設定カタログのモジュールをロードし、初期化完了後に照会結果を返せます。実行中のエクスポート表変更は未対応です。

`WindowsProcessLoader` は `windows.modules` の ASCII DLL ベース名をロードし、明示参照、共有依存関係、起動モジュールの保持を管理します。転送の反復照会で余分な参照は増えません。再ロードでは同じカタログ枠に新しい常駐世代を割り当てます。TLS と `DllMain` は同じ CPU 上で中断 API のスタックフレームより下に実行し、レジスター復元はゲストのメモリー書き込みを保ち、現在の戻り先を使用します。動的 attach/detach の予約ポインターはゼロです。明示的ロード中の attach 失敗はクリーンアップ後にエラー 1114 を返し、成功済みの独立した入れ子ロードは保持します。アンロードはイメージと TLS を解放し、再ロードは元の内容から始まります。モデル外のローダーリストや TLS ポインター変更は明示的に拒否します。ファイル、イメージ、メタデータ予算は失敗や再ロードでも累積します。API 提供元に架空 DLL ハンドルはありません。ファイルシステム検索、非 ASCII パス、`LoadLibraryEx` フラグ、循環インポート、初期化／アンロード中の同一モジュールへの再入状態変更は未対応です。

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` は PEB プロセスパラメーター内の実際のゲスト環境ブロックを共有します。名前は大文字小文字を区別しない ASCII、値は UTF-16 です。変更前に入力、容量、書き込み権限を検証します。スナップショットは後続の変更から独立し、解放時にゲストメモリを回収します。モデルのブロック上限は 64 KiB で、文字列と展開処理には境界と実行期限の検査があります。不明なポインター所有権、不正なブロック、ANSI コードページ、展開バッファーの重複は未対応です。`WindowsEnvironmentTests.cpp` は利用可能なバックエンドで独自の x64/ARM64 フィクスチャを比較し、CI では独立したネイティブ Windows オラクルを必須とします。

動的アンロードのコールバック前にモジュールを初期化リストから外しますが、マッピング、名前検索、ロード／メモリリストへの所属はコールバック中も維持します。入口の復帰を調べるネイティブ比較では、システムワーカースレッドとは独立して初期スレッドを観測します。

`WindowsDynamicTests.cpp` は元の x64/ARM64 DLL と EXE を独立したネイティブ Windows 観測と比較し、参照数、共有依存、入れ子ロード、attach 失敗時の清掃、転送照会、プロセス終了、入口なし DLL、再ロード時の TLS 初期化を確認します。追加回帰はローダー改変と無効なコードポインターを拒否し、累積準備予算と中断 API の未完了結果を確認します。Windows CI は原生オラクルと WHP ケースを必須にします。クロスコンパイルと Unicorn ARM64 はネイティブ ARM64 実行の証拠ではありません。

`GetProcAddress` の転送チェーンでライブラリが欠けるとエラー 127 を返し、明示的な `LoadLibrary` でカタログにないモジュールを指定すると 126 を返します。ネイティブ比較と各利用可能バックエンドは宣言済みの全 41 シナリオを検証します。Windows では DLL の各バリアントについて、全 DLL をアンロードした後の入口復帰を 16 回確認します。 `GetProcAddress` の転送先の初期化に失敗した場合も、クリーンアップ後に 127 を返します。プロセス終了の detach コールバックは終了を呼び出した側のスタック内容を保持します。

`WindowsExportTests.cpp` は元の x64/ARM64 DLL と EXE で、転送コード／データ／序数呼び出し、別名、初期化中の照会、再配置、大小文字を区別する欠落、LastError、循環・非常駐ターゲット、不正ポインター、成功した照会後のメタデータ変更を検証します。同じ EXE をネイティブ Windows の独立オラクルで実行し、ネイティブ CI は WHP ケースを必須とします。C ABI／CLI は完全なレポートを比較します。ARM64 ネイティブ実機の証拠は未取得です。 エクスポート表の有無を変えた EXE で、両依存グラフ、PEB リスト順、detach 順、名前／序数／NULL のエラーコードを確認します。

`WindowsLifetimeTests.cpp` は固定トレースを独立したネイティブ Windows プロセスと KVM/WHP/Unicorn で照合します。通常終了、入口 return、両 DLL の初期化失敗、4 箇所の早期終了、入口なし DLL を含みます。回呼障害、共通予算、再配置 TLS フィールド、TLS 総容量も検証します。ネイティブ入口 return のプローブは初期スレッドのハンドルを保持し、終了コードと正確なスレッド／プロセス通知列を 64 回検証します。残る子スレッドは観測後に終了させ、プロセス終了値を入口の戻り値として扱いません。

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

x64 GS と ARM64 x18 は TEB を指し、スタック範囲、自身、PID/TID、PEB、プロセスパラメーター、LastError、TLS を提供します。 UTF-8 を厳密に UTF-16 へ変換し、argv は Microsoft CRT 規則で引用します。 環境名は ASCII、大小文字を無視した重複は拒否し、値は Unicode 可、整列した環境は二重 NUL で終端します。 ホスト環境やファイルシステムは継承しません。 静的 TLS はテンプレートとゼロ BSS、32 ビット索引を設定し、動的 TLS は別の TEB スロットを使います。 起動・終了は変更後の回呼表を順に読み、期限と予算を共有します。 通常のプロセス終了は終了回呼を実行します。 入口からの return は常駐ゲスト DLL がない場合だけに対応し、プロセス終了処理中の二度目の `ExitProcess` は未対応です。

正確な API は `WindowsProcessServices.def` にあります。`ExitProcess`、`RtlExitUserProcess`、標準出力ハンドルと同期 `WriteFile`、LastError、プロセス・スレッド ID と疑似ハンドル、`GetCommandLineW`、ヒープ確保・解放・サイズ、動的 TLS、`LoadLibraryA` / `LoadLibraryW` / `FreeLibrary` / `GetModuleHandleW` / `GetProcAddress` を扱います。`kernel32.dll`、`kernelbase.dll`、`ntdll.dll` の正確な名前だけを解決します。直接 syscall や偽の回呼ゲートでは API を選べません。ヒープの所有権と回収、バイナリー出力、API エラーと非同期 I/O・ユーザー例外の未対応を区別します。別名ポインターでも完了数の初期ゼロ化と実際の戻り先変更を反映します。

`windows.native_calls` はモジュール・関数名、宣言されたスカラー引数、nullable な結果を記録し、NT syscall 番号を捏造しません。`NeverDWindowsProcessTests` は実 PE、コンパイラー TLS、回呼変更、ヒープ、別名、不正メタデータ、権限、予算を検証し、`NeverDProcessPublicTests` は CLI/C ABI を検証します。Windows CI は同じ EXE を直接実行して独立比較し、WHP テストも必須です。ネイティブ ARM64 の実行証拠には対応マシンが必要です。

空でない入力バッファが読み取り不可の場合、`WriteFile` は `ERROR_INVALID_USER_BUFFER`（1784）を返し、書き込みバイト数をゼロにして、バイトを出力しません。

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c). [GetProcAddress](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress).

<!-- i18n-section: verification -->

## 検証

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# shared-library/CLI build の場合:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

テストは両 ISA の独立した ELF エントリアセンブリと C をコンパイルし、data/BSS、起動情報、システムコールエラー、バイナリ出力、権限障害、部分書き込み、未対応サービス、実行量をまたぐ予算を確認します。TLS は独立整列ブロック、ゼロ化 BSS、スレッドポインタと切り替え後の保持を検証し、x64 は `arch_prctl` エラー後も旧ベースが残ることを確認します。利用できないバックエンドはスキップを明示します。公開テストは共有 C ABI と CLI の報告／終了コードを照合します。静的 PIE は自前のデータ／関数ポインタ再配置前に auxv とゼロの RELA スロットを確認します。マッピングテストは分析用バイトを選んだ場合の fixup 保持、動的表はセクションなし・不正・依存入力を検証します。匿名メモリは割り当て、保護、穴、再マッピング、ヒープ伸縮、処理可能なエラーを両 ISA で検証し、実際のゲスト書き込みで通常／部分保護後の障害を確認します。x64 は RW/RX 切り替えで同一アドレスのコードを更新し両版を呼び出します。同じ ELF の Linux ネイティブ実行を独立した結果／障害の参照とします。メモリ単体テストは予算枯渇、回収、RAM を保持しない正本マッピングのスナップショットを検証します。クロスコンパイルや Unicorn ARM64 はネイティブ ARM64 KVM/WHP の証拠ではありません。
