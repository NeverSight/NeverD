**Languages**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← ドキュメント索引](README.md)

# Windows ドライバーエミュレーション

NeverD のオプションのドライバーエミュレーターは、対応する x64 WDM ドライバーの PE エントリーポイントを実行します。必要に応じて、明示した同期リクエストシナリオを実行してからアンロードできます。CPU の実行には Unicorn を、Windows 環境には NeverD 独自の範囲を限定したモデルを使用します。ドライバーをホストカーネルにロードしたり、ゲスト API 呼び出しをホスト OS のサービスに転送したりすることはありません。

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

互換性は `.sys` 拡張子ではなく、実行されたコード経路とその依存関係で決まります。現在の受け入れ検証の根拠は、独自に作成したフリースタンディングの fixture と、Microsoft の SIOCTL WDM サンプルのバッファード I/O 経路です。任意のサードパーティードライバーとの互換性を示すものではありません。

| ドライバーの種類または要件 | 現在の対象範囲 | 不足する環境 |
|----------------------------|----------------|--------------|
| 下記 API を使用する x64 ソフトウェア WDM ドライバー | 範囲を限定した初期化と、1 つの同期ファイルライフサイクル | 追加で実行される API ごとに明確なモデルが必要 |
| `METHOD_BUFFERED` IOCTL | 明示したリクエストシナリオで対応 | 複数ファイルの同時オープンと非同期完了は未対応 |
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT`、`METHOD_NEITHER` | 拒否 | MDL、ロックされたページ、アクセスのプローブ、ユーザーバッファーの寿命 |
| KMDF / UMDF ドライバー | 未対応 | フレームワークのバインド、オブジェクト、キュー、コールバック、適切なホストランタイム |
| PnP バス／ファンクション／フィルタードライバー | API サブセット内で初期化できる場合がある。デバイススタックのライフサイクルは未対応 | デバイスのアタッチ、下位ドライバーへのディスパッチ、PnP および電源 IRP |
| ストレージ、ネットワーク、ディスプレイ、ファイルシステム、ミニフィルタードライバー | 各サブシステムの契約に未対応 | ポート／クラス／ミニポートのフレームワーク、NDIS/WFP、グラフィックスまたはファイルシステムのサービス |
| ワーカースレッド、タイマー、DPC、APC、待機、キャンセルを使うドライバー | 未対応 | スケジューリング、IRQL の遷移、同期、非同期の所有権 |
| プロセス／スレッドのコールバック、ハンドル、レジストリ／ファイル操作、カーネルモジュールの検出を使うドライバー | 下記 API の範囲外は未対応 | オブジェクトマネージャー、システム状態、コールバック／イベントの発生元 |
| ハードウェア、DMA、PCI、割り込み、仮想化を扱うドライバー | 必要な環境に未対応 | デバイスモデル、物理メモリ、バス、割り込み、特権 CPU 状態 |
| x86 または ARM64 Windows ドライバー | 拒否 | アーキテクチャ固有のロード、ABI、実行モデル |
| CFG、未対応のロード構成、TLS、その他の拒否対象 PE 機能を必要とする x64 イメージ | ロード時に拒否 | 各要件に対する明示的なローダー／ランタイムのセマンティクス |

使用されない未対応インポートはバインドしたままにできます。未対応の操作に到達すると停止し、診断とそれまでに収集した観測結果を残します。DriverEntry の成功だけでは、その後のディスパッチ、ハードウェア、フレームワークの経路に対応しているとはいえません。対応するサブセットの正式な定義は、以下の API 表です。

## 実行契約

このプロファイルは、`PASSIVE_LEVEL` で動作する単一スレッドの x64 WDM ライフサイクルをモデル化します。実行は PE エントリーポイントから始まり、コンパイラーが生成したエントリーラッパーがあれば、それも実行します。初期化を完了するには DriverEntry が `STATUS_SUCCESS` を返す必要があります。ゼロ以外の成功ステータスや保留ステータスは未対応の初期化契約として停止します。失敗ステータスは、完了済みの初期化結果として保持します。オブジェクト、文字列、スタック、関数ポインター、割り当てはすべてゲストメモリ内にあります。モデルは、設定されたサービス名（デフォルトは `NeverDDriver`）に対応する `DRIVER_OBJECT` とレジストリパスを提供します。

アダプターは Unicorn の仮想 TLB モードを使い、Windows のページテーブルを合成せずにゲスト仮想アドレスを保持します。これには正規形の高位カーネルアドレスも含まれます。RFLAGS の初期値は `0x202` です。ソフトウェアデバイスのプロファイルは固定の 64 バイトキャッシュラインを使用します。これらは、この実行シナリオで明示的に定めた属性です。

未知のインポートは遅延トラップにバインドされます。未使用なら実行を妨げませんが、その thunk を実行するか、モデル化していないエクスポートデータ値を読み取ると `unsupported_api` で停止します。未対応の CPU 環境への作用も明示的に停止します。NeverD は未実装の呼び出しを成功値で置き換えません。不正なイメージや未対応のロード要件は、実行前に失敗します。

このプロファイルは、完全な Windows カーネル、KMDF ランタイム、PnP／電源ライフサイクル、非同期または保留中の IRP、direct／neither 方式の IOCTL、割り込み、マルチスレッドのスケジューリングを実装しません。コールバックはシナリオが明示的に要求したときだけ実行します。初期化のみの実行は、従来どおり DriverEntry の後で停止します。

シナリオで有効な再配置先アドレスを選ばない限り、イメージは優先ベースアドレスを使用します。イメージは native サブシステムを持つ PE32+ x64 実行ファイルでなければなりません。インポート元には `ntoskrnl.exe` または `ntkrnlmp.exe` を使用できます。

実行ローダーは、検証済みの x64 `DIR64` ベース再配置と、限定されたセキュリティ Cookie のロード構成に対応します。エントリーラッパーの実行前に、決定的なゲスト Cookie を初期化します。CFG、その他の未モデル化ロード構成フィールド、TLS、遅延／バインド済みインポート、序数によるインポート、マネージドイメージは拒否します。イメージは厳格な範囲とアラインメントの検査にも合格する必要があります。

初期 API モデルの契約は、意図的に有限の範囲に限定しています。

| API | モデル化した動作と制限 |
|-----|------------------------|
| `RtlInitUnicodeString` | 範囲を限定した NUL 終端ソースからゲスト `UNICODE_STRING` を作る |
| `ExAllocatePoolWithTag`、`ExFreePoolWithTag`、`ExFreePool` | プール種別 `0`、`1`、`512` のデータ割り当て。サイズ／タグは正で、タグ付き解放は割り当てと一致する必要があり、アドレスは再利用しない |
| `IoCreateDevice`、`IoDeleteDevice` | デバイス種別 `0x22`、characteristics は `0` または `0x100`、拡張領域のサイズは有限、名前は ASCII の `\Device\Name` |
| `IoCreateSymbolicLink`、`IoDeleteSymbolicLink` | 1 セッションの名前空間内の ASCII `\DosDevices\Name` または `\??\Name`。リンク先は `\Device\Name` |
| `DbgPrint`、`DbgPrintEx` | ASCII リテラル文字列と `%%`、出力は最大 512 バイト。可変引数の書式指定で停止し、デバッガーフィルターはすべて有効 |
| `IoGetCurrentIrpStackLocation` | 現在モデル化している IRP のスタック位置を返す。通常コンパイルされた WDM マクロも同じゲストフィールドを読む |
| `KeGetCurrentIrql` | `PASSIVE_LEVEL` を返す |
| `IofCompleteRequest`、`IoCompleteRequest` | 現在の同期モデル IRP を `IO_NO_INCREMENT` で完了させる。完了した IRP やバッファーには再アクセスできない |
| `memcpy`、`memmove`、`memset`、`memcmp`、`RtlCopyMemory`、`RtlMoveMemory`、`RtlFillMemory`、`RtlZeroMemory`、`RtlCompareMemory` | ゲストバッファー操作は 1 呼び出しあたり最大 1 MiB。重複不可のコピー API は重複範囲を拒否する |

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

デバイス名と IOCTL コードはドライバーと一致する必要があります。`device` を省略すると、唯一の生存デバイスを選択します。一意に選べない場合は失敗します。このモデルは 1 つのオープンファイルを追跡し、create、IOCTL、cleanup、close の順序を要求します。対応する IOCTL は `METHOD_BUFFERED` のみです。ディスパッチは各 IRP を同期的に完了しなければなりません。`STATUS_PENDING` の返却、未完了、無効な出力長、完了済み IRP へのアクセスは明示的に失敗します。要求されたアンロードの後には、生存するデバイス、シンボリックリンク、プール割り当て、ファイルオブジェクトを残せません。

省略可能なルートフィールド `"load_address": "0x190000000"` はベースアドレスの変更を要求します。省略または `"0x0"` なら優先アドレスを使用します。イメージは再配置の要件を満たす必要があります。元の初期化コマンドや C API が暗黙にシナリオを実行することはありません。

ルートで受け付けるフィールドは `load_address`、`requests`、`unload` だけです。リクエストのフィールドは `kind`、任意の `device`、そして `ioctl` の場合に限り必須の `code` と任意の `input`、`output_size` です。未知のフィールドや重複フィールドは拒否します。`code` は符号なし 32 ビット JSON 整数または `0x` で始まる 16 進文字列を受け付けます。`input` は、プレフィックスや空白を含まない、長さが偶数の 16 進バイト文字列です。省略すると空の入力になります。`output_size` は符号なし JSON 整数で、省略時はゼロです。小数や浮動小数点形式の表記は拒否します。

シナリオテキストは最大 2 MiB、リクエストは最大 64 件、各入力／出力バッファーは最大 65536 バイト、入力と出力の合計は最大 512 KiB です。命令、観測、ゲストメモリ、時間の予算は、シナリオ全体に適用します。1 MiB の領域にはオブジェクトやメタデータも配置するため、シナリオのバッファー上限に達する前にモデルのメモリを使い切る場合があります。

## Microsoft サンプルの受け入れ検証

任意に実行する[検証スクリプト](../../scripts/validate_windows_driver_sample.py)は、[検証マニフェスト](../../unittests/emulation/fixtures/sioctl-validation.json)で固定したリビジョンの Microsoft SIOCTL ソースをダウンロードし、SHA-256 ハッシュを検証してから、未変更のソースを MinGW-w64 DDK ヘッダーでコンパイルします。指定した出力ディレクトリに、上流のライセンス／出所、ビルドコマンド、シナリオ、レポートを保存します。ネットワークアクセス、Clang、`lld-link`、`nm`、MinGW-w64 の DDK ヘッダーが必要です。

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

MinGW-w64 の include ディレクトリがデフォルトと異なる場合は `--headers` を使います。スクリプトはコンパイル済みオブジェクトの依存関係から MS COFF インポートライブラリを生成します。検証では DriverEntry、create、サンプルのバッファード IOCTL、cleanup、close、unload を実行します。上流サンプルは cleanup ハンドラーを登録していないため、モデルのデフォルトハンドラーが cleanup を `STATUS_INVALID_DEVICE_REQUEST`（`0xC0000010`）で完了させます。それでもドライバーは close と unload を実行し、成功した IOCTL は期待したバイト列を返します。この完全なシナリオでは、CLI の期待終了コードは **2**、`scenario_success` は false です。スクリプト自体が成功するのは、可視化された cleanup の失敗も含め、これらの結果がすべて一致した場合だけです。結果を隠すためにサンプルを書き換えることはありません。

## レポートと SDK

JSON レポートは `stop_reason`、null を取り得る `nt_status` と `nt_success`、停止時の PC、命令数を区別します。デバイスオブジェクトやドライバーのコールバックアドレスなど、停止前に収集した API 呼び出しと観測可能な状態を保持します。ゲストアドレスは 16 進文字列として表現するため、JSON の利用側で 64 ビットの精度が失われません。

`configuration` オブジェクトには、実行の上限とサービス名を記録します。プロファイルは `wdm-x64-synchronous-v1` です。`nt_status` は引き続き DriverEntry の結果を示し、`scenario_success` は初期化と完了済みリクエストを合わせた結果を示します。`phase`、`requests`、`unload_completed` は、要求されたライフサイクルのどの部分が実行されたかを示します。API 呼び出しと CPU 書き込みにも、そのフェーズ（`driver_entry`、`request:N`、`unload`）を記録します。各リクエストはディスパッチと I/O のステータス、完了の有無、information 長、返された `output_hex` バイト列を報告します。`preferred_image_base` は元の PE ベースアドレスを示します。`security_cookie` は初期化した Cookie のゲストアドレスで、不要だった場合は `"0x0"` です。リクエストのレポートフィールドは `kind`、`device`、`code`、`irp`、`completed`、`dispatch_status`、`io_status`、`information`、`output_hex` です。

`instructions` は、実行ポリシーが許可したゲスト命令の試行回数を数えます。ポリシーが拒否した命令は数えません。許可後に CPU フォールトが発生した命令は数えます。合成した API ディスパッチと戻り先の番兵は、このカウンターを増やしません。

各 `writes` 項目の `semantics: "attempted_guest_write"` は、スタック外への CPU 書き込みの試行を記録したことを示します。その後にフォールトが発生したり、予算で停止したりする試行も含みます。書き込みの完了は保証せず、API モデルによる書き込みも含みません。デバイスとドライバーオブジェクトのスナップショットは、実行停止時に観測した状態を示します。

`neverd/sdk/NeverDCAPIEmulation.h`（または C API の統合ヘッダー）を include し、セッションを作成して `neverd_emulate_driver_json(session, path, options)` を呼び出します。空でないパスを明示すると、汎用解析 API による事前ロードを経ずに、厳格な実行前検査へ直接入ります。CLI もこの経路を使います。options に `NULL` を渡すとデフォルト値を選択します。`neverd_driver_options_v1` を明示する場合は `struct_size` が厳密に一致し、命令、メモリ、イベント、タイムアウトの各予算が正でなければなりません。結果は `neverd_free_string` で解放してください。

パスに `NULL` を渡す場合はロード済みセッションが必要で、IR 解析や関数を制限したロードとは独立して、そのファイルを再解析します。どちらの経路もセッションのイメージを変更しません。呼び出し中は入力ファイルを利用可能かつ変更されない状態に保ってください。リクエスト／環境構築の失敗は `NULL` を返して `neverd_last_error` を設定し、実行の停止は JSON を返します。機能が無効なビルドでも API は存在し、有効化の方法を報告します。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` は、同じ v1 オプションと所有権規則を使い、厳密に検証するシナリオ入力を追加します。NULL ではない NUL 終端 JSON 文字列が必要です。従来の `neverd_emulate_driver_json` ABI は変更せず、初期化のみを実行します。C++ パーサー `driverOptionsFromScenarioJSON` は、`emulateDriver` の呼び出し側にも同じシナリオ検証を提供します。

内部 C++ エントリーポイントは `include/neverd/emulation/DriverSession.h` の `neverd::emulation::emulateDriver` です。形式の解析は既存ローダー、Windows のオブジェクト／API 動作は `lib/emulation/windows`、CPU の状態と実行は Unicorn アダプターが担当します。アダプターとモデルは同じゲストメモリインターフェースを使います。Windows API の動作を Unicorn fork に実装すべきではありません。
