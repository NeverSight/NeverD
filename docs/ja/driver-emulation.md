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

レポートは常に JSON として stdout に出力され、リクエストや環境構築に関する診断は stderr に出力されます。デフォルトの上限は、ゲスト命令 100000 個、ゲストメモリ 64 MiB、記録イベント 10000 件、実行時間 5000 ミリ秒です。命令数の上限は正でなければなりません。実行予算を使い切ると停止し、それまでの観測結果を残します。ただし、割り当て API は空間不足時の規定の戻り値を使い、MMIO マッピング不足は NULL を返します。

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
| `METHOD_BUFFERED` IOCTL | 逐次 buffered/direct I/O、ワーク項目または DPC による完了 | 以下の API 部分集合のみ。公開シナリオの並行送信 と 一般的な WDM 要求キャンセルは未対応 |
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT` | 要求所有 MDL、システムマッピング、共有物理ページ識別子、SG DMA | ユーザーマッピング、その他の DMA インターフェース |
| ドライバー割り当て MDL | 非ページプールまたは一つのユーザー割り当てを記述し、物理ページを共有 | IRP への関連付け、MDL チェーン、任意のプロセスは未対応 |
| READ/WRITE | 逐次 buffered/direct/neither I/O、ワーク項目または DPC による完了 | 以下の API 部分集合のみ。公開シナリオの並行送信、一般的な WDM 要求キャンセル、暗黙のファイル位置は未対応 |
| WDM `METHOD_NEITHER` | 独立ユーザーバッファー、プローブ、MDL ロック、合成要求元 ID、ディスパッチ後の VA 取り消しまたはプロセス終了、限定的なキャンセル | 一般的なプロセスアタッチや任意のユーザーマッピング・再マッピングは未対応 |
| KMDF 1.33 非 PnP ドライバー | バインド、オブジェクト／コンテキスト、名前付き制御デバイス、順次または有限または無制限の並列処理の既定キュー、実際にコールバックを実行するバッファー／直接／neither 要求 | PnP デバイス、一般のキュースケジューリング、クラス拡張、UMDF は未対応 |
| PnP バス／ファンクション／フィルタードライバー | 明示的なリソースなし／固定レジスターバンク PDO、ゲスト AddDevice、8 種の一般的な PnP ライフサイクル機能 | その他の PnP、一般的な電源管理、その他のハードウェア／リソース、一般的な KMDF PnP |
| ストレージ、ネットワーク、ディスプレイ、ファイルシステム、ミニフィルタードライバー | 各サブシステムの契約に未対応 | ポート／クラス／ミニポートのフレームワーク、NDIS/WFP、グラフィックスまたはファイルシステムのサービス |
| ワーク項目、タイマー、DPC、イベントと待機 | 現在の実行 IRQL はディスパッチとワーク項目で `PASSIVE_LEVEL`、DPC で `DISPATCH_LEVEL` です | 以下の API 部分集合のみ。公開シナリオの並行送信 と 一般的な WDM 要求キャンセルは未対応 |
| プロセス／スレッドのコールバック、ハンドル、レジストリ／ファイル操作、カーネルモジュールの検出を使うドライバー | 設定済みレジストリに対応。その他の動作は下記 API の範囲内のみ | オブジェクトマネージャー、システム状態、コールバック／イベントの発生元 |
| ハードウェア、DMA、PCI、割り込み、仮想化を扱うドライバー | 明示的なメモリレジスターバンク、MMIO、排他的 latched 割り込み、有界のコヒーレント共通バッファー／SG／チャネル DMA に対応 | その他のデバイスモデル、任意物理 RAM、PCI、ポート、その他の割り込みモード、その他の DMA インターフェース、特権 CPU 状態 |
| x86 または ARM64 Windows ドライバー | 拒否 | アーキテクチャ固有のロード、ABI、実行モデル |
| x64 CFG | 検証済みターゲット表と check/dispatch 呼び出し。非アクティブな計装はゲストのフォールバックを維持 | XFG、エクスポート抑制、未対応ロード構成、TLS は引き続き拒否 |

使用されない未対応インポートはバインドしたままにできます。未対応の操作に到達すると停止し、診断とそれまでに収集した観測結果を残します。DriverEntry の成功だけでは、その後のディスパッチ、一般のハードウェア、フレームワークの経路に対応しているとはいえません。対応するサブセットの正式な定義は、以下の API 表です。

## 実行契約

このプロファイルは CPU0 上の決定的な協調スケジューリングで x64 WDM ライフサイクルをモデル化します。 実行は PE エントリーポイントから始まり、コンパイラーが生成したエントリーラッパーがあれば、それも実行します。初期化を完了するには DriverEntry が `STATUS_SUCCESS` を返す必要があります。ゼロ以外の成功ステータスや保留ステータスは未対応の初期化契約として停止します。失敗ステータスは、完了済みの初期化結果として保持します。オブジェクト、文字列、スタック、関数ポインター、割り当てはすべてゲストメモリ内にあります。モデルは、設定されたサービス名（デフォルトは `NeverDDriver`）に対応する `DRIVER_OBJECT` とレジストリパスを提供します。

アダプターは Unicorn の仮想 TLB モードを使い、Windows のページテーブルを合成せずにゲスト仮想アドレスを保持します。これには正規形の高位カーネルアドレスも含まれます。RFLAGS の初期値は `0x202` です。ソフトウェアデバイスのプロファイルは固定の 64 バイトキャッシュラインを使用します。これらは、この実行シナリオで明示的に定めた属性です。インラインの x64 CR8 読み取りも同じ `PASSIVE_LEVEL` / `DISPATCH_LEVEL` を観測します。CR8 への書き込みとその他の制御レジスタ操作は引き続き未対応です。

未知のインポートは遅延トラップにバインドされます。未使用なら実行を妨げませんが、その thunk を実行するか、モデル化していないエクスポートデータ値を読み取ると `unsupported_api` で停止します。未対応の CPU 環境への作用も明示的に停止します。NeverD は未実装の呼び出しを成功値で置き換えません。不正なイメージや未対応のロード要件は、実行前に失敗します。

`DelayedWorkQueue` のワーク項目は `PASSIVE_LEVEL`、ゲスト DPC は規定の四引数で `DISPATCH_LEVEL` にて実行します。CPU0 上で呼び出しの復帰とブロッキング待機の境界に決定的な協調スケジューリングを行います。相対・絶対・周期タイマーは仮想時間を使い、実行可能なフレームがなければ次のタイマー、待機、キャンセルの期限へ進めます。通知型と同期型のイベント／タイマーは異なるシグナル消費を保持します。各コールバックは独立したゲストスタックを持ち、複数の待機フレームのローカル変数と完全な CPU コンテキストを保持しつつ、ゲストメモリを共有します。Win64 コールバックの先頭四引数はレジスタ、それ以降はスタックに渡します。要求は逐次処理し、IRP を保留としてマークしたディスパッチは `STATUS_PENDING` を返し、次の要求の前に完了する必要があります。保留要求や無限待機に実行可能な生成元がなければ、停滞した `model_error` で停止します。命令・メモリ・観測・実時間の予算は共有します。

これは限定的なスケジューリングモデルであり、完全な Windows 非同期対応ではありません。アラート可能／ユーザーモード待機、APC、一般的な WDM 要求キャンセル、公開シナリオの並行送信、UMDF、一般的な KMDF PnP デバイスと一般のキュースケジューリング、完全な PnP／電源、一般のハードウェア、その他の DMA インターフェース、その他の割り込みモードは未対応です。初期化のみの呼び出しも明示的に登録したコールバックを実行しますが、要求やアンロードを暗黙には生成しません。

ワーク項目はコールバック開始前にキューから外れるため、コールバックは自身の項目を解放できます。キュー内の項目の解放、二重登録、失効したオブジェクト、実行可能なゲストメモリ外のコールバック先は明示的に失敗します。デバイス参照はコールバックが戻るまで保持します。アンロードには全ワーク項目の解放とキュー内の処理の完了が必要です。CPU コンテキストは汎用、SIMD、FPU、制御状態を保存・復元します。ゲストメモリは共有され、障害後の CPU を保存コンテキストで再開することはできません。
ファイルオブジェクトやキュー内／実行中ワーク項目の参照が残る間は削除を延期します。オブジェクト領域が不足するとワーク項目の割り当ては NULL を返します。

シナリオで有効な再配置先アドレスを選ばない限り、イメージは優先ベースアドレスを使用します。イメージは native サブシステムを持つ PE32+ x64 実行ファイルでなければなりません。インポート元には `ntoskrnl.exe`、`ntkrnlmp.exe` または `WDFLDR.SYS` を使用できます。

実行ローダーは、検証済みの x64 `DIR64` ベース再配置と、限定されたセキュリティ Cookie のロード構成に対応します。エントリーラッパーの実行前に、決定的なゲスト Cookie を初期化します。その他の未モデル化ロード構成フィールド、TLS、遅延／バインド済みインポート、序数によるインポート、マネージドイメージは拒否します。イメージは厳格な範囲とアラインメントの検査にも合格する必要があります。

有効な Control Flow Guard（CFG）は PE フラグ、ポインタスロット、ソート済み実行可能ターゲット表を検証します。check/dispatch ヘルパーは宣言済みイメージ入口または登録済み API サンクだけを許可し、Win64 呼び出し状態を保持して未宣言ターゲットを拒否します。CFG を有効にしない計装では元のゲストのフォールバックポインタを維持します。有効な XFG、エクスポート抑制、その他の未モデル化ポリシーは拒否し、実行可能メモリ内という理由だけでターゲットを許可しません。

WDM スタックには同じゲストドライバーが所有する複数のデバイスを配置できます。`IoAttachDeviceToDeviceStack` は独立したソースを対象の現在の最上位に接続し、以前の最上位を返します。`StackSize` と `AlignmentRequirement` を設定しますが、`NextDevice` リストやバッファーフラグは変更しません。`IoDetachDevice` は保存した下位デバイスを受け取り、`PASSIVE_LEVEL` を要求します。接続は `DISPATCH_LEVEL` 以下で実行できます。名前付き下位デバイスを開くと現在の最上位へディスパッチされますが、`FILE_OBJECT.DeviceObject` とレポートは名前付きデバイスを保持します。READ/WRITE のバッファー方式は選択した最上位のフラグで決まります。要求が保持する経路は切断・削除後もディスパッチの復帰まで各デバイスを保持し、内部参照は開いたハンドル数を示す `ReferenceCount` に加算しません。

`IofCallDriver` と `IoCallDriver` ヘルパーは保持経路内の指定された対象を直接呼び出します。実際のインライン `IoCopyCurrentIrpStackLocationToNext`、`IoSkipCurrentIrpStackLocation`、`IoSetCompletionRoutine` は元のゲスト IRP を操作し、モデルはカーソル、個数、制御フラグを検証します。下位ディスパッチの実際の戻り値、`IoStatus`、完了ルーチンの戻り値は別物です。完了処理はカーソルを進め、成功／エラー／キャンセル条件でコールバックを選び、pending を上位へ伝播します。完了ルーチンが呼ばれる場合はそのルーチンが伝播を担当し、先に `STATUS_PENDING` が返った後でも伝播できます。`STATUS_MORE_PROCESSING_REQUIRED` は IRP、MDL、バッファーを保持したまま展開を止め、後の完了呼び出しで再開できます。入れ子の完了には外側の停止結果が必要で、最終展開時に一度だけストレージを解放します。所有サブシステムを示す継続が WDM／WDF の呼び出しフレームと継承 IRQL を保持します。 上位の完了コールバックを実行する前に、消費済みの下位スタック位置をゼロにします。

一般的な電源管理、ドライバー割り当て IRP、その他の PnP マイナー機能、その他のハードウェア／リソースモデルは未対応です。WDF の接続／転送、ファイルやコールバックが残るスタックへの接続、中間層の切断、転送時のメジャー機能変更、保持経路外への転送は明示的に失敗します。実 WDK の任意フィクスチャ `driver_wdm_stack.c` は `NEVERD_WDM_STACK_FIXTURE` と `NEVERD_WDM_STACK_CFG_FIXTURE` で指定します。ネイティブと C API／CLI の検証には再配置も含み、成果物がなければ明示的にスキップします。実行証拠は Linux のみです。

シナリオは `pnp_devices` を最大 64 個、明示的に設定できます。各項目には `id`、`bus: "resource_free"` または `bus: "register_bank"`、`initial_device_power: "D0"`、`initial_system_power: "working"` が必須で、省略情報を推測しません。ID は大文字小文字を区別する 1–64 バイトの ASCII で、先頭は英数字、以降は英数字、`_`、`-`、`.` のみです。通常の要求は `device` の代わりに設定済みの `device_id` を使えますが、併用できません。`kind: "pnp"` には `device_id`、`minor`、`bus_completion` が必須です。対応する minor は `start`、`query_remove`、`cancel_remove`、`remove`、`query_stop`、`stop`、`cancel_stop`、`surprise_removal`。`bus_completion.status` は 32 ビット整数または 16 進文字列で必須、任意の `delay_100ns` は INT64_MAX 以下の非負整数で、プロバイダーの実受信時から計測します。最終状態に `STATUS_PENDING` は指定できず、stop/cancel-stop/surprise-removal/cancel-remove/remove は正確に `STATUS_SUCCESS` (0) が必要です。PnP 要求はファイル・転送・キャンセルのフィールドをゼロでも拒否します。C++ API も同じ事前検証を適用します。

```json
{
  "pnp_devices": [
    {"id": "sensor0", "bus": "resource_free", "initial_device_power": "D0", "initial_system_power": "working"}
  ],
  "requests": [
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0", "delay_100ns": 10}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "surprise_removal", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "remove", "bus_completion": {"status": "0x0"}}
  ],
  "unload": true
}
```

DriverEntry 成功後、設定された PDO ごとに `AddDevice` を一度実行します。プロバイダーは独立した `DRIVER_OBJECT` を所有し、ゲストはそのオブジェクトを削除・偽装できません。PnP IRP は `KernelMode`、ファイルなしで、START リソースは設定バスに従い、初期状態は `STATUS_NOT_SUPPORTED` です。対象 PDO まで転送した場合だけバス応答を使用し、遅延完了は共通の仮想時計と完了継続を使います。ライフサイクルの確定／ロールバックは、バス状態とは別に最終上位完了が決めます。Started から通常削除するには query 成功、ファイルのクローズ、先行要求の終了、ゲストによる切断／削除が必要です。リークのない AddDevice 失敗ではプロバイダーだけを破棄します。新規ゲストデバイスのリークは切断済みでも `model_error` です。unload 前には全プロバイダーが不在でなければなりません。その他の PnP、一般的な電源管理、その他のハードウェア／リソース、一般的な KMDF PnP は含みません。 PnP 成功には実際のプロバイダー完了が必要です。START/QUERY_STOP/QUERY_REMOVE の早期上位失敗ではバス観測を null のまま保持できます。デバイス／ファイルのライフサイクル識別は切断後も維持します。

初期設定は `configuration.pnp_devices` に保持します。観測された `pnp_devices` は `id`、`pdo`、nullable な `add_device_status`、現在の `attached`、`pnp_state`、`provider_present` を持ち、削除後の `attached` は false です。AddDevice のフェーズは `add_device:<ID>`。失敗は `scenario_success` に反映し、DriverEntry の `nt_status` は上書きしません。各要求に nullable な `device_id` と `pnp` を追加し、PnP の `file` は null です。`pnp` は `minor`、`state_before`、`state_after`、nullable な `bus_status`、`bus_received_at_100ns`、`bus_completed_at_100ns` を記録します。設定した状態は実際のバス完了時だけ観測となり、受信時刻は独立です。既存フィールドの型は変わりません。

Removing/Removed 以外でデバイスが存在する限り、通常の CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE は実際のゲストへディスパッチします。モデルは Stopped、StopPending、RemovePending、電源状態だけで失敗を生成しません。ドライバー自身がソフトウェア I/O の完了、拒否、保持を決めます。公開実行器は直列です。保持 IRP に現在利用可能な生成元がなければ、後続シナリオの start や cleanup で解除できず、停滞 `model_error` になります。Remove 前のファイルクローズと先行要求終了はプロファイル上の制限です。`query_stop` の最終 `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) は未実装のリソース再照会を要求するため、事前検証とゲスト最終完了の両方で拒否します。[Microsoft の QUERY_STOP 契約](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device)を参照してください。停止／再開と突然の取り外しは、リソース再配分、一般的な電源管理、一般的な KMDF PnP 対応を意味しません。

リソースなし PnP は独自の実 WDK `driver_wdm_pnp.c`、任意の `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE` とネイティブ／C API／CLI テストを使います。成果物なしは明示的にスキップし、実行証拠は Linux のみです。

合成バス `bus: "register_bank"` は PDO に明示的な固定メモリリソースを追加します。`interrupts` が空でなければ `resources` 配列は省略でき、両リストを合わせて少なくとも一つのリソースが必要です。各メモリー項目には `id`、`raw_start`、`translated_start`、`length`、`registers` を指定し、各レジスターには `offset`、`width`、`access`（`read_only` または `read_write`）、初期 `value` が必要です。`DriverResources.h`／`DriverResources.def` が C++ と JSON の共通契約を定義します。リソース ID は長さ制限付き ASCII 識別子の規則に従い、PDO 内で一意です。上限は PDO ごとに8／全体32リソース、リソースごとに256／全体4096レジスター、リソース長は1–1048576バイトです。自然整列した正確な1／2／4バイトのアクセスだけを扱い、値は指定幅に収まる必要があります。両物理区間はオーバーフロー不可で、raw 区間は PDO 内、変換後区間は全体で重複できません。空の `registers` はバンク全体がアクセス不可であることを明示します。アドレスと初期値は宣言された情報であり、ホストハードウェアや暗黙のゼロ埋めではありません。`resource_free` はリソース一覧を省略し、START ポインターは null のままです。

START には別々の読み取り専用 raw／変換後 `CM_RESOURCE_LIST` を渡します。対応する Memory 記述子の順序は同じで、full 記述子1個、Internal インターフェイス、バス0、version／revision 1、DeviceExclusive 共有、READ_WRITE 範囲フラグを使います。レジスター自体の RO 権限は独立します。下位 START が成功すると、上位完了コールバックより前に割り当てが利用可能になります。NotStarted／Stopped からの各 START は同じ固定割り当てに新しいリソース世代を作ります。値は PDO 作成時に一度だけ初期化し、unmap、STOP、再開を越えて保持します。START 失敗および STOP／REMOVE 成功では、最終 IRP 完了前にドライバーがマッピングを解放する必要があり、自動清掃は行いません。突然の取り外しは新規 map とレジスターアクセスを直ちに禁止しますが、既存の unmap は可能です。プロバイダーのデバイス SET が実際に成功すると可用性が変わり、D3 はアクセス禁止、D0 は利用可能な割り当てがある場合だけ許可します。D3 でもアクセスを伴わない map は可能で、電源変更はマッピングや値を破棄しません。

`MmMapIoSpace` は NonCached、`MmMapIoSpaceEx` は PAGE_NOCACHE と PAGE_READONLY または PAGE_READWRITE の組み合わせを扱います。単一割り当て内の宣言済み変換後部分区間のみを、ページ内オフセットを保持してマップします。別名は一つのバンクを共有し、権限は独立します。`MmUnmapIoSpace` は元の基点と正確な長さが必要です。マッピング数／仮想アドレス枠や設定済みメモリ予算の不足は NULL、バックエンド障害は明示的な失敗になります。unmap 済みアドレスは後の map で復活しません。スカラーと実際の REP レジスターバッファー命令は CPU MMIO 検査を通ります。未宣言領域、幅違い、非整列、RO 書き込み、マッピング境界越え、実行はレジスターへの効果より前に失敗します。メモリ API も自然整列した単一レジスターへの正確な1／2／4バイト取引を実行できます。MMIO に触れるそれより大きな一括範囲は、レジスター取引へ自動分割せず明示的に拒否します。任意の物理 RAM、リソース再配分、ポート、その他の割り込みモード、その他の DMA インターフェース、一般的なハードウェア動作は含みません。

実行可能な[レジスターバンクのシナリオ](../examples/driver-register-bank-scenario.json) は独自の真正 WDK `driver_wdm_resources.c` と任意の `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE` を使います。遅延 START、ファイル I/O、STOP、再開、取り外しの14要求を実行し、ドライバー IOCTL で保持値を観測します。`configuration.pnp_devices[].resources` は物理アドレスを精度損失のない16進数で保存した初期情報であり、別のバンク状態報告ではありません。C API と Python は既存 `scenario_json` と変更のない `neverd_driver_options_v1` を使います。真正成果物がなければ明示的にスキップし、現在の実行証拠は Linux のみです。

同じ `register_bank` プロバイダーで `interrupts` 単独、または `resources` と両方を宣言できます。少なくとも一方のリストは空でない必要があります。`resource_free` は `[]` を含め、明示的な `interrupts` フィールドを拒否します。各割り込みには `id`、`raw_vector`、`raw_level`、`raw_affinity`、`translated_vector`、`translated_level`、`translated_affinity`、`mode: "latched"`、`share: "device_exclusive"` が必要です。`DriverInterrupts.h` / `DriverInterrupts.def` が型、表記、上限を管理します。PDO ごとに 8 件／全体で 32 件、PDO 内で一意な長さ制限付き ASCII ID、全体で排他的な変換後ベクター、raw level 0–65535、変換後 DIRQL 3–12 を許可します。ベクターは 32 ビット全体を保持し、ベクターと IRQL の関係を推測しません。両アフィニティーマスクは 1 が必須で、CPU0／group0 は明示的なプロバイダー情報です。raw 値と変換後値は独立です。同じ `CM_RESOURCE_LIST` ではメモリー記述子の順番を保ち、その後に順序付き割り込み記述子を配置します。割り込み記述子は type 2、DeviceExclusive、LATCHED フラグを使用します。この合成エッジトリガープロファイルは PCI の共有レベル割り込み線を表しません。

READ／WRITE／IOCTL 要求は `interrupt_events` を宣言でき、各要素に `after_100ns`、`device_id`、`interrupt_id` を明記します。他の要求種別では空でもこのフィールドを拒否します。要求ごとに最大 64 件／合計 1024 件、遅延は INT64_MAX 以下の非負値です。要求送信が成功した時点を遅延の起点とし、既に接続済みの割り込みと PDO のリソース世代を捕捉します。イベントは独立した外部ラインパルスです。発生元 IRP の完了では取り消されず、レジスターへの書き込みから enable、status、acknowledgement の動作を推測しません。時間はアイドル時だけ進み、期限に達したイベントは次の対応するコールバック境界で実行します。そのため `after_100ns: 0` は命令単位のプリエンプションや最初のゲスト命令より前の配信を保証しません。同時刻の生成元はまとめて容量を事前検証します。プロバイダーのハードウェア状態公開後に配信可否を判定し、ISR は DPC／ワーカーより先に実行します。接続喪失、古い／利用不能な世代、物理的 D3 は `undelivered_reason` を記録し `model_error` で停止します。別の接続への付け替えや黙った延期は行いません。

`IoConnectInterrupt` は実際の 11 引数と正確な変換後割り当てを使用します。`IoConnectInterruptEx` は FullySpecified（1）、LineBased（2、明示した PDO の割り当て済み 1 ライン）、FullySpecifiedGroup（4、group 0）に対応し、`IoDisconnectInterruptEx` は一致するバージョン／コンテキストを要求します。登録と切断は PASSIVE_LEVEL が必要です。専用割り込みロック、排他的 latched モード、CPU0／group0、浮動小数点状態の保存なしのみ対応します。同期 IRQL は割り当て DIRQL と一致しなければならず、LineBased のゼロはそのレベルを選びます。`KINTERRUPT` は不透明で、アドレスを再利用しません。実際の ISR は `(Interrupt, ServiceContext)` を受け取り AL の BOOLEAN を返します。`FALSE` は未受理であり NTSTATUS 失敗ではありません。`KeSynchronizeExecution` は同じロック下、DIRQL で実際の 1 引数コールバックを実行し、その BOOLEAN を返して呼び出し元 IRQL／CR8 を復元します。`KeAcquireInterruptSpinLock` / `KeReleaseInterruptSpinLock` は非再帰の所有権、元の実行コンテキスト、保存 IRQL を検証し、ロックを保持したままコールバックを終了できません。割り込み内の待機、呼び出し元提供の共有ロック、共有／レベル／MSI／パッシブ割り込み、命令単位の割り込みプリエンプションは未対応です。START 失敗と STOP／REMOVE 成功は最終完了前の切断を必要とし、上位完了コールバックで先に解放できます。切断時にキュー内 DPC を黙って捨てることはありません。

レポートは宣言と観測を分けます。`configuration.pnp_devices[].interrupts` はリソースを保持し、`configuration.interrupt_events` は入力イベントをゼロ始まりの `source_request_index`（設定要求の添字）と `event_index` 付きで平坦化します。ルートの `interrupts` 行は `device_id`、`interrupt_id`、`epoch`、絶対時刻 `due_at_100ns` と、null 許容の `occurred_at_100ns`、`delivered_at_100ns`、`returned_at_100ns`、`interrupt_object`、`return_value`、`claimed`、`undelivered_reason` を追加します。`claimed` は実際の戻り値の下位バイトだけから求めます。時刻は観測値であり、作り出したコールバック結果ではありません。`scenario_success` には全設定イベントが未配信失敗なしに戻る必要がありますが、未受理を返す ISR は有効です。DPC の効果は実際の要求完了、API 呼び出し、メッセージに現れます。実行可能な[割り込みシナリオ](../examples/driver-interrupt-scenario.json)は、真正 WDK でコンパイルする独自 fixture `driver_wdm_interrupts.c` と任意設定 `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE` を使用します。7 要求で遅延 START、ISR→DPC が完了する保留 IOCTL、ファイル cleanup／close、取り外しを実行します。既存 C／Python の `scenario_json` 境界と `neverd_driver_options_v1` の配置は変わりません。真正成果物がない場合は明示スキップし、実行の証拠は Linux に限られます。

`DriverDMA.h` / `DriverDMA.def` は、メモリー／割り込み割り当てを持つ `register_bank` PDO に任意の `dma` オブジェクトを追加します。DMA だけではこれらのリソースリストを代替できません。7 フィールドはすべて明示指定です：`address_bits`（32／64）、`maximum_length`（1–1048576 バイト）、`map_registers`（1–256）、`alignment`（1–4096 の 2 の累乗）、`logical_base`（非ゼロ、ページ整列）、`logical_length`（ページ整列、4096–1073741824 バイト）、真偽値 `scatter_gather`。論理範囲はオーバーフローせずアドレス幅内に収まる必要があります。PDO ごとの論理ドメインは独立し、別デバイスの同じアドレスは別名になりません。変換後 MMIO リソースは予約済みモデル RAM 範囲 `[0x1000000000, 0x1000100000)` と重複できません。これはコヒーレントな合成バスマスターの宣言であり、ホスト物理メモリーや PCI デバイスを表しません。

`IoGetDmaAdapter` は Internal バスマスター用の旧来の `DEVICE_DESCRIPTION` バージョン 0／1 フィールドを受け付け、バージョン 1 の `DMA_ADAPTER` と実際の 104 バイト `DMA_OPERATIONS` テーブルを公開します。バージョン 2／3 の照会は新しい構造体の末尾を読まず NULL を返します。各間接メソッドは正確な生存アダプターに結び付き、カーネルインポートとは別の識別子を持ちます。対応するのは `AllocateCommonBuffer`、`FreeCommonBuffer`、`GetDmaAlignment`、`GetScatterGatherList`、`PutScatterGatherList`、`PutDmaAdapter`、`AllocateAdapterChannel`、`MapTransfer`、`FlushAdapterBuffers`、`FreeMapRegisters` です。従属／システム DMA コントローラーをモデル化していないため、`FreeAdapterChannel` と `ReadDmaCounter` は名前付きの未対応エラーとして残ります。共通バッファーの割り当て／解放と整列照会は PASSIVE_LEVEL、Get／PutScatterGatherList は DISPATCH_LEVEL が必要です。アダプター解放は DISPATCH_LEVEL 以下で可能です。x64 では `CacheEnabled` を無視します。未対応バージョンの照会、宣言済み能力との不一致、文書化された割り当て不足は NULL を返します。不正または未モデル化のインターフェース選択とバックエンド障害は明示的なエラーになります。

`AllocateAdapterChannel` は DISPATCH_LEVEL を要求し、NULL ではない不透明なマップレジスタートークンを予約します。共通バッファー、SG リスト、チャネル予約は PDO ごとの同じ割り当て上限と FIFO を共有します。成功すると実際の `AdapterControl` を即時実行するかキューへ入れます。要求数が過大ならコールバックなしで `STATUS_INSUFFICIENT_RESOURCES` を返します。ゲストデバイスごとに未完了の割り当てコールバックは一つだけ許可します。AdapterControl 内からの AllocateAdapterChannel は、入れ子のコールバック経由でも拒否します。コールバックの 4 引数には登録時に捕捉した実際の `DEVICE_OBJECT.CurrentIrp` が含まれます。この正確な 8 バイトフィールドはゲストデバイスで書き込み可能で、ゼロまたはそのデバイスを経由する有効な IRP を受け付けます。待機コールバックは開始までパケットを保持し、開始後は自分で完了できます。StartIo は引き続き未対応なので、別の SG コールバックの未使用 IRP 引数は NULL のままです。

`AdapterControl` の戻り値は 32 ビットの `IO_ALLOCATION_ACTION` です。RAX の上位ビットは無視し、アクションで AllocateAdapterChannel の `STATUS_SUCCESS` を置き換えません。`DeallocateObject` はコールバックの return 時に未使用または全体をフラッシュ済みのレジスターを解放します。`DeallocateObjectKeepRegisters` は、正確なアダプター、トークン、元の個数による FreeMapRegisters まで保持します。`KeepObject` は未モデル化のシステムコントローラーを要求するため明示的に失敗します。新たに配信した割り当ては、コールバックの return 前に保持済み割り当てとして解放できません。別の以前から保持している割り当ては、自身の契約が許せば解放できます。コールバック識別子、保持レジスター、マッピング中のバイトは寿命が別で、アダプターやデバイスの破棄前にすべて検証します。

`MapTransfer` と `FlushAdapterBuffers` は DISPATCH_LEVEL 以下で使用でき、チャネル割り当てとレジスター解放は DISPATCH_LEVEL を要求します。MapTransfer は MDL 相対の位置を受け取り、実際の ULONG 長を読み書きし、論理アドレスを値として返します。有界 SG プロファイルは一回につき基底ページの一断片を返します。同じ MDL と方向の連続した後続位置は一つの操作を拡張します。非 SG では予約個数に収まる要求範囲全体を一回でマッピングし、長さを短縮しません。最初のマッピングでレジスター予約に対応する大きさの再利用されない論理領域を確保し、他の割り当てが途中に入っても重複させません。全断片は一つの拡張可能な物理固定範囲を共有します。デバイストランザクションはその時点のマッピング全体にまたがれますが、CPU アクセス、MDL 解放、固定ストレージを無効化する完了は全体のフラッシュまで拒否します。Flush は最初の位置、MDL、方向、実際にマッピングした合計長に一致する必要があります。バイトのマッピングを解放してもレジスターは解放しないので、保持したトークンは次の操作に使えます。部分フラッシュ、複数 MDL の混在、その他の MapTransfer パターンはこのプロファイルの範囲外であり、すべての Windows で不正だとはみなしません。

`KeFlushIoBuffers` は有効なロック済み／非ページ MDL を検証します。モデルのプラットフォームはコヒーレントなので、ReadOperation と DmaOperation のいずれの値でも別のキャッシュコピーは不要です。DMA 所有権を解放せず、FlushAdapterBuffers の代わりにもなりません。[チャネルシナリオ](../examples/driver-dma-channel-scenario.json)は独自の `driver_wdm_dma_channel.c` で、二回の MapTransfer、ページをまたぐ一つのデバイストランザクション、別途宣言した IRQ/DPC、全体のフラッシュ、正確なレジスター解放を実行します。真正の通常／CFG イメージには `NEVERD_WDM_DMA_CHANNEL_FIXTURE` と `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE` を使います。

`KernelPhysicalMemory` は既存 RAM に最大 256 個の 4096 バイトモデル物理ページ識別子を割り当てます。CPU 仮想アドレス、物理ページ識別子、デバイス論理アドレスは区別します。構築済み MDL の PFN 配列は共有識別子を読み取り専用で公開し、未構築の記述子には利用可能な PFN がありません。小さい隣接割り当ては PFN を共有しても、バイト範囲と寿命は独立します。共通バッファー、プール、要求バッファーは `GuestMemory` の同じバイト列を使い、DMA コピーを別に持ちません。生存 SG マッピングは正確なデータ範囲と記述子を保持し、完了、プール／MDL 解放、取り外しは依存を失効処理前に検証します。直接 MDL の unmap は CPU システムマッピングだけを取り消し、DMA はロック済みの基底 RAM に到達できます。`DmaWritable` は CPU 権限と別に書き込みロック契約を記録します。デバイス書き込みには直接 READ／OUT_DIRECT または書き込み可能な非ページストレージが必要で、WRITE／IN_DIRECT は CPU マッピングが書き込み可能でもこの権限を得ません。

`GetScatterGatherList` は MDL の元の範囲に対して CurrentVa／Length を検証し、既存 RAM 上に論理ページ断片を作ります。マップレジスターに空きがあれば、実際の 4 引数 void `AdapterListControl` を API が戻る前に入れ子で実行します。空きがなければデータ／記述子を保持し、PDO の FIFO で資源解放までコールバックを予約します。この範囲には StartIo 所有権がなく、第 2 IRP 引数は NULL です。コールバックの return はマッピングを解放しません。`PutScatterGatherList` はコールバック内で実行でき、その後に要求完了と最後のアダプター解放も可能ですが、継続処理とデバイス参照は return まで維持します。生存 SG マッピング中の CPU データアクセスには先に Put が必要です。マップレジスター待ちのコールバックはまだバイト列のデバイス所有権を取得していません。共通バッファー解放では元のアダプター、長さ、論理アドレス、CPU アドレスが一致する必要があります。論理アドレスは再起動を含むセッション全体で再利用しません。実際の生成元がない資源待ちは明示的に停止し、完了や期限を捏造しません。

`dma_events` は READ／WRITE／IOCTL 要求だけで指定できます。各イベントには `after_100ns`、`device_id`、`logical_address`、`direction`、`length` が必要です。`write_memory` は正確な長さの `data_hex` も必要とし、`read_memory` はこのフィールドを拒否します。方向はデバイスの視点です。上限は要求ごとに 64 件、全体で 1024 件、転送総量 16 MiB、1 取引 1 MiB、遅延は非負の INT64_MAX 以下です。送信時に PDO の現在の割り当て世代と仮想時刻の起点を捕捉しますが、その後のディスパッチが作るマッピングはまだ不要です。配信時に完全な生存論理範囲と方向を解決し、物理 D0 と全基底バイトを効果前に検証します。元 IRP の完了はイベントを取り消しません。マッピング不在／解放済み、古い世代、突然の取り外し、D3 は失敗を記録して停止します。再結合や割り込み、レジスタープロトコル、IRP 完了の捏造は行いません。同じ境界ではプロバイダーのハードウェア状態公開、DMA バイトアクセス、独立に宣言された割り込みパルスの順になります。実行は協調的で、命令単位のプリエンプションはありません。

レポートには `configuration.pnp_devices[].dma` と平坦化した `configuration.dma_events` を残します。ルートの `dma_transfers` 行は `source_request_index`、`event_index`、`device_id`、`epoch`、`logical_address`、`direction`、`length`、`due_at_100ns` を識別し、`occurred_at_100ns`、`completed_at_100ns`、`mapping`、`adapter`、`failure_reason` は null になり得ます。`data_hex` は実際の転送バイトだけです。`scenario_success` には宣言した全取引の失敗なしの完了が必要です。[DMA シナリオ](../examples/driver-dma-scenario.json)は真正 WDK の独自 `driver_wdm_dma.c` と任意の `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE` を使い、実際のアダプターポインターで共通バッファーを操作し、別途宣言した ISR→DPC で完了します。C／Python は引き続き `scenario_json` を使い、`neverd_driver_options_v1` は変わりません。成果物不足は明示スキップし、実行証拠は Linux のみです。従属コントローラー、V2／V3 メソッド、ハードウェア記述子エンジン、一般 KMDF DMA、その他のデバイスモデルは未対応です。

WDM remove-lock は実際のエクスポート `IoInitializeRemoveLockEx`、`IoAcquireRemoveLockEx`、`IoReleaseRemoveLockEx`、`IoReleaseRemoveLockAndWaitEx` を実行します。Ex なしの WDK 名はマクロです。ロックは整列した全領域を拡張部に含む正確な DEVICE_OBJECT に属し、PDO 状態や Tag の形では所有者を決めません。接続前の初期化も可能です。retail 32 バイト／DBG 120 バイトと一致する独立サイズ引数を要求し、登録領域全体は不透明です。NULL／重複 Tag はロックごとに計数し、逆参照しないため IRP 完了後にも解放できます。初期化／AndWait は `PASSIVE_LEVEL`、acquire/release は `DISPATCH_LEVEL` まで対応します。

AndWait は新規取得を閉じ、一致する取得を1件解放して、残りがなくなるまで実際のゲストフレームを中断します。以後の acquire は `STATUS_DELETE_PENDING` で新たな解放義務を作りません。最終 release はコールバック復帰前に準備完了をラッチするため、ワーカーが release 後に REMOVE の継続からのイベントを待てます。架空のコールバック、タイムアウト、生成元なしの成功は作りません。所有者を含む関連 REMOVE 経路と実際のプロバイダー受信が必要で、`bus_received_at_100ns` のゼロも有効です。下位完了は不要ですが、プロバイダー到達前の下位キュー保持は対象外であり、完全な OutsideRemoveDevice／Driver Verifier 検証ではありません。REMOVE 前のファイル閉鎖・先行要求終了は必要ですが、ロックを解放するコールバックは残せます。経路は待機、切断／削除、下位 pending を越えて保持し、残るフレームが復帰してから最終解放します。

未知／サイズ不一致、未対応の release、二重 drain、再初期化、取得や未消費 drain 待機が残る拡張部の削除は変更前に失敗します。AddDevice 失敗では初期化済み未使用ロックを削除できます。ロックはデバイス／ワーク項目参照の代わりではなく、拡張部の実際の退役時に登録解除します。デバッグ情報は Verifier の時間／高水位制限を有効にしません。真正 `driver_wdm_remove_lock.c` は `NEVERD_WDM_REMOVE_LOCK_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` の4変種を使います。欠落は明示スキップ、native と C API／CLI の証拠は Linux のみで、完全な削除管理や一般並行 I/O の排出を意味しません。

WDM 電源要求は `kind: "power"` と設定済み `device_id` を使います。各パケットに `minor`（`query`／`set`）、`power_type`（`device`／`system`）、`power_state`（`D0`／`D3` または `working`／`sleeping3`）、`power_action`（`none`／`sleep`）、32 ビット整数または16進文字列の `system_context`、`bus_completion` が必須です。System Query の Working 目標、ファイル・転送・キャンセルのフィールドは未対応です。`system_context` 全体は不透明な明示値として保持し、親要求や休止／高速スタートアップを推定しません。経路は `DO_POWER_PAGABLE` が必要で `DO_POWER_INRUSH` を禁止し、電源ディスパッチと `PoRequestPowerIrp` は `PASSIVE_LEVEL` で実行します。`PoCallDriver` は同じ管理対象電源 IRP を転送し、`PoStartNextPowerIrp` は追加の直列化ハンドシェイクがない Vista+ 契約に従います。一般の電源ポリシー、WAIT_WAKE、その他の状態／動作、シャットダウン／休止、突入電流、非ページ可能経路、一般のハードウェア、一般的な KMDF PnP は対象外です。

`pnp_devices` の各要素には、必須の初期ライフサイクル D0／working と独立した `initial_reported_device_power: "D0"` または `"D3"` を指定できます。PDO と初めて関連付けた各ゲスト DEVICE_OBJECT は通知状態を個別に持ち、`PoSetPowerState` は呼び出し元デバイスの前値だけを返して更新します。初期値なしの実呼び出しは D0 を仮定せず失敗します。任意の `requested_device_power` は同じ6項目を持つ device 型テンプレートで、全 PDO 合計64件までです。実際の `PoRequestPowerIrp` の PDO・minor・目標がその PDO の FIFO 先頭に一致した場合のみ消費し、欠落／不一致は失敗、未使用分は要求を生成しません。context から親を推定しません。子は独立 IRP／報告行を持ち、`origin: "PoRequestPowerIrp"` と0始まりの `response_index` を記録します。シナリオ行は `origin: "scenario"` と null 索引です。同期子は API が `STATUS_PENDING` を返す前に5引数 void コールバックを実行でき、待機も可能です。System S0 は独立した D0 子より先に完了できます。IO_STATUS_BLOCK スナップショットはコールバック復帰まで有効です。

報告に nullable `power` を加え、電源行の `file` は null です。`power` はパケット値、`device_state_before`／`device_state_after`、`system_state_before`／`system_state_after`、nullable `requested_device_object`、実際の `bus_status`／`bus_received_at_100ns`／`bus_completed_at_100ns` を含みます。最終 PnP 観測は `device_power`／`system_power`、存続デバイスは nullable `reported_device_power` を追加します。`scenario_success` の件数比較はシナリオ由来だけですが、実際の全シナリオ／子要求の成功完了が必要で、未使用テンプレートは失敗を生みません。真正 `driver_wdm_power.c` は `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE` を使用し、通常／active-CFG のネイティブと C API／CLI を検証します。成果物欠落は明示的にスキップし、実行証拠は Linux のみです。

[完全な電源シナリオ](../examples/driver-power-scenario.json) を `--scenario` に渡すと、開始、システム query／スリープ／復帰、削除と3件の明示子応答を真正フィクスチャで実行できます。

KMDF 1.33 対応は正確な 1.33.0 ABI を使用します。458 個の関数スロットに安定したゲスト識別子を割り当て、以下の 72 API に実行意味論を実装しています。`WdfVersionBind` と `WdfVersionUnbind` は実際の WDK `FxDriverEntry` ラッパーの前後でゲストのバインドを管理します。`WdfGetDriver` は公開ドライバーグローバル構造を読みます。非 PnP ドライバー、汎用オブジェクト、制御デバイス、キュー、受信要求は、型付きコンテキスト、参照カウント、実際に実行するクリーンアップ／破棄／アンロードコールバックを共有します。モデル化したすべてのフレームワーク呼び出しとコールバックは現在 `PASSIVE_LEVEL` を必要とし、クリーンアップ完了後の新しい参照取得はこのプロファイルの対象外です。未実装スロット、`WdfLdrQueryInterface`、クラス拡張、UMDF は明示的に停止します。

制御デバイスには、コピーされた表示可能 ASCII 名と、正確に `D:P(A;;GA;;;WD)` という SDDL が必要です。すべての呼び出し元にアクセスを許可し、呼び出し元トークンを仮定しません。他のセキュリティ記述子、名前なしデバイス、自動命名は未対応です。デバイス初期化は一つの WDM デバイスを所有します。要求は既存のセッション名前空間のシンボリックリンク別名 `\DosDevices\Name` または `\??\Name` でデバイスを選択でき、レポートには正規のデバイス名を保持します。作成成功時は初期化オブジェクトを消費してそのポインターをクリアし、失敗時は部分的なデバイス所有権をロールバックします。`WdfControlFinishInitializing` が I/O 配信を許可します。モデル内のファイル、ワークアイテム、要求が許可する場合にのみ、削除によってデバイスとリンクを除去します。削除時のキャンセルや要求の排出は未対応です。

96 バイトの `WDF_IO_QUEUE_CONFIG` は、明示的なパッシブ実行とフレームワーク同期なしで、既定および既定以外の手動・順次・有限または無制限の並列キューに対応します。制御デバイスのキューは電源管理の対象外です。専用 READ／WRITE／IOCTL コールバックは既定のコールバックより優先されます。受け付けたキュー要求は同期完了時も `STATUS_PENDING` を返し、void コールバックの戻り値レジスターでは要求は完了しません。遅延完了は既存のスケジューラーを使います。ハンドラーがなければ `STATUS_INVALID_DEVICE_REQUEST` で完了し、ゼロ長 READ／WRITE は配信を有効にしない限りそのまま完了します。既定のファイルパッケージは CREATE／CLEANUP／CLOSE を成功、Information=0 で完了します。既定以外の手動キューには `WdfRequestForwardToIoQueue` で要求を転送し、`WdfIoQueueRetrieveNextRequest` で FIFO 順に取得できます。取得前に取り消された要求はフレームワークがキューから除き、`STATUS_CANCELLED` で完了します。既定以外の自動キューは転送された要求を自身のコールバックに配信し、手動の既定キューは取得まで受信要求を保持します。`WdfIoQueueRetrieveNextRequest` は手動・順次キューで動作し、並列キューでは `STATUS_INVALID_DEVICE_STATE` を返します。対応するコールバックがない自動キューは、配信枠が空いた時点で要求を `STATUS_INVALID_DEVICE_REQUEST` で完了します。ファイルコールバック、PnP デバイス、完全な PnP／電源は未対応です。 `WdfRequestRequeue` は取得済みの要求を同じ手動キューの先頭に戻します。 `NumberOfPresentedRequests` は並列キューで配信中の要求数を制限し、超過分は配信済み要求の完了または取り消しまで待機します。 既定の逐次キューはリクエストを 1 件提示中でも次のリクエストを受け付けます。後続のリクエストは空きができるまで FIFO 順に待機し、配信前に取り消せます。 `WdfIoQueueStop` は要求の受け付けを続けながら配信を停止します。`WdfIoQueueStart` は待機中の要求の配信を再開し、`WdfIoQueueGetState` は待機中と配信済みの件数を返します。停止中の取得は `STATUS_WDF_PAUSED` を返し、停止完了コールバックは指定されたコンテキストを受け取り、配信済みの要求がすべて完了またはキューを離れた後に実行されます。待機中の要求は妨げず、保留中の二重登録は拒否します。

`WdfIoQueueReadyNotify` は手動キューに `EvtIoQueueState` コールバックを 1 つ登録します。待機要求数がゼロから非ゼロになると、ドライバーが以前に取り出した要求をまだ所有していても、`PASSIVE_LEVEL` で `(WDFQUEUE, WDFCONTEXT)` を受け取ります。登録時に既にキューが空でなければ直ちに通知される場合があります。停止中のキューは `WdfIoQueueStart` まで待ちます。重複登録または停止前の登録解除は `STATUS_INVALID_DEVICE_REQUEST` を返します。`WdfIoQueueStop` 後に NULL を渡すと登録解除します。

`WdfIoQueueFindRequest` は手動キューを検索しますが、要求の処理権は移しません。成功時には要求の参照が 1 つ増えるため、ドライバーは `WdfObjectDereference` で解放します。`WdfIoQueueRetrieveFoundRequest` はキューに残る要求の処理権を移し、キャンセルで取り除かれた要求には `STATUS_NOT_FOUND` を返します。省略可能なパラメーターは `WdfRequestGetParameters` と同じ配置です。フレームワークのファイルオブジェクトによる絞り込みは未対応です。

設定された `EvtIoCanceledOnQueue` は、以前ドライバーに渡されてから転送または再キューイングされた要求、あるいは呼び出し元コンテキストのコールバックが明示的にキューへ入れた要求に対してのみ `(WDFQUEUE, WDFREQUEST)` を受け取ります。一度もドライバーに渡されていない要求は、コールバックなしでフレームワークが `STATUS_CANCELLED` で完了します。通知された要求の所有権はドライバーに戻り、ドライバーはコールバック中または後で完了させる必要があり、再キューイングはできません。パージと状態通知は、コールバックの復帰と要求の完了を待ちます。

`WdfIoQueueDrain` は新規要求を `STATUS_INVALID_DEVICE_STATE` で拒否し、待機中の要求を配送します。待機中およびドライバー所有の要求がゼロになると完了コールバックを呼びます。排出中のキューへの転送は `STATUS_WDF_BUSY` を返し、`WdfIoQueueStart` は受け付けを再開します。

`WdfIoQueuePurge` は未配送の要求を `STATUS_CANCELLED` で取り消し、配送済みで取り消し可能な要求には元の IRP 上でキャンセルを要求します。要求のクリーンアップは IRP の解放より先に実行します。状態コールバックは待機中、ドライバー所有およびキャンセルコールバックの終了を待ちます。取り消し可能とマークされていない要求はドライバーが完了します。

`WdfIoQueueStopSynchronously`、`WdfIoQueueDrainSynchronously`、`WdfIoQueuePurgeSynchronously` は、対象の要求が終了するまで `PASSIVE_LEVEL` のゲスト呼び出しを中断します。停止は受け付けを続けて配送を止め、配送済み要求を待ちます。ドレインは新規要求を拒否し、待機中の要求を配送して両方の完了を待ちます。パージは待機中とキャンセル登録済みの要求を取り消し、キャンセルコールバックの復帰まで待ちます。完了させる実行元がなければ停滞したモデルエラーを報告します。

`WdfIoQueueStopAndPurge` と `WdfIoQueueStopAndPurgeSynchronously` は呼び出し前の待機要求とキャンセル登録済みのドライバー所有要求を取り消し、`WdfIoQueueStart` まで配送を止めたまま新規要求を受け付けます。非同期状態コールバックと同期待機は元の要求とキャンセルコールバックの終了後に完了します。呼び出し後の新規要求はキューに残り、完了通知を遅らせません。通常および同期停止は、以前のドレイン・パージ状態から受け付けも再開します。

要求パラメーターは 40 バイトの `WDF_REQUEST_PARAMETERS` レイアウトを使います。入力／出力アクセサーは論理長を返し、バッファーの別名関係と既存の直接 I/O MDL マッピングを維持します。直接 IOCTL の入力は引き続きバッファー方式です。方向の誤りやバッファー不足は文書化されたステータスを返します。完了処理はバッファーが有効な間に要求のクリーンアップを実行し、IRP を完了して要求所有のページロックを解放した後、参照が許す時点で子オブジェクトと要求を破棄します。完了処理開始後は新たなバッファー／パラメーターアクセサー呼び出しを拒否しますが、取得済みのバッファーポインターはクリーンアップ中も使用できます。外部オブジェクト参照が維持するのはコンテキストであり、完了済み IRP へのアクセスではありません。

キャンセル対応は、上記の制御デバイスのキューに渡された要求に限定されます。既にキャンセルされていれば、`WdfRequestMarkCancelableEx` はコールバックを呼ばずに `STATUS_CANCELLED` を返します。`WdfRequestUnmarkCancelable` が成功するとコールバックを解除し、その後のキャンセルは状態の記録のみを行います。`WdfRequestIsCanceled` はキャンセル可能としてマークされていない有効な要求でその状態を読み取ります。キャンセル可能としてのマークに成功した後は、完了にはマーク解除の成功、またはキャンセルコールバックの配信開始が必要です。キューに入っただけでは完了できません。配信開始後は、コールバックが待機中の場合もワーク項目と協調して完了できます。独立した内部参照により、キャンセルコールバックが戻るまで要求を保持します。完了時には先に IRP が無効となり、最後の要求破棄の継続処理自体も待機できます。DPC、FIFO 順のキャンセルコールバック、通常のワーク項目の順に優先し、キャンセルコールバックは実行可能なパッシブ待機フレームの再開よりも先に配信します。

既にキャンセルされた要求に対し、旧版 void `WdfRequestMarkCancelable` は戻る前にゲストのキャンセルコールバックを同期実行します。子の継続処理は待機でき、入れ子のクリーンアップによる完了と最終破棄を行ってから元の API を再開します。登録後のキャンセルは上記のスケジュール経路を使います。これは `PASSIVE_LEVEL` と `WdfSynchronizationScopeNone` における公開ソースの動作を再現しますが、[Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) は自動同期を使わないドライバーに Ex の使用を求めています。旧版の実行は互換動作であり、この設定での使用を推奨するものではありません。

`WdfRequestGetInformation` と `WdfRequestSetInformation` は元の IRP の 64 ビット `IoStatus.Information` を共有し、ゲストの直接書き込みとも一致します。Set は代入のみを行い、転送長は完了時に検証します。`WdfRequestCompleteWithInformation` はクリーンアップ前に同じフィールドへ書き込みます。保存済み IRP を通じたクリーンアップ中の変更は最終 Information に反映され、その時点で GetInformation がゼロを返していても失われません。`WdfRequestGetIoQueue` は元のキューを返します。既定のファイル設定では `WdfRequestGetFileObject` は NULL を返し、WDM FILE_OBJECT から WDF ファイルオブジェクトを捏造しません。`WdfRequestWdmGetIrp` は同じ IRP を返しますが、ゲストの `IoCompleteRequest`／`IofCompleteRequest` による WDF 完了の迂回は拒否します。完了処理中または完了後もハンドルが有効なら、GetInformation／GetIoQueue はゼロを返し、MDL 取得は有効な出力スロットを NULL にしてから `STATUS_INTERNAL_ERROR` を返します。SetInformation／GetFileObject／WdmGetIrp はその時点では拒否します。既存のバッファー／パラメーターアクセサーの制約は変わりません。

`WdfRequestRetrieveInputWdmMdl` と `WdfRequestRetrieveOutputWdmMdl` は、バッファー方式の WRITE 入力、READ 出力、IOCTL 入出力について、既存 SystemBuffer の記述子を必要時に作成します。方向と非ゼロ長を毎回検証した後、要求ごとに一つのキャッシュを使います。最初の取得が ByteCount を決め、逆方向の論理長が異なっても保持します。この記述子の `MmGetSystemAddressForMdlSafe` は元の VA を返し、追加マッピング、解除、ドライバーによる解放は拒否します。直接 READ 出力、WRITE 入力、IOCTL 出力は既存の `IRP.MdlAddress` を返し、取得だけではマッピングしません。直接 IOCTL 入力は SystemBuffer キャッシュを使います。記述子、IRP、バッファーは完了時に無効化します。キャンセル内部参照や外部参照が保持するのは WDF コンテキストだけです。WDF の `METHOD_NEITHER` 用 MDL 取得関数は未対応です。要求所有の WDFMEMORY は別のロック済みページマッピングを使います。構築済み記述子のモデル PFN 配列は読み取り専用で参照でき、未構築記述子の PFN アクセスは拒否します。

バッファー方式、直接方式、neither 方式の KMDF 要求では、`WdfDeviceInitSetIoInCallerContextCallback` が要求元プロセスの `PASSIVE_LEVEL` でキュー前コールバックを実行します。要求を完了するか、`WdfDeviceEnqueueRequest` を一度呼んで既定キューへ渡す必要があります。`METHOD_NEITHER` IOCTL と neither READ／WRITE では、`WdfRequestRetrieveUnsafeUserInputBuffer` と `WdfRequestRetrieveUnsafeUserOutputBuffer` がこのコールバック内だけで元のユーザーアドレスを返します。`WdfRequestProbeAndLockUserBufferForRead` と `WdfRequestProbeAndLockUserBufferForWrite` はページ権限を確認して要求所有のページをロックし、`WdfMemoryGetBuffer` は要求元コンテキストを離れたキューコールバックでも使えるシステム別名を返します。完了時にロックと別名を解放します。元のシナリオバッファーのみを扱い、埋め込みポインターや任意のユーザーマッピングは未対応です。

KMDF PnP の直接 FDO/PDO 構成を限定的にサポートします。`EvtDriverDeviceAdd` はフレームワーク所有の初期化子を受け取り、`WdfDeviceCreate` は FDO を作成します。`WdfFdoInitWdmGetPhysicalDevice` と `WdfDeviceWdmGetPhysicalDevice` は PDO 識別子を返します。AddDevice の失敗と Remove の完了後にはクリーンアップ／破棄コールバックを実行してデバイスを削除します。PnP キューは電源管理に `WdfUseDefault` または `WdfTrue` を使用できます。`WdfFalse` は自動管理を無効にします。`WdfDeviceInitSetPnpPowerEventCallbacks` では `EvtDevicePrepareHardware`、`EvtDeviceD0Entry`、`EvtDeviceD0Exit`、`EvtDeviceReleaseHardware` をサポートします。PrepareHardware は D0Entry より先に別々のリソース一覧を受け取り、リソースのないデバイスでは空になります。空の一覧では `WdfCmResourceListGetCount` は 0、`WdfCmResourceListGetDescriptor` は NULL を返します。PrepareHardware または D0Entry が失敗すると START が失敗し、その場合も ReleaseHardware を呼び出します。D0 からの停止・削除では、D0Exit の後、IRP 完了前に ReleaseHardware を実行します。構成された `register_bank` デバイスは ReleaseHardware まで読み取り専用の `CM_PARTIAL_RESOURCE_DESCRIPTOR` を公開します。ドライバーは変換済みメモリーを `MmMapIoSpace` でマップできますが、STOP または Remove の完了前に解除する必要があります。その他のリソース種別と一般的な電源ポリシーは未対応です。`WdfIoQueuePnpHeld` は D0 に入る前と D0 を離れた後の配送停止を示します。D0 を離れる前に、電源管理キュー内でドライバーが保持する各要求に `EvtIoStop` を呼び出します。ドライバーは要求を完了するか、`WdfRequestStopAcknowledge` を呼び出すか、予定済みの処理による完了を待てます。`EvtIoStop` が未登録でも、フレームワークは配送済み要求の完了を待ちます。TRUE は再起動後の再配送のためキューへ戻し、FALSE はドライバーの所有権を保って D0 再突入後に `EvtIoResume` を呼び出します。キュー内の要求は再起動まで待機し、完了させる実行元が残っていない場合に限り、待機は停滞した `model_error` になります。実 WDK フィクスチャ `driver_kmdf_pnp.c` には `NEVERD_KMDF_PNP_FIXTURE`／`NEVERD_KMDF_PNP_CFG_FIXTURE` を使用します。

モデル化済み KMDF API: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfWdmDeviceGetWdfDeviceHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetDeviceType`, `WdfDeviceInitSetExclusive`, `WdfDeviceInitSetIoType`, `WdfDeviceInitSetIoInCallerContextCallback`, `WdfDeviceInitSetPnpPowerEventCallbacks`, `WdfCmResourceListGetCount`, `WdfCmResourceListGetDescriptor`, `WdfFdoInitWdmGetPhysicalDevice`, `WdfDeviceCreate`, `WdfDeviceEnqueueRequest`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfDeviceWdmGetAttachedDevice`, `WdfDeviceWdmGetPhysicalDevice`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfDeviceGetDriver`, `WdfIoQueueGetDevice`, `WdfIoQueueGetState`, `WdfIoQueueStop`, `WdfIoQueueStopSynchronously`, `WdfIoQueueStart`, `WdfIoQueueDrain`, `WdfIoQueueDrainSynchronously`, `WdfIoQueuePurge`, `WdfIoQueuePurgeSynchronously`, `WdfIoQueueStopAndPurge`, `WdfIoQueueStopAndPurgeSynchronously`, `WdfIoQueueReadyNotify`, `WdfIoQueueRetrieveNextRequest`, `WdfIoQueueFindRequest`, `WdfIoQueueRetrieveFoundRequest`, `WdfRequestForwardToIoQueue`, `WdfRequestRequeue`, `WdfRequestStopAcknowledge`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveUnsafeUserInputBuffer`, `WdfRequestRetrieveUnsafeUserOutputBuffer`, `WdfRequestProbeAndLockUserBufferForRead`, `WdfRequestProbeAndLockUserBufferForWrite`, `WdfMemoryGetBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

PnP FDO では、`WdfDeviceInitSetDeviceType` が指定した 32 ビットの型を WDM `DEVICE_OBJECT` に記録します。指定しない場合は `FILE_DEVICE_UNKNOWN` が既定値です。デバイス型に依存する I/O 優先度のブーストはモデル化していません。

`WdfDeviceInitSetExclusive` は初期化子から作成した WDM デバイスに `DO_EXCLUSIVE` を設定します。名前付き制御デバイスは最初のファイルが閉じるまで 2 件目の独立したオープンを拒否します。PnP FDO のこのフラグだけでは、名前付き PDO やスタック全体は排他的になりません。INF による PDO の排他設定はこのプロファイルの対象外です。

任意の実 WDK 検証では、`driver_kmdf_lifecycle.c` と `driver_kmdf_control.c` を本物の KMDF エントリーライブラリで別々にコンパイルします。`NEVERD_KMDF_FIXTURE`／`NEVERD_KMDF_CFG_FIXTURE` はライフサイクルイメージを、`NEVERD_KMDF_CONTROL_FIXTURE`／`NEVERD_KMDF_CONTROL_CFG_FIXTURE` は通常版／CFG 有効版の制御デバイスイメージを選択します。外部成果物がなければ明示的にスキップします。ネイティブおよび C API／CLI の範囲は[テスト](testing.md)を参照してください。現在の実行証拠は Linux ホストに限定されます。

初期 API モデルの契約は、意図的に有限の範囲に限定しています。

| API | モデル化した動作と制限 |
|-----|------------------------|
| `RtlInitUnicodeString` | 範囲を限定した NUL 終端ソースからゲスト `UNICODE_STRING` を作る |
| `RtlCopyUnicodeString`、`RtlCompareUnicodeString`、`RtlEqualUnicodeString` | 長さを持つ UTF-16 文字列のコピーと、大文字小文字を区別する比較。区別しない比較には Windows の大小文字テーブルが必要なため停止する |
| `ExRaiseStatus`, `ExRaiseAccessViolation`, `ExRaiseDatatypeMisalignment` | 対応する定数 C `__except` ハンドラーへゲスト例外を送る。通常の API return はなく、フィルター／finally と CPU 障害からの復帰は未対応 |
| `ExAllocatePool2` | ページプール／非ページ NX プールの割り当て。デフォルトでゼロ初期化し、未初期化とキャッシュ整列のフラグをモデル化する。無効な必須フラグは NULL を返し、クォータ／実行可能プールおよび割り当て例外の送出では停止する |
| `MmGetSystemRoutineAddress` | 長さを持つゲストの名前を、共通のエクスポート一覧で解決する |
| `MmMapIoSpace`, `MmMapIoSpaceEx`, `MmUnmapIoSpace` | 宣言済み変換後部分区間、非キャッシュ RO／RW、共有別名、正確な unmap。任意物理メモリは未対応 |
| `IoConnectInterrupt`, `IoDisconnectInterrupt`, `IoConnectInterruptEx`, `IoDisconnectInterruptEx` | 正確に割り当てられた排他的 latched ライン。PASSIVE_LEVEL の旧来 ABI と Ex 1／2／4、不透明な接続と正確な世代寿命 |
| `KeSynchronizeExecution`, `KeAcquireInterruptSpinLock`, `KeReleaseInterruptSpinLock` | 実際の BOOLEAN 同期コールバックと、割り当て DIRQL の同じ非再帰ロック。元の呼び出し元 IRQL と所有権を復元 |
| `IoGetDmaAdapter` | PASSIVE_LEVEL の明示的 Internal バスマスター記述 0／1 と束縛 V1 操作表。新版照会は NULL |
| `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `PutDmaAdapter` | 同じコヒーレント RAM、厳密な割り当て識別子、独立したアダプター寿命を使う表メソッド |
| `GetScatterGatherList`, `PutScatterGatherList` | DISPATCH_LEVEL の表メソッド。実際のインライン／資源待ちコールバック、固定 MDL ビュー、明示的解放 |
| `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters` | 変換付きバスマスターチャネルのコールバック、共有レジスター上限、連続 MDL 断片、全体フラッシュ、正確な保持レジスター解放 |
| `KeFlushIoBuffers` | 有効なロック済み／非ページ MDL のコヒーレント CPU キャッシュフラッシュ。DMA マッピングは解放しない |
| `MmMapLockedPagesSpecifyCache`、`MmGetSystemAddressForMdlSafe`、`MmUnmapLockedPages` | リクエスト所有 MDL のキャッシュ付き KernelMode マッピングと権限。非ページプール MDL は安全ヘルパーで元のプールマッピングを再利用 |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | 独立した MDL。全範囲が有効な単一の非ページプール割り当て内にあること。MDL とバッファーの寿命は独立。IRP 関連付け、チェーン、クォータは未対応 |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | 明示的なセッションレジストリ、ハンドルごとの権限と寿命、クエリバッファーサイズと変更。ホストレジストリは使用しない |
| `ExAllocatePoolWithTag`、`ExFreePoolWithTag`、`ExFreePool` | プール種別 `0`、`1`、`512` のデータ割り当て。サイズ／タグは正で、タグ付き解放は割り当てと一致する必要があり、アドレスは再利用しない |
| `IoCreateDevice`、`IoDeleteDevice` | デバイス種別 `0x22`、characteristics は `0` または `0x100`、拡張領域のサイズは有限、名前は ASCII の `\Device\Name` |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | 同一ドライバー内の接続。以前の最上位を返し、切断は保存した下位を受け取る。上記の構造・寿命制限に従う |
| `IofCallDriver`, `IoCallDriver` | 保持経路の指定対象へ直接ディスパッチ。ゲストカーソルを検証し下位 NTSTATUS を保持 |
| `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx` | 拡張部の正確な所有者と不透明32／120バイト。NULL／重複 Tag |
| `IoReleaseRemoveLockEx`, `IoReleaseRemoveLockAndWaitEx` | 一致する Tag の解放とプロバイダー受信後の再開可能な REMOVE 待機 |
| `PoCallDriver`, `PoStartNextPowerIrp` | 管理対象電源 IRP の転送と追加ハンドシェイクなしの Vista+ 検証 |
| `PoSetPowerState`, `PoRequestPowerIrp` | デバイス別通知状態と明示 PDO FIFO に基づく実際の子要求。範囲は上記 |
| `IoCreateSymbolicLink`、`IoDeleteSymbolicLink` | 1 セッションの名前空間内の ASCII `\DosDevices\Name` または `\??\Name`。リンク先は `\Device\Name` |
| `DbgPrint`、`DbgPrintEx` | 検査付き Win64 可変引数の書式処理。出力は最大 512 バイトで、デバッガーフィルターはすべて有効 |
| `IoGetCurrentIrpStackLocation` | 現在モデル化している IRP のスタック位置を返す。通常コンパイルされた WDM マクロも同じゲストフィールドを読む |
| `KeGetCurrentIrql` | 明示的な引き上げと復元を含む現在の IRQL/CR8 を読みます。ディスパッチとワーク項目は `PASSIVE_LEVEL`、DPC は `DISPATCH_LEVEL` で開始します |
| `KfRaiseIrql`, `KeLowerIrql` | 実際の x64 WDK 昇降 IRQL インポートです。保存した IRQL は同じ実行上で LIFO 順に、復帰や中断の前に復元します。CR8 読み取りには各変更が反映されます。命令単位の割り込み先取りは再現しません。 |
| `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`, `KeReleaseSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `KeReleaseSpinLockFromDpcLevel`, `KeTryToAcquireSpinLockAtDpcLevel` | CPU0 上の常駐・整列済み実行スピンロック。所有者、取得と解放の対応、IRQL の復元を検査します。競合するブロッキング取得は明示的に停止します。 |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | デバイスが所有する不透明なワーク項目。`DelayedWorkQueue` のみ。`PASSIVE_LEVEL` でデバイスとコンテキストをコールバックに渡す。キュー内の項目は解放不可 |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | 不透明な DPC、四つのゲスト引数、`DISPATCH_LEVEL`、重複登録／削除と重要度。対象は CPU0 のみ |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | 通知／同期タイマー、相対／絶対の 100 ns 期限、ミリ秒周期、再設定／キャンセルと仮想時間のシグナル照会 |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | 通知／同期イベントの異なるシグナル消費。`KeSetEvent` は Increment=0、Wait=FALSE のみ |
| `KeInitializeSemaphore`, `KeReleaseSemaphore`, `KeReadStateSemaphore` | 正の上限を持つ常駐カウントセマフォです。成功した待機ごとに 1 減算します。解放は Increment=0 と Wait=FALSE に限定し、上限超過は `STATUS_SEMAPHORE_LIMIT_EXCEEDED` を発生させます。 |
| `KeInitializeMutex`, `KeReleaseMutex`, `KeReadStateMutex` | 常駐 KMUTEX は実行フレームが所有し、再帰取得できます。KeReleaseMutex は直前の符号付きシグナル状態を返し、所有者と一致する DISPATCH_LEVEL 取得状態を要求し、Wait=FALSE のみ受け付けます。保持中の復帰、再初期化、領域解放は禁止します。 所有者以外の解放は `STATUS_MUTANT_NOT_OWNED` を発生させます。 |
| `PsCreateSystemThread`, `PsTerminateSystemThread`, `ObReferenceObjectByHandle`, `ObfDereferenceObject`, `ZwClose` | PASSIVE_LEVEL で動く限定的なシステムプロセススレッド。ハンドルと不透明なスレッドオブジェクトの参照は別々の寿命を持つ。PsTerminateSystemThread は復帰せずに終了し、待機対象をシグナル状態にする。APC、優先度、型付きオブジェクト参照は未対応。 |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeEnterGuardedRegion`, `KeLeaveGuardedRegion`, `KeAreApcsDisabled`, `KeAreAllApcsDisabled` | スレッドごとの入れ子の APC 無効化状態。クリティカル領域と保持中の KMUTEX は通常の APC を、ガード領域と IRQL >= APC_LEVEL は全 APC を無効化する。システムスレッドは一つのクリティカル領域内で開始する。不一致の終了や領域を残した復帰は失敗する。APC 配信は未対応。 |
| `KeWaitForSingleObject` | 初期化済みイベント、タイマー、セマフォまたはミューテックス一個。非アラート `KernelMode`、理由 `Executive`。ゼロのポーリング、有限の相対／絶対または無限待機。非ゼロ／無限待機は IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | IRQL <= APC_LEVEL で非アラート `KernelMode` の相対／絶対遅延。仮想時間が進むと保存したゲストフレームを再開 |
| `IoMarkIrpPending` | 現在の生存する IRP を保留にする。WDM マクロによるスタック制御フィールドへの等価な書き込みにも対応。ディスパッチは `STATUS_PENDING` を返す必要がある |
| `IofCompleteRequest`、`IoCompleteRequest` | `IO_NO_INCREMENT` で完了を展開。停止／再開をサポートし、最終展開時にだけ IRP／MDL／バッファーを解放 |
| `memcpy`、`memmove`、`memset`、`memcmp`、`RtlCopyMemory`、`RtlMoveMemory`、`RtlFillMemory`、`RtlZeroMemory`、`RtlCompareMemory` | ゲストバッファー操作は 1 呼び出しあたり最大 1 MiB。重複不可のコピー API は重複範囲を拒否する |

API の IRQL 上限は `KernelAPIIRQL.def` にあり、引数依存の制約は担当モデルが検査します。DPC からレジストリ API やページプールの割り当て・解放・アクセスはできません。Unicode `DbgPrint` 変換は `PASSIVE_LEVEL` を要求し、対応する ANSI 出力と非ページ操作は `DISPATCH_LEVEL` で使用できます。コールバックスタックには範囲があり、逸脱したスタックポインターは別の待機ワーカーのスタックへ侵入できません。デバイス拡張内の有効なタイマーは早期解放を防ぎます。

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

ルートで受け付けるフィールドは `load_address`、`requests`、`unload`、`kernel_exports`、`registry`、`pnp_devices` だけです。通常のファイル要求は `kind`、任意の `device` または `device_id`（併用不可）、任意の `file` を受け付けます。IOCTL は `code` が必須で、`input`、`output_size`、`direct_input` を受け付けます。`read` は `output_size` と `byte_offset`、`write` は `input` と `byte_offset` を受け付けます。オフセットのデフォルトはゼロで、整数または 16 進文字列を使えますが、非負の符号付き 64 ビット値に収まる必要があります。ライフサイクルのリクエストは転送フィールドを拒否します。未知のフィールドや重複フィールドは拒否します。`code` は符号なし 32 ビット JSON 整数または `0x` で始まる 16 進文字列を受け付けます。`input` は、プレフィックスや空白を含まない、長さが偶数の 16 進バイト文字列です。省略すると空の入力になります。`output_size` は符号なし JSON 整数で、省略時はゼロです。小数や浮動小数点形式の表記は拒否します。

READ／WRITE／IOCTL 要求だけが省略可能な `cancel_after_100ns` を受け付けます。値は 0 から `INT64_MAX`（9223372036854775807）までの JSON 整数です。要求の提出時点を基準に、実時間ではなく仮想時間の 100 ns 単位でキャンセルを予約します。KMDF ではゼロをフレームワークのルーティング後、ゲスト I/O コールバックの前に適用します。ルーティングが既に要求を完了していれば、完了が優先されます。WDM ではディスパッチの復帰後にゼロを適用します。正の遅延では、実行可能なコールバックやフレームがないときだけ、タイマー、待機、キャンセルの期限へ時間を進めます。保留中の WDM IRP は、登録されたキャンセルルーチンをキャンセルスピンロック保持下の `DISPATCH_LEVEL` で呼び出します。完了前に `Irp->CancelIrql` を使ってロックを解放する必要があります。一般のキューや PnP のキャンセルは未対応です。各要求レポートの `cancel_requested_at_100ns` は、キャンセルが実際に発生した絶対仮想時刻、または発生しなかった場合の null です。先に完了した場合も null のままです。キャンセル要求だけでは IRP は完了せず、最終ステータスも決まりません。
`IoSetCancelRoutine`、`IoAcquireCancelSpinLock`、`IoReleaseCancelSpinLock`、`IoCancelIrp` は同じ IRP 状態とキャンセルスピンロックを使います。`IoCancelIrp` は登録済みルーチンを同期的に呼び出し、呼び出したかどうかを返します。

省略可能な真偽値 `user_unmap_after_dispatch` は、空でない WDM neither 転送について、ディスパッチ復帰後かつ予定されたワークやキャンセルの前に元のユーザーアドレスのアクセス権を取り消します。MDL でロック済みのページとシステムエイリアスは解除まで使えますが、生のユーザーポインターと新たなロックは失敗します。出力アドレスを取り消した場合、`output_hex` は空です。アドレス再利用と任意時刻の解除はモデル化しません。

`requestor_process_id` は合成要求元プロセス ID です（既定値 4096、範囲 5～`UINT32_MAX`）。`IoGetRequestorProcessId` は有効な IRP の ID を返し、`PsGetCurrentProcessId` は直接ディスパッチではその ID、モデル化されたシステムワーカーでは 4 を返します。プロセスを切り替えると別プロセスの元のユーザー VA は使えなくなりますが、ロック済み MDL のシステム別名は有効です。`requestor_exit_after_dispatch` はディスパッチ復帰後にそのプロセスの元のユーザー VA をすべて取り消して新しい I/O を拒否します。明示的な CLEANUP/CLOSE は可能で、キャンセルやハンドル解放は自動推定しません。

システムワーカーは `IoGetRequestorProcess` で有効な IRP の不透明な要求元プロセスを取得し、書き込み可能なカーネル `KAPC_STATE` を使って `KeStackAttachProcess` で一時的にアタッチできます。元のユーザー VA にアクセスした後、同じ状態を `KeUnstackDetachProcess` に渡して戻します。`IoGetCurrentProcess` と `PsGetProcessId` はアタッチ先を示しますが、`PsGetCurrentProcessId` はワーカーを作成したシステムプロセスの ID 4 のままです。終了済みプロセス、完了済み IRP、不一致の状態、およびアタッチ中の待機や IRP 完了は明示的に失敗します。

ダイレクト IOCTL では、`input` が最初のシステムバッファーを初期化し、`direct_input` が MDL で記述される別の第 2 バッファーを初期化して、`output_size` までゼロで埋めます。`METHOD_IN_DIRECT` は読み取りアクセスを要求しますが、システムマッピングが読み取り専用であることを意味しません。両方式とも読み書き可能なシナリオバッファーを使います。`MdlMappingNoWrite` はマッピングの書き込み権限を、`MdlMappingNoExecute` は実行権限を除去します。アンマップはシステム VA を無効にしますが、再マップしても同じロック済みデータを保持します。完了時に MDL とマッピングの寿命が終了します。WDM マクロが使う公開 MDL フィールドはモデル化しますが、プロセスフィールド、未構築記述子の PFN、手作りの MDL、ユーザーマッピング、生の UserBuffer を介した直接アクセスは拒否します。構築済み PFN 配列は読み取り専用です。長さがゼロのダイレクトバッファーは null MDL を持ちます。

`IoAllocateMdl` は、空でなく、アドレスがオーバーフローせず、1 MiB 以下のバッファーに対して独立したメタデータを割り当てます。バッファーのプローブやロックは行いません。`Irp` は NULL、`SecondaryBuffer` と `ChargeQuota` は FALSE が必要です。アリーナが枯渇すると NULL を返します。`MmBuildMdlForNonPagedPool` では、記述範囲全体が有効な単一の非ページプール割り当て内にある必要があります。安全ヘルパーと通常の WDM マクロは元のアドレスを再利用し、別名参照と既存の権限を維持します。書き込み／実行禁止フラグを追加しても既存の権限は変わりません。追加のシステムマッピングとアンマップは拒否します。`IoFreeMdl` は MDL だけを無効にし、プールバッファーの寿命は独立しています。解放済み領域を再使用しなければ、どちらの解放順序も利用できます。モデルの MDL フィールドと構築済み PFN 配列は読み取り専用で、プロセスフィールド、未構築 PFN、MDL チェーン、手動のフィールド変更は未対応です。アンロード時にはドライバー所有の MDL をすべて解放する必要があります。

IOCTL の `output_size` が非ゼロなら、入力バッファーが大きくても `Information` はそのサイズを超えてはいけません。出力バッファーがない IOCTL はドライバー定義の結果を返すことができ、出力バイトはコピーしません。`information_hex` は元の 64 ビット値を正確に保持します。

READ/WRITE では、`DO_BUFFERED_IO` または `DO_DIRECT_IO` が buffered/direct 転送を選びます。どちらも設定されていない場合、元のユーザーアドレスは `IRP.UserBuffer` のみに入り、WRITE は入力、READ は出力を示します。SystemBuffer や MDL は暗黙に作成されません。ドライバーは呼び出し元のコンテキストで検査してアクセスするか、処理を延期する前にページをロックします。`user_input_access` は neither WRITE、`user_output_access` は neither READ に適用されます。buffered/direct READ/WRITE で権限を指定した場合と両方のフラグを設定した場合は停止します。Information は転送長に対して検査します。書き込みはバイト数を、読み取りはバイト列を返します。

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

`configuration` オブジェクトには、実行の上限、サービス名、`kernel_exports` の上書き設定を記録します。プロファイルは `wdm-x64-scheduled-v58` です。`nt_status` は引き続き DriverEntry の結果を示し、`scenario_success` は初期化と完了済みリクエストを合わせた結果を示します。`phase`、`requests`、`unload_completed` は、要求されたライフサイクルのどの部分が実行されたかを示します。API 呼び出しと CPU 書き込みにも、そのフェーズ（`driver_entry`、`add_device:<ID>`、`request:N`, `callback:N`、`unload`）を記録します。各リクエストはディスパッチと I/O のステータス、完了の有無、information 長、返された `output_hex` バイト列を報告します。`preferred_image_base` は元の PE ベースアドレスを示します。`security_cookie` は初期化した Cookie のゲストアドレスで、不要だった場合は `"0x0"` です。リクエストのレポートフィールドは `kind`、`device`、`device_id`、`pnp`、`file`, `requestor_process_id`、`byte_offset`、`code`、`irp`、`completed`、`cancel_requested_at_100ns`、`dispatch_status`、`io_status`、`information`、`information_hex`、`output_hex` です。 `configuration.registry` は元のレジストリ設定を保持します。 `information_hex` は元の 64 ビット `IoStatus.Information` を 16 進文字列で正確に保持します。従来の数値フィールド `information` も保持します。

ワーク項目の観測フェーズは `callback:N` です。保留リクエストの `dispatch_status` は `STATUS_PENDING` を保持し、最終完了状態は別の `io_status` に記録され、`scenario_success` の判定に使われます。

`ExRaiseStatus` は NTSTATUS の下位 32 ビットをゲスト例外ハンドラーへ渡します。`ExRaiseAccessViolation` と `ExRaiseDatatypeMisalignment` はそれぞれ `STATUS_ACCESS_VIOLATION` と `STATUS_DATATYPE_MISALIGNMENT` を発生させます。プロファイルは各 Microsoft DDI ページに従い、ExRaiseStatus は `APC_LEVEL` を許可し、引数のない二つのルーチンは `PASSIVE_LEVEL` を要求します。一部の WDK SAL 注釈はラッパーに APC_LEVEL を許可しますが、このプロファイルは文書の厳しい方の上限を採用します。例外を送出する呼び出しは `result: null` を保ち、コードを `detail` に記録します。API が成功して return したとは報告しません。

例外配信はイメージからデコードした x64 バージョン 1 展開テーブルと、`__C_specific_handler` の定数 `EXCEPTION_EXECUTE_HANDLER` スコープを使います。実際のゲストハンドラー本体を実行し、通常のヘルパーフレームを展開して、保存された不揮発汎用レジスターを復元し、現在の実行のスタック境界を保持します。`GetExceptionCode()` は送出されたコードを取得します。ハンドラーは対応する外側スコープへ別の例外を送出できます。途中でフィルター関数、`__finally`、GS／C++ パーソナリティー、連鎖または不完全なメタデータ、プロローグの展開、XMM 復元操作に遭遇した場合は明示的に失敗します。捕捉されない API 例外は `model_error` で停止し、CPU のメモリ／割り込み／無効命令フォールトは引き続き実行を終了します。

独自の `driver_wdm_seh.c` フィクスチャーは真正 WDK ヘッダーと `/GS-` を使います。通常イメージと有効 CFG イメージには `NEVERD_WDM_SEH_FIXTURE` と `NEVERD_WDM_SEH_CFG_FIXTURE` を設定します。[driver-seh-scenario.json](../examples/driver-seh-scenario.json) の例はイメージを再配置し、DriverEntry 内で API 例外を捕捉してアンロードします。 別個の WDM METHOD_NEITHER 経路では、ユーザープローブ、MDL ロック、捕捉可能なメモリ障害に対応します。

null を取り得る `fault` オブジェクトは、最初のバックエンドフォールトを保持します。`kind`、`pc`、null を取り得る `address`、`size`、`access`、`interrupt` により、未マップまたは保護されたメモリ、無効な範囲、無効な命令、CPU 例外を区別します。アドレスは 16 進文字列、サイズと割り込みベクターは整数で表します。観測用の読み取りが元のフォールトを置き換えることはありません。フォールトが発生したバックエンドは再開できず、この記録によってバックエンドフォールトをゲスト SEH で処理できるわけではありません。

`instructions` は、実行ポリシーが許可したゲスト命令の試行回数を数えます。ポリシーが拒否した命令は数えません。許可後に CPU フォールトが発生した命令は数えます。合成した API ディスパッチと戻り先の番兵は、このカウンターを増やしません。

各 `writes` 項目の `semantics: "attempted_guest_write"` は、スタック外への CPU 書き込みの試行を記録したことを示します。その後にフォールトが発生したり、予算で停止したりする試行も含みます。書き込みの完了は保証せず、API モデルによる書き込みも含みません。デバイスとドライバーオブジェクトのスナップショットは、実行停止時に観測した状態を示します。

`neverd/sdk/NeverDCAPIEmulation.h`（または C API の統合ヘッダー）を include し、セッションを作成して `neverd_emulate_driver_json(session, path, options)` を呼び出します。空でないパスを明示すると、汎用解析 API による事前ロードを経ずに、厳格な実行前検査へ直接入ります。CLI もこの経路を使います。options に `NULL` を渡すとデフォルト値を選択します。`neverd_driver_options_v1` を明示する場合は `struct_size` が厳密に一致し、命令、メモリ、イベント、タイムアウトの各予算が正でなければなりません。結果は `neverd_free_string` で解放してください。

パスに `NULL` を渡す場合はロード済みセッションが必要で、IR 解析や関数を制限したロードとは独立して、そのファイルを再解析します。どちらの経路もセッションのイメージを変更しません。呼び出し中は入力ファイルを利用可能かつ変更されない状態に保ってください。リクエスト／環境構築の失敗は `NULL` を返して `neverd_last_error` を設定し、実行の停止は JSON を返します。機能が無効なビルドでも API は存在し、有効化の方法を報告します。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` は、同じ v1 オプションと所有権規則を使い、厳密に検証するシナリオ入力を追加します。NULL ではない NUL 終端 JSON 文字列が必要です。従来の `neverd_emulate_driver_json` ABI は変更せず、初期化のみを実行します。C++ パーサー `driverOptionsFromScenarioJSON` は、`emulateDriver` の呼び出し側にも同じシナリオ検証を提供します。

内部 C++ エントリーポイントは `include/neverd/emulation/DriverSession.h` の `neverd::emulation::emulateDriver` です。形式の解析は既存ローダー、Windows のオブジェクト／API 動作は `lib/emulation/windows`、CPU の状態と実行は Unicorn アダプターが担当します。アダプターとモデルは同じゲストメモリインターフェースを使います。Windows API の動作を Unicorn fork に実装すべきではありません。

WDM `METHOD_NEITHER` では `Type3InputBuffer` と `IRP.UserBuffer` は別々のユーザー割り当てを指します。`ProbeForRead` はページに触れず範囲とアラインメントを確認し、`ProbeForWrite` は各ページに触れます。`ExGetPreviousMode` は要求モードを返します。`MmProbeAndLockPages` は一つのユーザー割り当てをロックし、`MmGetSystemAddressForMdlSafe` は共有エイリアスを返し、`MmUnlockPages` はエイリアスとロックを解除します。任意のプロセスは未対応です。

空でない WDM `METHOD_NEITHER` IOCTL 要求では、`user_input_access` と `user_output_access` を個別に `read_write`（既定）、`read_only`、`no_access` に設定できます。buffered/direct 方式や空バッファーでは拒否されます。`no_access` はポインターを残したままページアクセスを禁止します。
報告の `configuration.user_page_access` は明示された設定のみを、0 始まりの `source_request_index` とともに記録します。省略した方向は `read_write` です。

## 限定的な WDM 要求の並行実行

WDM の READ/WRITE/IOCTL 要求または無制限の並列 KMDF 既定キューの要求には `defer_callback_drain: true` を指定できます。ディスパッチが `STATUS_PENDING` を返し、IRP が実際に保留中の場合だけ、コールバックを実行する前に次の要求を投入します。フラグのない次の要求の後でコールバックを処理し、バッチを完了します。最後の要求にフラグがある場合はシナリオ末尾で処理します。重複する要求には別々のファイルオブジェクト、または明示的に非同期で開いた同一ファイルオブジェクトを使えます。同期ファイル上の重複、任意のプリエンプションや外部要求の到着には対応しません。

## 非同期ファイルオブジェクト

CREATE 要求だけが真偽値 `asynchronous_file: true` を指定できます。省略または false は同期オープンです。非同期オープンではゲスト `FILE_OBJECT` の `FO_SYNCHRONOUS_IO` をクリアし、後続のファイル IRP に `IRP_SYNCHRONOUS_API` を設定しません。同一非同期ファイルの READ/WRITE/IOCTL はコールバック処理を明示的に遅延した場合のみ重複できます。CLEANUP/CLOSE は先行する全転送の完了と最終化を待ちます。暗黙のファイル位置は保持せず、`byte_offset` は要求ごとに指定され、省略時はゼロです。CREATE 以外では false も拒否します。
