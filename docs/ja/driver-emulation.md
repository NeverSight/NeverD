**Languages**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← ドキュメント索引](README.md)

# Windows ドライバーエミュレーション

NeverD のオプションのドライバーエミュレーターは、対応する x64 WDM ドライバーの PE エントリーポイントを実行します。必要に応じて、明示した逐次リクエストシナリオを実行してからアンロードできます。CPU の実行には Unicorn を、Windows 環境には NeverD 独自の範囲を限定したモデルを使用します。ドライバーをホストカーネルにロードしたり、ゲスト API 呼び出しをホスト OS のサービスに転送したりすることはありません。

## ビルドと実行

この機能は明示的に有効化する必要があり、`BUILD_TESTING` には依存しません。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON
cmake --build build-release --target neverd --parallel 4
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --instruction-limit 100000 > driver-report.json
```

レポートは常に JSON として stdout に出力され、リクエストや環境構築に関する診断は stderr に出力されます。デフォルトの上限は、ゲスト命令 100000 個、ゲストメモリ 64 MiB、記録イベント 10000 件、実行時間 5000 ミリ秒です。命令数の上限は正でなければなりません。いずれかの予算を使い切ると実行を停止し、それまでの観測結果を残します。

| 終了コード | 意味 |
|------------|------|
| `0` | 初期化と、要求されたすべての完了済み操作が成功した |
| `1` | 入力やオプションが無効、環境構築の失敗、またはビルド時に機能が無効だった |
| `2` | 初期化または完了済みリクエストが失敗を表す `NTSTATUS` を返した |
| `3` | 未対応 API、フォールト、予算上限などにより、シナリオの完了前に実行が停止した |

失敗ステータスを返した場合も、その操作の結果を観測し終えたことを意味します。成功という戻り値が示すのは今回のモデル上の実行結果だけであり、Windows 上でドライバーが動作することの証明にはなりません。

## ドライバーの互換性

互換性は `.sys` 拡張子ではなく、実行されたコード経路とその依存関係で決まります。現在の受け入れ検証の根拠は、独自に作成したフリースタンディングの fixture と、Microsoft の SIOCTL WDM サンプルのバッファード、in-direct、out-direct の各経路であり、デバッグログを有効にしたビルドも含みます。任意のサードパーティードライバーとの互換性を示すものではありません。

変更していない Pavel Yosifovich の Zero WDM サンプルでも、direct READ/WRITE、アトミック統計、統計 IOCTL を検証しています。

| ドライバーの種類または要件 | 現在の対象範囲 | 不足する環境 |
|----------------------------|----------------|--------------|
| 下記 API を使用する x64 ソフトウェア WDM ドライバー | 有界 x64 WDM 初期化、逐次 buffered/direct 要求、ワーク項目、タイマー、DPC、イベントと待機、動作レポートと制限 | 追加で実行される API ごとに明確なモデルが必要 |
| `METHOD_BUFFERED` IOCTL | 逐次 buffered/direct I/O、ワーク項目または DPC による完了 | 以下の API 部分集合のみ。並行 IRP と要求キャンセルは未対応 |
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT` | リクエストが所有する MDL とシステムマッピング | 物理ページの識別、DMA、ユーザーマッピング |
| ドライバーが割り当てる MDL | モデルの非ページプールを記述する独立した MDL。元のバッファーアドレスを共有 | IRP との関連付け、MDL チェーン、プローブ／ロック、物理ページ、ユーザーマッピング |
| READ/WRITE | 逐次 buffered/direct I/O、ワーク項目または DPC による完了 | 以下の API 部分集合のみ。並行 IRP と要求キャンセルは未対応。`METHOD_NEITHER` と暗黙のファイル位置も未対応 |
| `METHOD_NEITHER` | 拒否 | ユーザーアドレス空間のコンテキスト、アクセスのプローブ、ゲストの例外処理 |
| KMDF 1.33 非 PnP ドライバー | バージョンバインド、ドライバーと汎用オブジェクト、型付きコンテキスト、参照、実際に実行するクリーンアップ／アンロードコールバック | KMDF デバイス、キュー、要求、クラス拡張、UMDF は未対応 |
| PnP バス／ファンクション／フィルタードライバー | API サブセット内で初期化できる場合がある。デバイススタックのライフサイクルは未対応 | デバイスのアタッチ、下位ドライバーへのディスパッチ、PnP および電源 IRP |
| ストレージ、ネットワーク、ディスプレイ、ファイルシステム、ミニフィルタードライバー | 各サブシステムの契約に未対応 | ポート／クラス／ミニポートのフレームワーク、NDIS/WFP、グラフィックスまたはファイルシステムのサービス |
| ワーク項目、タイマー、DPC、イベントと待機 | 現在の実行 IRQL はディスパッチとワーク項目で `PASSIVE_LEVEL`、DPC で `DISPATCH_LEVEL` です | 以下の API 部分集合のみ。並行 IRP と要求キャンセルは未対応 |
| プロセス／スレッドのコールバック、ハンドル、レジストリ／ファイル操作、カーネルモジュールの検出を使うドライバー | 設定済みレジストリに対応。その他の動作は下記 API の範囲内のみ | オブジェクトマネージャー、システム状態、コールバック／イベントの発生元 |
| ハードウェア、DMA、PCI、割り込み、仮想化を扱うドライバー | 必要な環境に未対応 | デバイスモデル、物理メモリ、バス、割り込み、特権 CPU 状態 |
| x86 または ARM64 Windows ドライバー | 拒否 | アーキテクチャ固有のロード、ABI、実行モデル |
| x64 CFG | 検証済みターゲット表と check/dispatch 呼び出し。非アクティブな計装はゲストのフォールバックを維持 | XFG、エクスポート抑制、未対応ロード構成、TLS は引き続き拒否 |

使用されない未対応インポートはバインドしたままにできます。未対応の操作に到達すると停止し、診断とそれまでに収集した観測結果を残します。DriverEntry の成功だけでは、その後のディスパッチ、ハードウェア、フレームワークの経路に対応しているとはいえません。対応するサブセットの正式な定義は、以下の API 表です。

## 実行契約

このプロファイルは CPU0 上の決定的な協調スケジューリングで x64 WDM ライフサイクルをモデル化します。 実行は PE エントリーポイントから始まり、コンパイラーが生成したエントリーラッパーがあれば、それも実行します。初期化を完了するには DriverEntry が `STATUS_SUCCESS` を返す必要があります。ゼロ以外の成功ステータスや保留ステータスは未対応の初期化契約として停止します。失敗ステータスは、完了済みの初期化結果として保持します。オブジェクト、文字列、スタック、関数ポインター、割り当てはすべてゲストメモリ内にあります。モデルは、設定されたサービス名（デフォルトは `NeverDDriver`）に対応する `DRIVER_OBJECT` とレジストリパスを提供します。

アダプターは Unicorn の仮想 TLB モードを使い、Windows のページテーブルを合成せずにゲスト仮想アドレスを保持します。これには正規形の高位カーネルアドレスも含まれます。RFLAGS の初期値は `0x202` です。ソフトウェアデバイスのプロファイルは固定の 64 バイトキャッシュラインを使用します。これらは、この実行シナリオで明示的に定めた属性です。インラインの x64 CR8 読み取りも同じ `PASSIVE_LEVEL` / `DISPATCH_LEVEL` を観測します。CR8 への書き込みとその他の制御レジスタ操作は引き続き未対応です。

未知のインポートは遅延トラップにバインドされます。未使用なら実行を妨げませんが、その thunk を実行するか、モデル化していないエクスポートデータ値を読み取ると `unsupported_api` で停止します。未対応の CPU 環境への作用も明示的に停止します。NeverD は未実装の呼び出しを成功値で置き換えません。不正なイメージや未対応のロード要件は、実行前に失敗します。

`DelayedWorkQueue` のワーク項目は `PASSIVE_LEVEL`、ゲスト DPC は規定の四引数で `DISPATCH_LEVEL` にて実行します。CPU0 上で呼び出しの復帰とブロッキング待機の境界に決定的な協調スケジューリングを行います。相対・絶対・周期タイマーは仮想時間を使い、実行可能なフレームがなければ次のタイマーまたは待機期限へ進めます。通知型と同期型のイベント／タイマーは異なるシグナル消費を保持します。各コールバックは独立したゲストスタックを持ち、複数の待機フレームのローカル変数と完全な CPU コンテキストを保持しつつ、ゲストメモリを共有します。Win64 コールバックの先頭四引数はレジスタ、それ以降はスタックに渡します。要求は逐次処理し、IRP を保留としてマークしたディスパッチは `STATUS_PENDING` を返し、次の要求の前に完了する必要があります。保留要求や無限待機に実行可能な生成元がなければ、停滞した `model_error` で停止します。命令・メモリ・観測・実時間の予算は共有します。

これは限定的なスケジューリングモデルであり、完全な Windows 非同期対応ではありません。アラート可能／ユーザーモード待機、システムスレッド、APC、要求キャンセル、スピンロック、並行 IRP、一般の IRQL 変更、`METHOD_NEITHER`、UMDF と KMDF のデバイス／キュー／要求契約、完全な PnP／電源、ハードウェア、DMA、割り込みは未対応です。初期化のみの呼び出しも明示的に登録したコールバックを実行しますが、要求やアンロードを暗黙には生成しません。

ワーク項目はコールバック開始前にキューから外れるため、コールバックは自身の項目を解放できます。キュー内の項目の解放、二重登録、失効したオブジェクト、実行可能なゲストメモリ外のコールバック先は明示的に失敗します。デバイス参照はコールバックが戻るまで保持します。アンロードには全ワーク項目の解放とキュー内の処理の完了が必要です。CPU コンテキストは汎用、SIMD、FPU、制御状態を保存・復元します。ゲストメモリは共有され、障害後の CPU を保存コンテキストで再開することはできません。
ファイルオブジェクトやキュー内／実行中ワーク項目の参照が残る間は削除を延期します。オブジェクト領域が不足するとワーク項目の割り当ては NULL を返します。

シナリオで有効な再配置先アドレスを選ばない限り、イメージは優先ベースアドレスを使用します。イメージは native サブシステムを持つ PE32+ x64 実行ファイルでなければなりません。インポート元には `ntoskrnl.exe`、`ntkrnlmp.exe` または `WDFLDR.SYS` を使用できます。

実行ローダーは、検証済みの x64 `DIR64` ベース再配置と、限定されたセキュリティ Cookie のロード構成に対応します。エントリーラッパーの実行前に、決定的なゲスト Cookie を初期化します。その他の未モデル化ロード構成フィールド、TLS、遅延／バインド済みインポート、序数によるインポート、マネージドイメージは拒否します。イメージは厳格な範囲とアラインメントの検査にも合格する必要があります。

有効な Control Flow Guard（CFG）は PE フラグ、ポインタスロット、ソート済み実行可能ターゲット表を検証します。check/dispatch ヘルパーは宣言済みイメージ入口または登録済み API サンクだけを許可し、Win64 呼び出し状態を保持して未宣言ターゲットを拒否します。CFG を有効にしない計装では元のゲストのフォールバックポインタを維持します。有効な XFG、エクスポート抑制、その他の未モデル化ポリシーは拒否し、実行可能メモリ内という理由だけでターゲットを許可しません。

KMDF 対応は正確な 1.33.0 ABI に限定されます。458 個の関数スロットに安定したゲスト識別子を割り当てますが、実行意味論を実装するのは以下の 11 API のみです。`WdfVersionBind` と `WdfVersionUnbind` はゲストのバインドを検証・管理します。実際の WDK `FxDriverEntry` ラッパーをイメージのエントリーポイントとして維持し、`WdfGetDriver` は公開グローバル構造からモデル内のドライバーハンドルを読みます。非 PnP ドライバーと汎用オブジェクトは、ゼロ初期化する型付きコンテキスト、参照カウント、親の所有権、共通予算内で実行する入れ子の cleanup/destroy/unload コールバックに対応します。現在は `PASSIVE_LEVEL` が必要です。デバイス、キュー、要求、クラス拡張、UMDF、完全な PnP／電源は対象外です。未実装スロット、`WdfLdrQueryInterface`、クラスバインドは明示的に停止し、表への登録は実装済みを意味しません。 オブジェクトのコールバックは `PASSIVE_LEVEL` でのみ実行し、クリーンアップ完了後の新しい参照取得はこの実行プロファイルの対象外です。

モデル化済み KMDF API: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`.

任意の実 WDK 検証には `driver_kmdf_lifecycle.c` を使い、本物の KMDF エントリーライブラリで別途コンパイル・リンクします。通常版と CFG 有効版を `NEVERD_KMDF_FIXTURE` と `NEVERD_KMDF_CFG_FIXTURE` に指定し、外部成果物がなければ明示的にスキップします。ネイティブおよび C API／CLI の範囲は[テスト](testing.md)を参照してください。現在の実行証拠は Linux ホストに限定されます。

初期 API モデルの契約は、意図的に有限の範囲に限定しています。

| API | モデル化した動作と制限 |
|-----|------------------------|
| `RtlInitUnicodeString` | 範囲を限定した NUL 終端ソースからゲスト `UNICODE_STRING` を作る |
| `RtlCopyUnicodeString`、`RtlCompareUnicodeString`、`RtlEqualUnicodeString` | 長さを持つ UTF-16 文字列のコピーと、大文字小文字を区別する比較。区別しない比較には Windows の大小文字テーブルが必要なため停止する |
| `ExAllocatePool2` | ページプール／非ページ NX プールの割り当て。デフォルトでゼロ初期化し、未初期化とキャッシュ整列のフラグをモデル化する。無効な必須フラグは NULL を返し、クォータ／実行可能プールおよび割り当て例外の送出では停止する |
| `MmGetSystemRoutineAddress` | 長さを持つゲストの名前を、共通のエクスポート一覧で解決する |
| `MmMapLockedPagesSpecifyCache`、`MmGetSystemAddressForMdlSafe`、`MmUnmapLockedPages` | リクエスト所有 MDL のキャッシュ付き KernelMode マッピングと権限。非ページプール MDL は安全ヘルパーで元のプールマッピングを再利用 |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | 独立した MDL。全範囲が有効な単一の非ページプール割り当て内にあること。MDL とバッファーの寿命は独立。IRP 関連付け、チェーン、クォータは未対応 |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | 明示的なセッションレジストリ、ハンドルごとの権限と寿命、クエリバッファーサイズと変更。ホストレジストリは使用しない |
| `ExAllocatePoolWithTag`、`ExFreePoolWithTag`、`ExFreePool` | プール種別 `0`、`1`、`512` のデータ割り当て。サイズ／タグは正で、タグ付き解放は割り当てと一致する必要があり、アドレスは再利用しない |
| `IoCreateDevice`、`IoDeleteDevice` | デバイス種別 `0x22`、characteristics は `0` または `0x100`、拡張領域のサイズは有限、名前は ASCII の `\Device\Name` |
| `IoCreateSymbolicLink`、`IoDeleteSymbolicLink` | 1 セッションの名前空間内の ASCII `\DosDevices\Name` または `\??\Name`。リンク先は `\Device\Name` |
| `DbgPrint`、`DbgPrintEx` | 検査付き Win64 可変引数の書式処理。出力は最大 512 バイトで、デバッガーフィルターはすべて有効 |
| `IoGetCurrentIrpStackLocation` | 現在モデル化している IRP のスタック位置を返す。通常コンパイルされた WDM マクロも同じゲストフィールドを読む |
| `KeGetCurrentIrql` | 現在の実行 IRQL はディスパッチとワーク項目で `PASSIVE_LEVEL`、DPC で `DISPATCH_LEVEL` です |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | デバイスが所有する不透明なワーク項目。`DelayedWorkQueue` のみ。`PASSIVE_LEVEL` でデバイスとコンテキストをコールバックに渡す。キュー内の項目は解放不可 |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | 不透明な DPC、四つのゲスト引数、`DISPATCH_LEVEL`、重複登録／削除と重要度。対象は CPU0 のみ |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | 通知／同期タイマー、相対／絶対の 100 ns 期限、ミリ秒周期、再設定／キャンセルと仮想時間のシグナル照会 |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | 通知／同期イベントの異なるシグナル消費。`KeSetEvent` は Increment=0、Wait=FALSE のみ |
| `KeWaitForSingleObject` | 初期化済みイベントまたはタイマー一個。非アラート `KernelMode`、理由 `Executive`。ゼロのポーリング、有限の相対／絶対または無限待機。非ゼロ／無限待機は IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | IRQL <= APC_LEVEL で非アラート `KernelMode` の相対／絶対遅延。仮想時間が進むと保存したゲストフレームを再開 |
| `IoMarkIrpPending` | 現在の生存する IRP を保留にする。WDM マクロによるスタック制御フィールドへの等価な書き込みにも対応。ディスパッチは `STATUS_PENDING` を返す必要がある |
| `IofCompleteRequest`、`IoCompleteRequest` | 現在の同期または保留中のモデル IRP を `IO_NO_INCREMENT` で完了させる。完了した IRP やバッファーには再アクセスできない |
| `memcpy`、`memmove`、`memset`、`memcmp`、`RtlCopyMemory`、`RtlMoveMemory`、`RtlFillMemory`、`RtlZeroMemory`、`RtlCompareMemory` | ゲストバッファー操作は 1 呼び出しあたり最大 1 MiB。重複不可のコピー API は重複範囲を拒否する |

API の IRQL 上限は `KernelAPIIRQL.def` にあり、引数依存の制約は担当モデルが検査します。DPC からレジストリ API やページプールの割り当て・解放・アクセスはできません。Unicode `DbgPrint` 変換は `PASSIVE_LEVEL` を要求し、対応する ANSI 出力と非ページ操作は `DISPATCH_LEVEL` で使用できます。コールバックスタックには範囲があり、逸脱したスタックポインターは別の待機ワーカーのスタックへ侵入できません。デバイス拡張内の有効なタイマーは早期解放を防ぎます。一般の IRQL 変更を公開する機能ではありません。

タイマー満了は DPC がタイマーをリセット・再設定する前に登録済み待機を満たします。キュー済み DPC は起床した `PASSIVE_LEVEL` フレームの再開より先に実行します。要求領域にキュー済み DPC が残る場合、IRP 完了処理は完了とバッファ無効化の前に解放を拒否します。

`DbgPrint` の書式処理は、整数 `d/i/u/o/x/X`、ポインター `p`、テキスト `s/c`、`%%`、長さを持つ Unicode `wZ/lZ`、ワイド文字 `ls/ws`、フラグ、`*` を含む幅／精度、Windows 整数の長さ修飾子に対応します。読み取る可変引数は最大 32 個、書式文字列は最大 1024 バイトです。幅と精度の上限は 512 です。浮動小数点、`%n`、未知の組み合わせ、非 ASCII テキストの変換は明示的に停止します。モデルは Windows のコードページを推測せず、ゲストデータをホストの printf に渡しません。

元の RegistryPath レコードとバッファーの寿命は、DriverEntry が戻った時点で終了します。後でその文字列を使うドライバーは、初期化中にコピーしなければなりません。

オブジェクト／プール領域は 1 MiB です。未初期化プールのバイトには決定的な `0xCD` を、解放済みプールのバイトには `0xDD` を使います。これは具体的な 1 つの実行シナリオです。CPU アクセスとモデル化したバッファー API は、解放済みプール割り当て、削除済みデバイス、領域内の未割り当てバイト、不透明なオブジェクトフィールドへのアクセス、および読み取り専用フィールドへの書き込みを拒否します。これらの検査が対象とするのは本モデルのオブジェクトの寿命であり、汎用的なドライバーのメモリ安全性解析ではありません。未書き込みのディスパッチテーブルスロットは、レポートでは「未登録」を表すゼロになります。未登録のメジャー関数に対するシナリオリクエストは、モデルのデフォルトハンドラーにより `STATUS_INVALID_DEVICE_REQUEST` で完了します。この失敗はディスパッチと I/O の両ステータスに残ります。モデルはハンドラー用の架空のゲスト関数アドレスを作らず、ゲストによる未書き込みスロットの読み取りは引き続き未対応です。null コールバックを明示的に登録することはエラーです。

## リクエストシナリオ

`--scenario` に JSON ファイルを渡して、リクエストと任意のアンロードを指定します。

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --scenario scenario.json > driver-report.json
```

`\Device\NeverDIO` を作成し、バッファード IOCTL `0x222000` を受け付けるドライバーの場合、`scenario.json` は次のように記述できます。

```json
{
  "requests": [
    {"kind": "create", "device": "\\Device\\NeverDIO"},
    {"kind": "ioctl", "code": "0x222000", "input": "00112233", "output_size": 4},
    {"kind": "cleanup"},
    {"kind": "close"}
  ],
  "unload": true
}
```

デバイス名と IOCTL コードはドライバーと一致する必要があります。create で `device` を省略すると、唯一の生存デバイスを選択します。一意に選べない場合は失敗します。後続のリクエストは、一致する名前を明示しない限り、そのファイルのデバイスを使います。任意の `file` は符号なし 32 ビットのシナリオ識別子で、デフォルトはゼロです。各識別子は独自の FILE_OBJECT と FsContext を持ち、create、転送、cleanup、close の順序を要求します。独立したファイルのリクエストは交互に実行できます。排他デバイスは二度目のオープンを拒否します。これらの識別子はファイルオブジェクトを表し、複製されたハンドルではありません。バッファードと両ダイレクト方式の IOCTL に対応します。ディスパッチは同期的に完了するか、上記のコールバックによる保留契約を守る必要があります。無効な出力長や完了済み IRP へのアクセスは明示的に失敗します。要求されたアンロードの後には、生存するデバイス、シンボリックリンク、プール割り当て、ファイルオブジェクトを残せません。

省略可能なルートフィールド `"load_address": "0x190000000"` はベースアドレスの変更を要求します。省略または `"0x0"` なら優先アドレスを使用します。イメージは再配置の要件を満たす必要があります。元の初期化コマンドや C API が暗黙にシナリオを実行することはありません。

ルートで受け付けるフィールドは `load_address`、`requests`、`unload`、`kernel_exports`、`registry` だけです。すべてのリクエストは `kind`、任意の `device`、任意の `file` を受け付けます。IOCTL は `code` が必須で、`input`、`output_size`、`direct_input` を受け付けます。`read` は `output_size` と `byte_offset`、`write` は `input` と `byte_offset` を受け付けます。オフセットのデフォルトはゼロで、整数または 16 進文字列を使えますが、非負の符号付き 64 ビット値に収まる必要があります。ライフサイクルのリクエストは転送フィールドを拒否します。未知のフィールドや重複フィールドは拒否します。`code` は符号なし 32 ビット JSON 整数または `0x` で始まる 16 進文字列を受け付けます。`input` は、プレフィックスや空白を含まない、長さが偶数の 16 進バイト文字列です。省略すると空の入力になります。`output_size` は符号なし JSON 整数で、省略時はゼロです。小数や浮動小数点形式の表記は拒否します。

ダイレクト IOCTL では、`input` が最初のシステムバッファーを初期化し、`direct_input` が MDL で記述される別の第 2 バッファーを初期化して、`output_size` までゼロで埋めます。`METHOD_IN_DIRECT` は読み取りアクセスを要求しますが、システムマッピングが読み取り専用であることを意味しません。両方式とも読み書き可能なシナリオバッファーを使います。`MdlMappingNoWrite` はマッピングの書き込み権限を、`MdlMappingNoExecute` は実行権限を除去します。アンマップはシステム VA を無効にしますが、再マップしても同じロック済みデータを保持します。完了時に MDL とマッピングの寿命が終了します。WDM マクロが使う公開 MDL フィールドはモデル化しますが、プロセス／PFN フィールド、手作りの MDL、ユーザーマッピング、生の UserBuffer を介した直接アクセスは拒否します。長さがゼロのダイレクトバッファーは null MDL を持ちます。

`IoAllocateMdl` は、空でなく、アドレスがオーバーフローせず、1 MiB 以下のバッファーに対して独立したメタデータを割り当てます。バッファーのプローブやロックは行いません。`Irp` は NULL、`SecondaryBuffer` と `ChargeQuota` は FALSE が必要です。アリーナが枯渇すると NULL を返します。`MmBuildMdlForNonPagedPool` では、記述範囲全体が有効な単一の非ページプール割り当て内にある必要があります。安全ヘルパーと通常の WDM マクロは元のアドレスを再利用し、別名参照と既存の権限を維持します。書き込み／実行禁止フラグを追加しても既存の権限は変わりません。追加のシステムマッピングとアンマップは拒否します。`IoFreeMdl` は MDL だけを無効にし、プールバッファーの寿命は独立しています。解放済み領域を再使用しなければ、どちらの解放順序も利用できます。モデルの MDL フィールドはすべて読み取り専用で、プロセス／PFN へのアクセス、MDL チェーン、手動のフィールド変更は未対応です。アンロード時にはドライバー所有の MDL をすべて解放する必要があります。

IOCTL の `output_size` が非ゼロなら、入力バッファーが大きくても `Information` はそのサイズを超えてはいけません。出力バッファーがない IOCTL はドライバー定義の結果を返すことができ、出力バイトはコピーしません。`information_hex` は元の 64 ビット値を正確に保持します。

READ/WRITE では、`DO_BUFFERED_IO` または `DO_DIRECT_IO` が転送方式を選びます。Neither または競合するフラグでは停止します。Information は転送長に対して検査します。書き込みはバイト数を、読み取りはバイト列を返します。

`kernel_exports` はルーチン名を明示的な可用性の真偽値に対応付けます。例は `"kernel_exports": {"OptionalRoutine": false}` です。モデル化したエクスポートと静的インポートは、`MmGetSystemRoutineAddress` と共有する安定したアドレスを取得します。明示的に存在しないエクスポートは NULL に解決され、静的インポートを満たせません。存在すると宣言されても API モデルがなければ遅延トラップに解決されます。未知の動的な名前は、可用性が未指定であることを診断して停止します。実装がないことから不在を推測しません。名前は長さを制限した表示可能な ASCII で、解決時は大文字小文字を区別します。この一覧は具体的なシナリオの属性であり、すべての Windows リリースとの一致を意味しません。
`IoMarkIrpPending`, `IoGetCurrentIrpStackLocation` と `MmGetSystemAddressForMdlSafe` はモデル化した WDM ヘッダーの補助関数であり、モデル化されているだけではデフォルトのエクスポートとは宣言されないため、その可用性には静的インポートまたは明示的な `kernel_exports` 宣言が必要です。

シナリオテキストは最大 2 MiB、リクエストは最大 64 件、各入力／出力バッファーは最大 65536 バイト、`direct_input` の内容を含む要求バイト数の合計は最大 512 KiB です。命令、観測、ゲストメモリ、時間の予算は、シナリオ全体に適用します。1 MiB の領域にはオブジェクトやメタデータも配置するため、シナリオのバッファー上限に達する前にモデルのメモリを使い切る場合があります。

## レジストリシナリオ

任意の `registry` 配列は、具体的なセッション内レジストリツリーを定義します。各キーには必須の `path` と任意の `values` 配列があり、各値には `name`、符号なし整数の `type`、16 進数の `data` を指定します。空の値名は既定値です。DWORD の例は `{"name":"Mode","type":4,"data":"01000000"}` です。値のバイトはそのまま保持し、文字列終端の修復や環境変数の展開は行いません。

パスは `\Registry\Machine` または `\Registry\User` 配下の絶対 ASCII パスが必要です。祖先キーは自動作成します。キーと値は ASCII 規則で大文字小文字を区別せず比較し、非 ASCII 名や重複は拒否します。省略すると可用性は未指定となり、レジストリ呼び出しは停止します。`"registry": []` は空の名前空間を明示します。ドライバーからキー、値、ホストのデータやサービス設定を推測しません。

`ZwOpenKey` と `ZwCreateKey` は独立した不透明ハンドルを返し、クエリ、設定、子キー作成、削除の権限をハンドルごとに検査します。設定ツリーは通常の `KEY_READ`、`KEY_WRITE` を含む対応済みの `KEY_ALL_ACCESS` ビットを許可します。これは明示的にアクセス可能なテストツリーで、Windows ACL や特権評価は行いません。汎用権限、`MAXIMUM_ALLOWED`、別のレジストリビュー、独自のセキュリティ記述子、クラス、シンボリックリンクは未対応です。相対作成には `KEY_CREATE_SUB_KEY` を持つ直接の親ハンドルが必要です。入力キーは非揮発性で、新規キーは揮発性にもできますが、揮発性キーの下に非揮発性の子を作ることは拒否します。再起動やディスク永続化はモデル化しません。

`ZwQueryValueKey` は Basic、Full、Partial および定義済み Align64 情報クラスを実装し、正確な長さ、データ整列、部分出力、`STATUS_BUFFER_TOO_SMALL` と `STATUS_BUFFER_OVERFLOW` の区別に対応します。`ZwSetValueKey` と `ZwDeleteValueKey` はこのセッションだけを変更します。`ZwDeleteKey` は子キーの残るキーを拒否し、削除済みキーのハンドルは閉じるまで `STATUS_KEY_DELETED` を返します。`ZwClose` はキーとは独立してハンドルを解放し、レジストリハンドルが残った状態のアンロードは失敗します。

上限は祖先を含む 256 キー、合計 1024 値、値ごとに 65536 バイト、値データ合計 512 KiB、キーパス 1024 ASCII バイト、値名 256 バイト、同時に開くハンドル 256 個です。作成と変更にもシナリオ事前検証と同じ制限が適用されます。報告の `configuration.registry` は元の入力、`registry` は停止前の変更を含む最終的なキーと値を保持します。未指定なら null です。この値スナップショットには揮発性属性とハンドル識別子は含まれません。

## Microsoft サンプルの受け入れ検証

任意に実行する[検証スクリプト](../../scripts/validate_windows_driver_sample.py)は、[検証マニフェスト](../../unittests/emulation/fixtures/sioctl-validation.json)で固定したリビジョンの Microsoft SIOCTL ソースをダウンロードし、SHA-256 ハッシュを検証してから、未変更のソースを MinGW-w64 DDK ヘッダーでコンパイルします。指定した出力ディレクトリに、上流のライセンス／出所、ビルドコマンド、シナリオ、レポートを保存します。ネットワークアクセス、Clang、`lld-link`、`nm`、MinGW-w64 の DDK ヘッダーが必要です。

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

MinGW-w64 の include ディレクトリがデフォルトと異なる場合は `--headers` を使います。スクリプトはコンパイル済みオブジェクトの依存関係から MS COFF インポートライブラリを生成します。検証では、バッファード、in-direct、out-direct の各シナリオで DriverEntry、create、IOCTL、cleanup、close、unload を実行します。`--debug` を追加して別の出力ディレクトリを選ぶと、`DBG=1` でコンパイルし、ゲストのログメッセージも検証できます。上流サンプルは cleanup ハンドラーを登録していないため、モデルのデフォルトハンドラーが cleanup を `STATUS_INVALID_DEVICE_REQUEST`（`0xC0000010`）で完了させます。それでもドライバーは close と unload を実行し、成功した IOCTL は期待したバイト列を返します。この完全なシナリオでは、CLI の期待終了コードは **2**、`scenario_success` は false です。スクリプト自体が成功するのは、可視化された cleanup の失敗も含め、これらの結果がすべて一致した場合だけです。結果を隠すためにサンプルを書き換えることはありません。

## Zero サンプルの受け入れ検証

追加の [Zero 検証スクリプト](../../scripts/validate_zero_driver_sample.py)は、[マニフェスト](../../unittests/emulation/fixtures/zero-validation.json)のリビジョンとハッシュに基づき、Pavel Yosifovich の公開 Zero WDM サンプルを無変更でビルドします。同じツールチェーンで `python3 scripts/validate_zero_driver_sample.py` を実行します。既定では `build-release/driver-validation/zero` にソース、MIT ライセンス、コマンド、シナリオ、報告を保存します。9 個のリクエストでページ境界をまたぐ direct read、write のバイト数、ゲストのアトミック統計、buffered 統計 IOCTL を検証します。ゼロ長 read の失敗と CLEANUP ハンドラーの欠落はそのまま可視化されます。期待 CLI 終了コードは 2 で、close と unload は成功します。すべての結果と出力バイトが一致する場合のみ検証は成功します。

## レポートと SDK

JSON レポートは `stop_reason`、null を取り得る `nt_status` と `nt_success`、停止時の PC、命令数を区別します。デバイスオブジェクトやドライバーのコールバックアドレスなど、停止前に収集した API 呼び出しと観測可能な状態を保持します。ゲストアドレスは 16 進文字列として表現するため、JSON の利用側で 64 ビットの精度が失われません。

`configuration` オブジェクトには、実行の上限、サービス名、`kernel_exports` の上書き設定を記録します。プロファイルは `wdm-x64-scheduled-v3` です。`nt_status` は引き続き DriverEntry の結果を示し、`scenario_success` は初期化と完了済みリクエストを合わせた結果を示します。`phase`、`requests`、`unload_completed` は、要求されたライフサイクルのどの部分が実行されたかを示します。API 呼び出しと CPU 書き込みにも、そのフェーズ（`driver_entry`、`request:N`, `callback:N`、`unload`）を記録します。各リクエストはディスパッチと I/O のステータス、完了の有無、information 長、返された `output_hex` バイト列を報告します。`preferred_image_base` は元の PE ベースアドレスを示します。`security_cookie` は初期化した Cookie のゲストアドレスで、不要だった場合は `"0x0"` です。リクエストのレポートフィールドは `kind`、`device`、`file`、`byte_offset`、`code`、`irp`、`completed`、`dispatch_status`、`io_status`、`information`、`information_hex`、`output_hex` です。 `configuration.registry` は元のレジストリ設定を保持します。 `information_hex` は元の 64 ビット `IoStatus.Information` を 16 進文字列で正確に保持します。従来の数値フィールド `information` も保持します。

ワーク項目の観測フェーズは `callback:N` です。保留リクエストの `dispatch_status` は `STATUS_PENDING` を保持し、最終完了状態は別の `io_status` に記録され、`scenario_success` の判定に使われます。

null を取り得る `fault` オブジェクトは、最初のバックエンドフォールトを保持します。`kind`、`pc`、null を取り得る `address`、`size`、`access`、`interrupt` により、未マップまたは保護されたメモリ、無効な範囲、無効な命令、CPU 例外を区別します。アドレスは 16 進文字列、サイズと割り込みベクターは整数で表します。観測用の読み取りが元のフォールトを置き換えることはありません。フォールトが発生したバックエンドは再開できず、この記録はゲストの SEH 処理を意味しません。

`instructions` は、実行ポリシーが許可したゲスト命令の試行回数を数えます。ポリシーが拒否した命令は数えません。許可後に CPU フォールトが発生した命令は数えます。合成した API ディスパッチと戻り先の番兵は、このカウンターを増やしません。

各 `writes` 項目の `semantics: "attempted_guest_write"` は、スタック外への CPU 書き込みの試行を記録したことを示します。その後にフォールトが発生したり、予算で停止したりする試行も含みます。書き込みの完了は保証せず、API モデルによる書き込みも含みません。デバイスとドライバーオブジェクトのスナップショットは、実行停止時に観測した状態を示します。

`neverd/sdk/NeverDCAPIEmulation.h`（または C API の統合ヘッダー）を include し、セッションを作成して `neverd_emulate_driver_json(session, path, options)` を呼び出します。空でないパスを明示すると、汎用解析 API による事前ロードを経ずに、厳格な実行前検査へ直接入ります。CLI もこの経路を使います。options に `NULL` を渡すとデフォルト値を選択します。`neverd_driver_options_v1` を明示する場合は `struct_size` が厳密に一致し、命令、メモリ、イベント、タイムアウトの各予算が正でなければなりません。結果は `neverd_free_string` で解放してください。

パスに `NULL` を渡す場合はロード済みセッションが必要で、IR 解析や関数を制限したロードとは独立して、そのファイルを再解析します。どちらの経路もセッションのイメージを変更しません。呼び出し中は入力ファイルを利用可能かつ変更されない状態に保ってください。リクエスト／環境構築の失敗は `NULL` を返して `neverd_last_error` を設定し、実行の停止は JSON を返します。機能が無効なビルドでも API は存在し、有効化の方法を報告します。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` は、同じ v1 オプションと所有権規則を使い、厳密に検証するシナリオ入力を追加します。NULL ではない NUL 終端 JSON 文字列が必要です。従来の `neverd_emulate_driver_json` ABI は変更せず、初期化のみを実行します。C++ パーサー `driverOptionsFromScenarioJSON` は、`emulateDriver` の呼び出し側にも同じシナリオ検証を提供します。

内部 C++ エントリーポイントは `include/neverd/emulation/DriverSession.h` の `neverd::emulation::emulateDriver` です。形式の解析は既存ローダー、Windows のオブジェクト／API 動作は `lib/emulation/windows`、CPU の状態と実行は Unicorn アダプターが担当します。アダプターとモデルは同じゲストメモリインターフェースを使います。Windows API の動作を Unicorn fork に実装すべきではありません。
