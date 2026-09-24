**Languages**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← ドキュメント索引](README.md)

# NeverD のテスト

NeverD のテストは、表現が期待した形か、バイナリ fixture のパイプライン経路が
動くか、生成コードが動作を保つか、という 3 つの異なる問いに答えます。変更の
問いに答える最小のスイートを選び、リスクの高いプルリクエストではより広い集約を
実行してください。

## テストビルドの構成

`BUILD_TESTING` を有効にしない限りテストは無効です。全スイートには通常 Release
を使います。Debug はアサーションとステップ実行を保ちますが、意図的に未最適化で
デコードベンチマークを代表しません。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

全 fixture には、クロスターゲットコンパイル用の `clang` と、`PATH` 上の LLVM
linker（`ld.lld` と `lld-link`）が必要です。CMake は多数の再配置可能 fixture を
常に作り、対応する linker があればリンク済み ELF/PE fixture も作ります。ホストが
fixture をコンパイル/リンクできずスキップされたテストは未実行のカバレッジであり、
そのターゲットの合格ではありません。

クローン、ビルドプロファイル、macOS のプリビルド LLVM は
[CONTRIBUTING.md](CONTRIBUTING.md)を参照してください。

## ドライバーエミュレーションの検査

`NEVERD_ENABLE_DRIVER_EMULATION=ON` と `BUILD_TESTING=ON` を両方有効にすると、専用の実行スイートと共有 C API／CLI の検査をビルドできます。

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

fixture は、ゲストの初期化、成功／失敗の戻り値、未対応動作、メモリフォールト、厳格なシナリオ解析、予算内の実行、および create、転送、cleanup、close、unload を通る同期バッファード／ダイレクト I/O、READ/WRITE、独立したファイルの寿命、MDL の権限、動的エクスポート、ゲスト可変引数、構造化 CPU フォールトを検証します。[`emulate-driver` CLI](driver-emulation.md) で JSON とプロセスの終了コードを確認してください。製品ビルドは `BUILD_TESTING=OFF` でもこの機能を有効にできます。`libneverd` がテスト専用の Unicorn 構成を必要としてはなりません。

追加テストでは、ドライバー所有の非ページプール MDL、記述子とバッファーの独立した寿命、レジストリ問い合わせの配置と短いバッファー、ハンドル権限、削除とリーク、および出力のない IOCTL の完全な64ビット `information_hex` を検証します。実際のサンプル検証には Zero の同期直接読み書きと統計問い合わせも含まれます。

バックエンドテストは完全な CPU コンテキスト（レジスタ、フラグ、SIMD、FPU、CR8）、共有メモリ、別バックエンドや障害後コンテキストの拒否を検証します。コンパイル済み `driver_dispatcher.c` は実際の DPC／ワーク項目、タイマー境界、通知／同期イベントとタイマー、理由 `Executive` の非アラート `KernelMode` 待機、タイムアウト／遅延、複数の待機スタック、セット直後のリセットでも保持する起床、コールバック引数、不正 IRQL／寿命を検証します。ワーク項目の保留／完了、キュー、停滞、共通予算の検証も維持します。文書化した部分集合の証拠であり、完全な Windows 非同期対応ではありません。

`driver_context_limits.c`: API の IRQL 上限は `KernelAPIIRQL.def` にあり、引数依存の制約は担当モデルが検査します。DPC からレジストリ API やページプールの割り当て・解放・アクセスはできません。Unicode `DbgPrint` 変換は `PASSIVE_LEVEL` を要求し、対応する ANSI 出力と非ページ操作は `DISPATCH_LEVEL` で使用できます。コールバックスタックには範囲があり、逸脱したスタックポインターは別の待機ワーカーのスタックへ侵入できません。デバイス拡張内の有効なタイマーは早期解放を防ぎます。一般の IRQL 変更を公開する機能ではありません。

`KernelDeviceStackTests.cpp` は所有関係と接続の独立性、最上位選択、失敗時の原子性、スタック容量、不透明フィールド、ハンドル数、切断・削除をまたぐワーク項目／要求の保持、ファイルとディスパッチ対象の違いを検証します。独自の `driver_wdm_stack.c` は実 WDK ヘッダーとインライン Copy/Skip/SetCompletion を使用し、通常／有効 CFG イメージを任意の `NEVERD_WDM_STACK_FIXTURE`／`NEVERD_WDM_STACK_CFG_FIXTURE` で指定します。`DriverWDMStackTests.cpp` は再配置、下位の実状態、完了順序／条件、遅延 pending 伝播、ワーカー／DPC、待機、`STATUS_MORE_PROCESSING_REQUIRED`、直接 MDL 保持、入れ子の完了、不正カーソル／制御を検証します。`DriverScenarioPublicTests.cpp` は C API／CLI 転送と C API の保持／入れ子完了を、設定された CFG イメージも含めて検証します。欠落時は明示的にスキップし、Linux の証拠は同一ドライバーのスタック範囲だけを示します。PDO／PnP／電源対応は示しません。 `KernelIRPStackTests.cpp` は個数に基づくカーソル、完全なインライン Copy 範囲、消費済み位置のクリア、状態／pending 伝播、MPR と入れ子完了、継続所有者、保持経路を検証します。実 READ/WRITE とファイルのライフサイクルもインライン Copy で検証します。

`DriverPnpScenarioTests.cpp` は JSON／ネイティブ検証の一致、必須初期情報、ID／個数制限、禁止フィールド、最終バス状態、nullable な観測レポートを検証します。`KernelPnpDeviceTests.cpp`、`KernelPnpRequestTests.cpp`、`KernelPnpCompletionTests.cpp` は所有権、AddDevice 成功／失敗／リーク、初期 IRP、ファイル受付、状態ロールバック、遅延完了、MPR／入れ子／待機継続、失敗の原子性を検証します。独自の実 WDK `driver_wdm_pnp.c` は任意の `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE` を使います。`DriverWDMPnpTests.cpp` は通常／有効 CFG の再配置イメージで AddDevice、ファイル I/O、順序付き削除、遅延開始／削除、開始／query 失敗、リーク有無の AddDevice 失敗を実行します。成果物がなければ明示的にスキップします。実行証拠は Linux のみで、記載したリソースなし PnP 範囲に限られます。 `DriverScenarioPublicTests.cpp` は通常／有効 CFG イメージの 7 要求の遅延 PnP レポートを C API と CLI でも検証します。

V9 の schema テストは 8 種の名前の往復と共有最終状態検証を確認し、QueryStop 0x119 をイメージ読み込み前に拒否します。拡張モデルと実フィクスチャは query-stop のロールバック、cancel-stop、停止／再開、突然の取り外し、厳密成功値、停止／取り外し待ちのソフトウェア I/O、取り外し後のゲスト拒否、デバイス識別、混在 AddDevice 結果を検証します。`DriverScenarioPublicTests.cpp` は通常／有効 CFG の 16 要求を C API と CLI から実行し、ソフトウェア IOCTL の正常バイト、突然の取り外し後の失敗、最終 cleanup/close/remove を保持します。公開実行は直列なので、現在の生成元がない保持 IRP を後続の start/cleanup で解除できません。Remove の排出条件はプロファイル境界であり、一般的な Windows の I/O 受付規則ではありません。証拠は Linux のみです。

`KernelRemoveLocksTests.cpp` は独立した所有者、NULL／重複 Tag、retail／DBG サイズ、即時／遅延排出、失敗取得の義務、原子性、容量、退役を検証します。`KernelRemoveLockBridgeTests.cpp` は接続前初期化、拡張部範囲、不透明領域、IRQL、危険な Delete／Detach の変更前拒否を確認します。真正 `driver_wdm_remove_lock.c` は retail／DBG と通常／active-CFG の4種で、`NEVERD_WDM_REMOVE_LOCK_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` を指定します。`DriverWDMRemoveLockTests.cpp` はパケット退役後の release、排出後のバス完了、コールバック復帰前の待機準備、ワーカー待機、リークなし AddDevice 失敗を確認します。C API／CLI は既存 PnP スキーマで受信／完了／最終撤去を区別します。欠落は明示スキップ、証拠は Linux のみで、完全な Driver Verifier や一般並行排出ではありません。

`DriverPowerScenarioTests.cpp` は必須電源値、JSON／ネイティブ整合、不透明32ビット context、応答 FIFO 上限、独立子報告を検証します。`KernelPowerRequestTests.cpp` と `KernelPowerCompletionTests.cpp` はレイアウト、経路フラグ、ライフサイクルと通知状態の区別、FIFO 一致、最終コールバック所有権、MPR、待機、解放境界を確認します。真正 WDK の独自 `driver_wdm_power.c` には任意の `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE` を使います。`DriverWDMPowerTests.cpp` は通常／active-CFG 再配置、直接／入れ子 Query/Set、独立遅延完了、S0 が D0 に先行する順序、待機を越える5引数スナップショット、ワーカー由来の子、null コールバック、query 拒否、PDO 別初期値／FIFO、明示値欠落を検証します。`DriverScenarioPublicTests.cpp` は不正入力と C API／CLI の6シナリオ／3子要求のスリープ復帰を追加します。成果物欠落は明示的にスキップし、Linux の実行証拠は文書化したページ可能・リソースなしの部分範囲に限ります。

`DriverResourceScenarioTests.cpp` は JSON／ネイティブ明示情報、整数幅、個数、物理／レジスター重複、整列、ID、空バンク、設定の直列化を検証します。`KernelMMIOTests.cpp`、`KernelMMIOFailureTests.cpp`、`KernelResourceBridgeTests.cpp`、`UnicornMMIOTests.cpp` はバンク／マッピング所有権、別名、世代、packed リストの寿命、プロバイダー時序、再開時の値保持、突然の取り外し／電源可用性、正確な CPU／API 取引、失敗の原子性を扱います。独自の真正 WDK `driver_wdm_resources.c` には `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE` を使います。`DriverWDMResourceTests.cpp` は実際のスカラー／REP アクセサー、通常／active-CFG 再配置、部分範囲の別名、ページ末尾マッピング、STOP／再開、不正アクセスを実行します。C API／CLI はイメージロード前の不正情報拒否と同じ14要求の再開シナリオを検証し、保持された IOCTL 出力と正確な map／unmap 回数を確認します。共通 [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) にはこの fixture のレジスター／IOCTL プロトコルが必要です。成果物不足は明示スキップ、証拠は Linux のみで、ホスト物理メモリや一般デバイスバックエンドは扱いません。

`DriverDMAScenarioTests.cpp` は明示能力、論理ドメイン、バイト／件数／時刻上限、方向の厳密性、設定と観測の分離を検証します。`KernelPhysicalMemoryTests.cpp` と `BackendBackingTests.cpp` は同一ページ上の割り当て境界、固定参照、CPU 権限不変、MMIO／再入の排除、全範囲検証失敗の原子性を確認します。`KernelRequestMDLTests.cpp` は構築済み記述子の別名と読み取り専用 PFN が同じ物理識別子を使うことを確認します。`KernelDMATests.cpp`、`KernelDMABridgeTests.cpp`、`SchedulerDMATests.cpp` は実 RAM、束縛されたテーブル呼び出し、インライン／待機 FIFO、別々のコールバック／マッピング寿命、ページ断片、方向違反、解放前検証、PDO 分離、世代／電源失敗を実行します。真正 WDK の独自 `driver_wdm_dma.c` は `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE` を使い、`DriverWDMDMATests.cpp` と C API／CLI は実アダプターポインター、共通／SG ストレージ、別々に設定した DMA／割り込みを実行します。共有 [driver-dma-scenario.json](../examples/driver-dma-scenario.json)はこの fixture のプロトコルが必要です。成果物不足は明示スキップし、証拠は Linux に限定され、実ホスト DMA、PCI、一般デバイスエンジンを保証しません。`pluginsdk/python/tests/test_driver_dma_integration.py` は `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_FIXTURE`、任意の `NEVERD_TEST_WDM_DMA_CFG_FIXTURE` で同じ JSON 境界を Python から実行し、バイト列、FIFO、コールバック内完了、不正 JSON、IRP 完了後の独立 DMA 失敗を検証します。

`KernelSEHTests.cpp` は純粋な展開計画、スコープ順序、不揮発 GPR の復元、有界スタック、明示的な未対応メタデータを検証します。`KernelExceptionTests.cpp` は正確な API 引数数、下位 32 ビットのステータス、型付き例外、IRQL 上限、モデル／CPU 状態を変更しないことを確認します。真正 WDK と `/GS-` の `driver_wdm_seh.c` は任意の `NEVERD_WDM_SEH_FIXTURE`／`NEVERD_WDM_SEH_CFG_FIXTURE` を使います。`DriverWDMSEHTests.cpp` は通常／有効 CFG／再配置イメージで直接およびヘルパーからの例外送出、入れ子ハンドラー、再送出、未捕捉例外、フィルター／finally／CPU フォールトの明示的拒否を実行します。C API／CLI は [driver-seh-scenario.json](../examples/driver-seh-scenario.json) を実行し、null の API 結果と実際のゲストハンドラーメッセージを確認します。`pluginsdk/python/tests/test_driver_seh_integration.py` は `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_SEH_FIXTURE`、`NEVERD_TEST_WDM_SEH_CFG_FIXTURE` を使用します。外部イメージがなければ明示スキップし、証拠は Linux に限定され、ユーザーバッファーや一般 SEH への対応を示しません。

`KernelDMAChannelTests.cpp`、`KernelDMAChannelBridgeTests.cpp`、共有の `SchedulerDMATests.cpp` は、混在する割り当ての FIFO、コールバック戻り値幅、純粋な受付／解放検証、レジスター再利用、連続ページ断片、操作全体のフラッシュ、CurrentIrp の捕捉、パケット／MDL／デバイス寿命を検証します。独自に作成し真正 WDK でビルドした `driver_wdm_dma_channel.c` には任意の `NEVERD_WDM_DMA_CHANNEL_FIXTURE`／`NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE` を使います。`DriverWDMDMAChannelTests.cpp` は通常／有効 CFG／再配置イメージを実行し、実際の MapTransfer と FlushAdapterBuffers、共通／SG／チャネルの共有上限、明示的デバイストランザクション、IRQ/DPC 完了、連続する操作、二つの PDO、失敗ケースを扱います。C API／CLI は 7 リクエストの [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json)を実行し、二つのマッピング断片にまたがる単一トランザクションも含みます。`pluginsdk/python/tests/test_driver_dma_channel_integration.py` は `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE`、`NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` で同じ公開 JSON インターフェースを使います。成果物不足は明示的にスキップし、Linux の実行証拠はシステム DMA コントローラーや任意の HAL マッピング／フラッシュの対応を意味しません。

`DriverInterruptScenarioTests.cpp` は明示的な raw／変換後記述子、混在および割り込み専用割り当て、厳密なイベントフィールド／件数、発生元識別子、独立 BOOLEAN 観測を検証します。`KernelInterruptsTests.cpp`、`KernelInterruptBridgeTests.cpp`、`SchedulerInterruptTests.cpp` は排他的な組の照合、不透明トークン、世代／接続の捕捉、イベント寿命、選択した Ex フィールドだけの読み取り、共有するロックと IRQL 復元、コールバック所有権、同時刻 ISR の優先度、変更前の容量失敗を扱います。`KernelFrameworkRequestTests.cpp` は呼び出し公開や参照消費を伴わない純粋なキャンセル事前検証と一括トークン容量を確認します。真正 WDK を使う独自 fixture `driver_wdm_interrupts.c` は `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE` を使用します。`DriverWDMInterruptTests.cpp` は通常／active-CFG 再配置、旧来の 11 引数 ABI、Ex 1／2／4、実際の ISR→DPC 完了、AL 下位の FALSE、同期／手動ロック、独立 PDO、再開時の世代、不正なハードウェア情報を実行します。C API／CLI テストはイメージロード前に不正宣言を拒否し、7 要求の [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json) で保留 IOCTL のバイト列と独立した配信観測を確認します。イメージ不足は明示スキップします。実行証拠は Linux のみで、共有／レベル／MSI 割り込みや命令単位のプリエンプションを証明するものではありません。

`DriverGuardTests.cpp` と独自の `driver_guard.c` の 4 変種は、有効／無効の CFG、ベースアドレスの再配置、check/dispatch ABI、不正なターゲットを検証します。`KernelFrameworkTests.cpp`、`KernelFrameworkControlTests.cpp`、`KernelFrameworkQueueTests.cpp`、`KernelFrameworkRequestTests.cpp` は、バインド、失敗時にロールバックするデバイス作成、キューのルーティング、バッファーの論理長、クリーンアップの順序と IRP／コンテキストの寿命を検証します。独自の `driver_kmdf_lifecycle.c` と `driver_kmdf_control.c` は、実際の WDK 1.33 ヘッダーで任意にコンパイルし、本物の `FxDriverEntry` ライブラリを通じてリンクします。CMake キャッシュのパスとして、ライフサイクルのイメージには `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE`、制御デバイスの通常版／CFG 有効版には `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` を設定します。外部成果物がなければ明示的にスキップします。`DriverKMDFLifecycleTests.cpp`、`DriverKMDFControlTests.cpp`、`DriverScenarioPublicTests.cpp` の C API／CLI ケースは、実際のコールバック、バッファード／ダイレクト I/O、ワークアイテムによる保留要求の完了、失敗ステータス、アンロード、再配置後の CFG 実行を検証します。実行証拠は Linux に限定され、完全な KMDF や PnP／電源管理の対応を示すものではありません。

旧版キャンセルのテストは、キャンセル、入れ子のクリーンアップ、最終破棄まで API 継続処理を保持します。既にキャンセル済みの Ex は引き続きコールバックなしでキャンセル状態を返します。`KernelFrameworkRequestAccessorTests.cpp` と `KernelRequestMDLTests.cpp` の検証対象は、共有 64 ビット Information、完了時の長さ検査、元キュー／IRP 識別、NULL WDF ファイルハンドル、保持ハンドルの getter 結果、バッファー MDL キャッシュと最初の方向の ByteCount、直接記述子の識別と遅延マッピング、完了時の無効化、WDM 完了による迂回の拒否です。実際の制御デバイス fixture の L、M、D、C モードは通常／CFG 有効イメージで、旧版キャンセル、バッファー MDL／情報、直接 READ／WRITE MDL、完了後アクセスを実行します。

キャンセルのテストは、転送要求だけに許す仮想期限とレポートフィールド、完了優先と既にキャンセル済みの経路、マーク／解除の結果、キュー登録と配信済みの完了権限、コールバック待機、内部参照の寿命を検証します。スケジューラーのテストは DPC／キャンセル／ワーク項目の順序、容量、識別子の分離、中断／再開を独立に検証します。WDM キャンセルは明示的なモデルエラーのままです。


## テスト構成

`add_neverd_unittest` は GoogleTest 実行ファイルを 1 つ作り、検出した各ケースに
その実行ターゲット名と同じ CTest ラベルを割り当てます。

| ソース領域 | ターゲットと CTest ラベル | 対象 |
|------------|---------------------------|------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | クロスプラットフォーム子プロセス、引用、リダイレクト、終了コード |
| `unittests/libc` | `NeverDLibCTests` | 既知の libc 名と分類 |
| `unittests/safety` | `NeverDSafetyTests`、`NeverDSafetyIntegrationTests` | シンクカタログ、識別優先順位、引数事前フィルタ、コピー越境ハント、ヒープ寿命監査、必須の PE/ELF/Mach-O × x86-64/AArch64 6 セル行列 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter の LowIR 形状、IR 段階、loader、relocation、形式 fixture、デコンパイル、代表的 patch 経路 |
| `unittests/semantic` の大半 | `NeverDSemanticTests` | 命令、ABI、制御フロー、C 式、lift/recompile の差分セマンティクス |
| `unittests/evm` | `NeverDEVMOpcodeTests`、`NeverDEVMBytecodeTests`、`NeverDEVMLoaderTests`、`NeverDEVMABITests`、`NeverDEVMAnalyzerTests`、`NeverDEVMDecoderPropertyTests`、`NeverDEVMProxyTests`、`NeverDEVMCallTests`、`NeverDEVMSemanticTests`、`NeverDEVMEmitterTests`、`NeverDEVMIntegrationTests` | hardfork metadata、input normalization、ABI/signature ambiguity、CFG/SSA/recovery、decoder boundary 全網羅と hostile input、proxy/call fact、interpreter semantics、LLVM/C/Solidity differential execution、public API routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFProgramImageTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFVerifierTests`、`NeverDSBFISAConformanceTests`、`NeverDSBFAgaveConformanceTests`、`NeverDSBFSemanticTests`、`NeverDSBFEmitterTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`、`NeverDSBFMalformedCorpusTests`、`NeverDSBFUpstreamConformanceTests`、`NeverDSBFExternalOracleTests`、`NeverDSBFSolanaModelTests`、`NeverDSBFIntegrationTests` | v0-v4 メタデータと ELF レイアウト、厳格な verifier/loader 動作、固定済み ELF 成果物 23 個、独立 official oracle、全 opcode の可用性、敵対的入力、CFG/復元、実行済み LLVM/C/Rust 差分 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 4 ISA×3 オブジェクト形式の書き換え/難読化等価性 |
| `unittests/semantic` の重点変換ファイル | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 大きなセマンティック実行形式から分離した高速再リンク用プローブ |
| `unittests/corpus`（submodule） | `NeverDWindowsEHCorpusTests`、`NeverDRustEHCorpusTests`、`NeverDGoEHCorpusTests`、`NeverDCxxItaniumEHCorpusTests`、`NeverDObjCEHCorpusTests`、`NeverDAdaDEHCorpusTests` | pin された 545 個の実バイナリから読み取る例外とランタイム metadata。各バイナリは manifest で復元が満たすべき下限を宣言している |

登録の信頼できる情報源は
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt)、
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt)、
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt)、
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt)、
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt) です。

### pin されたバイナリ corpus

他のスイートはテスト対象を自分でビルドしますが、corpus は違います。これは実際の
ツールチェーンが、このリポジトリからは到達できないホスト上で到達できないターゲット
向けに生成したバイナリの submodule であり、各ファイルはダイジェストで pin され、
隣の manifest がその復元の満たすべき下限を宣言しています。「`-O2` で strip された
`armv7` の共有オブジェクトから NeverD が何を読み取れるのか」という問いに、議論では
なく答えを出せる場所はここだけです。

これらのスイートは configure がそれらを探すよう指示されたときにのみビルドされるので、
このフラグがテスト対象であり続けるかどうかのすべてです。

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` は全ラインを、`check-neverd-windows-eh-corpus`、
`check-neverd-rust-eh-corpus`、`check-neverd-go-eh-corpus`、
`check-neverd-cxx-itanium-eh-corpus`、`check-neverd-objc-eh-corpus`、`check-neverd-ada-d-eh-corpus` はそれぞれ 1 ライン
を実行します。CI の 3 ホストすべてがこのフラグ付きで configure し、6 ライン全部を
実行します。バイトはどこでも同一ですが、それを読むものは同一ではなく、1 ホストでの
corpus 実行は他の 2 ホストについて何も証明しません。
`scripts/audit_ci_test_inventory.py` は 6 つの label のどれかを欠く inventory を拒否
します。corpus を静かに読まなくなったビルドは、どのテストにも捕捉できない回帰だから
です。消えたものがテストそのものなのです。

live EVM opcode audit は次のコマンドで実行します。

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

public CLI が受理する唯一の option は `--manifest-output` で、remote/ref/toolchain override は
提供しません。出力 manifest の closed contract は `schema 3` です。

ローカルと CI の標準経路は、公式
`https://github.com/ethereum/go-ethereum.git` に対して必ず
`git fetch --depth=1 --force` を行い、default branch の remote `HEAD` から得た正確な
SHA だけを detached worktree で検査します。各実行は予測不能な名前の private temporary
bare repository を使い、official fetch の authority ref と exact SHA を detached worktree
の生存期間中保持して、最後に repository と worktree をまとめて破棄します。shared persistent
Git repository や cache はありません。`local_docs`、既存 checkout、
submodule は監査経路ではなく、pin された submodule は live drift を検出すべき時点で古くなります。

各 Git command は継承した `GIT_*`（`GIT_CONFIG_*` を含む）を最初に全消去し、監査済みの
値だけを設定します。`GIT_CONFIG_NOSYSTEM` と `GIT_CONFIG_GLOBAL` は system/global
config を、`GIT_ATTR_NOSYSTEM` と command scope の `core.attributesFile` は system/global
attributes を、`core.hooksPath` は hooks を無効化します。想定外の private-repository config、graft、
`objects/info/alternates`、`refs/replace` は検証失敗となり、
`GIT_NO_REPLACE_OBJECTS` も replacement lookup を無効化します。

probe は `params.Rules` の export 済み bool field をすべて反射し、各 fork で
`LookupInstructionSet(params.Rules)` を呼んで 256 byte slot 全体を走査します。
`EVMUpstreamOpcodePolicy.def` は typed historical/unscheduled-EOF exclusion と alias、
`EVMUpstreamSemanticsPolicy.def` は closed Rules inventory、fork mapping、base-stack
exception、EIP-8024 dynamic opcode family の宣言をそれぞれ所有します。

CI は `dev` への push、pull request、manual dispatch、daily schedule でだけ同じ live
audit を実行します。Go probe は対応する各 fork で公開 API
`LookupInstructionSet(params.Rules)` を呼びます。`EVMUpstreamOpcodePolicy.def` は name
alias と review 済み historical/unscheduled-EOF exclusion を、直交する
`EVMUpstreamSemanticsPolicy.def` は fork rule、stack semantics 例外、EIP-8024 dynamic opcode
family の membership/activation を所有します。
closed manifest は正確な revision、fork activation、byte/name、`base_min_stack`、
`net_stack_delta` を検査し、未知または重複した field、fork、name、byte を拒否します。
allocation は `operation.undefined` だけで判定し、`HasCost` は defined zero-cost operation
でも false なので cost cross-check にのみ使います。すべての `defined && !HasCost` slot は
宣言した fork から `EVM_GETH_ACTIVE_WITHOUT_COST` と正確に一致する必要があります。cost を
持つ undefined slot、未レビューの defined slot、marker の消失は fail closed です。
CI 失敗時には正確な revision、manifest、log が artifact になります。parser と drift
diagnostic には独立した Python unit coverage があります。

`EVMUpstreamSemanticsPolicy.def` は export された boolean `params.Rules` field ごとに唯一の
`EVM_GETH_RULE_FIELD` を置き、`MappedForkSelector`、`NoOpcodeAllocation`、
`ExcludedSelectorExpectedError` のいずれかに分類します。probe は field を 1 つだけ有効にして
`LookupInstructionSet` を呼びます。最初の 2 category は nil error、3 番目は error でなければ
ならず、返された完全な 256-slot opcode/stack fingerprint は `ExpectedFork` と一致する必要が
あります。`IsEIP155`、`IsEIP2929`、`IsEIP4762`、`IsPetersburg` は現在 Frontier fingerprint
の no-allocation fields、`IsUBT` は error と Cancun fingerprint が期待値です。

`EVMEIP8024Immediates.def` は引き続き single/pair の各 byte に対する immediate semantics の
唯一の authority で、各 256 byte を明示分類します。production は直接 lookup します。live
audit は `go -overlay` で `core/vm` に virtual wrapper を注入して本物の private
`operation.execute` handler を得て、active な table/family ごとに `DUPN`、`SWAPN`、
`EXCHANGE` の `3x256` candidates と `3 missing-operand cases` を実行します。acceptance、PC
delta、marker-derived operand/stack mutation、valid case の正確な underflow、operand 欠落時の
`0x00` を検査し、Python は formula を再記述せず同じ `.def` と比較します。

`EVM_HARDFORK_LATEST` の canonical target は 1 つだけです。closed
`EVMUpstreamForkAliases.def` は Prague→Pectra、Osaka と BPO1〜BPO5→Fusaka、
Paris/Shanghai/Cancun/Amsterdam/Bogota→自身を定義し、未知名は fail closed です。記録した
1 つの `audit_unix_time` で `MainnetChainConfig.LatestFork(time)`（NeverD latest と一致必須）
と `LatestFork(max uint64)` の alias/probed canonical fork を検査します。probe は実在する
`canonical fork jump tables` と `mainnet active/scheduled jump tables` を列挙して一表ずつ完全
比較し、dynamic family または fork の `inactive` 状態を明示的に記録します。一部の
table/family/probe しか得られない `partial` result は受理せず fail closed です。manifest は
`authority=official-fresh-fetch`、公式 URL、要求 `HEAD`、SHA を固定
します。public CLI に remote/ref/toolchain bypass はなく、probe は `GOTOOLCHAIN=local` です。

Go の request/response と Python controller は hostile metadata を allocate する前に
`input/collection/string hard limits` を適用し、上限を超える input、array、string を fail
closed にします。別途 `bounded diagnostic output` を強制し、長すぎる表示には full-content
`digest` と `explicit truncated marker` が含まれます。すべての command に bounded child output
と共通 deadline が適用され、timeout または output-limit 違反は `process group` 全体と子孫
process tree を kill して pipe を drain します。すべての `.def parser` は unparsed、unknown、
duplicate、missing、out-of-range の entry を拒否して fail closed します。

現在の schema-3 live receipt は `schema_version=3`、
`audit_unix_time=1787534659`、`authority=official-fresh-fetch`、
`remote=https://github.com/ethereum/go-ethereum.git`、`ref=HEAD`、revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`、local `Go 1.24.0`、
`stack_limit=1024`、`diagnostics=[]` を記録しています。`21 fork tables` と
`20 Rules probes` を対象とし、分類は `15 mapped/4 no-op/1 expected-error` です。2 つの
`mainnet active/scheduled` record は `upstream BPO2` を報告し、closed map はこれを
`NeverD Fusaka` に対応させます。EIP-8024 の `23 table targets` のうち active なのは
`Amsterdam/Bogota` だけで、`1536 candidate executions` と `6 missing-operand cases` を
生成します。`three handler symbols` は 2 つの active target 間で一致します。Python audit は
`67/67`、`C++ Opcode 10/10` です。macOS の実 run は `sandbox-exec` 内で成功し、最後の
`go run` は offline でした。Linux workflow は `bubblewrap` を必須にします。

すべての Go stage、すなわち `go env`、`go mod init`、`go mod edit`、`go mod tidy`、
`go mod download`、`go run` は `capability-root` filesystem sandbox を通過する必要があります。
read capability は private probe、fresh geth、検証済み `resolved GOROOT`、必要な system runtime
root の正確な集合だけを含み、書き込み可能なのは isolated environment root だけです。network は
必要な dependency stage にのみ許可され、final run は offline です。test は
`host HOME/workspace` に sentinel を置き、access が拒否され、どの output にも内容が現れないことを
要求します。Linux は `/` broad bind を持たない同型の `bubblewrap` policy を検証します。

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

CMake に登録された 11 個の EVM test target は次のとおりです。

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` は decoder が変わる各 fork で全 2-byte input を網羅し、
完全な decode と正確な `JUMPDEST` boundary を比較します。さらに長さを制限した決定的な
hostile input を全 fork に通します。

EVM control-flow の変更では、まず fixed-point と height-domain contract を実行します。

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

これらの case は block をまたぐ internal return、有限 multi-target merge、loop
convergence、deterministic edge ordering、path-sensitive whole-stack lane、correlation
preservation、unknown jump、exact invalid target、fail-loud budget、strict/relaxed stack
fault を網羅します。`MayReachable` は CFG candidate のみで確定 semantic fact を作れません。
続けて 11 個すべての EVM target と live upstream audit を実行してください。

MedIR/HighIR dataflow の変更では、constant-phi、selector、typed-operand、
malformed-graph、deep-chain contract も実行します。

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

これらは equal/conflicting cyclic phi、非隣接または block をまたぐ selector expression、
両方の equality operand order、exact ABI width check、typed storage/event/calldata
operand、malformed MedIR の deterministic handling、16,384-value の iterative producer
walk を検証します。

## fixture の生成方法

### Lift と形式 fixture

`unittests/lift/CMakeLists.txt` はビルド中に C とアセンブリソースをクロス
コンパイルします。Clang の target triple が x86-64、i386、AArch64、ARM32 の
ELF オブジェクト、PE/COFF オブジェクトとリンク済みイメージ、PIC/no-PIC の
Mach-O i386 オブジェクトを生成します。LLD があれば、選択したオブジェクトを
patch テスト用の実行形式にもリンクします。`NeverDLiftTests` は
`lift-test-objects` ターゲットに依存するため、通常のテストバイナリビルドで生成
fixture が更新されます。

多くの lift テストは `NeverDLiftFixture.h` からビルド済み `neverd` CLI を呼び、
LowIR、MedIR、HighIR、LLVM IR、生成 C、書き換えバイナリを検査します。重点的な
手動実験では `NEVERD` 環境変数で CLI パスを上書きできます。通常の CTest は
CMake が埋め込んだ実行ファイルを使います。

### メモリ安全性 fixture

`unittests/safety/fixtures/binaries` には x86-64 と AArch64 の PE、ELF、Mach-O
イメージが検入されており、各形式が用意する PDB または dSYM の付属ファイルに
加えて、イメージごとにリンカ MAP が付きます。MAP は strip されたビルドが唯一
残す識別情報なので、各セルは MAP を明示的に指定した解析も行い、型も行番号も
残っていない状態で発見が何を主張できるかを固定します。
`NeverDSafetyIntegrationTests` はすべてのホストで 6 セルすべてを実行します。
必要なイメージや付属ファイルが欠けていれば構成段階で失敗し、ホストのツール
チェーンによるスキップ経路はありません。

6 つの等価なバイナリは 1 つのソースから生成します。`make` はホストネイティブ
の smoke fixture だけを再構築します。検入済みの完全な行列を再生成するには次を
使います。

```bash
make -C unittests/safety/fixtures matrix
```

行列のレシピには Clang の Linux／Windows クロスターゲット、LLD の COFF ツール、
両方の Darwin アーキテクチャ、そして `dsymutil` が必要です。デバッグパスは
再マップされ、CodeView のコマンドライン記録は無効化されるため、検入された
付属ファイルが開発者のワークスペース絶対パスを取り込むことはありません。

### Windows 例外再構築

Windows のテーブルベース例外を変更する場合は、表現テストとリンク済み PE の
patch テストの両方が必要です。対象を絞った lift-suite フィルターは、正規化された
unwind/SEH/C++ モデル、破損入力処理、例外 CFG エッジ、HighIR、LLVM WinEH 生成、
例外ディレクトリの置換、および Guard CF/EH continuation の再構築を網羅します。

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

保護された x64 assembly fixture には Clang の Windows target と `lld-link` が必要で、
CMake link は `/guard:cf` と `/guard:ehcont` を使用します。cross-linker 不足による
skip は final-image 経路の証拠にはなりません。統合ケースが成功すれば、書き換えた
PE を再ロードでき、runtime-function、unwind、load-config、Guard CF、Guard EH
continuation の各テーブルがソート済みでファイルに裏付けられ、実行可能 target のみを
指すことを確認できます。

リンク済み FH3 fixture は、固定状態テーブル、HighC 注釈、personality の保持、生成した
catch target、再ロード後の IP-to-state グラフからなるネイティブ C++ closure を独立して
検証します。

解析／ネイティブのサポート表と fail-closed patch 契約については、
[Windows 例外再構築](windows-exception-reconstruction.md)を参照してください。

### 言語例外モデル

Windows テーブルモデル以外のすべては一つの絞り込み target にまとまっています。
`NeverDLanguageEHTests` は DWARF フレームチェーン、Itanium 言語固有データ領域、
ARM EHABI、Darwin compact unwind、Go ランタイムのフレームメタデータ、Rust の
panic 機構、そして三つの Objective-C ランタイムを網羅します。

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

このスイートのテーブルはコンパイルではなくバイト単位で組み立てています。
検証したい組み合わせの多くは、単一の toolchain がまとめて出力することがない
からです。Objective-C が最も分かりやすい例で、三つのランタイムはいずれも
Itanium LSDA を出力し、違いは型テーブルのスロットに何を置くかだけ——しかも
その違いは程度ではなく全面的です。Apple のスロットは `objc_typeinfo` を指し、
その最初の二つのフィールドは意図的に `std::type_info` を模しています。GNUstep
の Objective-C++ スロットは本物の `std::type_info` 派生型を指し、GNU ランタイム
のスロットはそもそもポインタですらなくクラス名の文字列そのものです。あるランタイム
の規約を別のランタイムのテーブルに当てはめても失敗はせず、まったく別のものの
途中から読み取ったクラス名を報告するだけです。だからスロットを読む前に、フレーム
の personality からランタイムを確定します。

同じスイートは、まとめてしまいがちで、まとめると誤りになる二つの区別も固定します。
`@catch(id)` と `@catch(...)` は別のハンドラで——前者は任意の Objective-C
オブジェクトを受け取り、外来例外はその横を通過させます——各ランタイムで綴りが
異なります。両方を catch-all として報告するデコーダは、本来素通りするはずの例外に
ハンドラを付けてしまいます。また setjmp/longjmp の call-site テーブルはアドレスでは
なく呼び出し位置の索引を並べるため、SJLJ personality を認識しそこねた読み取り側は
エラーにならず、プログラムが指定していない保護範囲と landing pad を捏造します。

その形式を認識することと、解読を拒むことは別です。SJLJ の 1 エントリは ULEB128 の
組——ディスパッチ用のセレクタと action オフセット——であり、この action オフセットの
意味はアドレス形式のそれと完全に同じです。したがって action チェーンも catch の型も
例外仕様も、コードを一切名指ししない表から読み出せます。読み出せないのは各エントリが
守る範囲だけで、それを語るのは関数自身が call-site スロットへ行う書き込みであって、
表の中の何かではありません。本スイートはここで信用してはならない 1 バイトも固定します。
call-site エンコーディングとして GCC は `DW_EH_PE_uleb128` を、LLVM は
`DW_EH_PE_udata4` を書きますが、どちらもその後 ULEB128 を出力し、どの personality も
それを読みません——ならばデコーダも読んではなりません。

personality の同定も併せて固定します。上のあらゆる表をどう読むかを決めるのがそれだから
です。GNAT は GCC が各フロントエンドに与える 3 通りの綴り——`_v0`、`_sj0`、`_seh0`——で
自らのルーチンを名づけ、Windows では一方のシンボルを登録して他方へ転送するので、4 つの
綴りすべてが Ada に行き着かねばなりません。D はその鏡像で、3 つのコンパイラ、1 つの
ルーチンに対する 3 つの名前、その背後にあるのは同一の表です。

### Unicorn 差分ラウンドトリップ

セマンティック fixture はテキストの形ではなく動作を検査します。

1. 小さな C/アセンブリケースを書くか LLVM IR を構築する。
2. Clang/LLVM で指定ターゲット向けにコンパイルする。
3. 元の機械語を Unicorn で実行し、期待する戻り値など fixture 定義の状態を取得する。
4. NeverD で読み込んで lift し、LLVM IR を出力して機械語へ再コンパイルする。
5. 同じ ABI、入力、メモリ配置、CPU モデルで再生成コードを実行する。
6. 観測可能な結果を比較する。

主な実装は
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h)
です。patch-full fixture は patch 操作と同じ rewrite backend である
`Codegen::compileForRewrite` を使い、4×3 の全 ISA/形式グリッドで基準コードと
変換コードを比較します。

決定的な NeverD のセマンティック失敗はテスト失敗にしてください。skip は明示的な
外部能力の境界に限り、その理由を読んでください。クロス linker がない状態の緑色
サマリーは、形式経路の実行を証明しません。

### EVM 差分バックエンド

interpreter test は deterministic 256-bit oracle です。emitter suite は LLVM を
compile/execute し、C23 を Clang で同じ host harness に lower し、`solc`、`anvil`、
`cast`、`jq` があれば generated Solidity を local node に deploy します。status、
storage、instruction trace count を比較します。別の raw-bytecode corpus は Anvil native
EVM 上で pre-Fusaka ALU、calldata/memory copy、overlapping `MCOPY`、Keccak、return
data を実行します。

Low/Med のテストは path-sensitive whole-stack execution lane と phi lane identity を
維持し、`MaxAbstractInstructionTransfers` を含む budget exhaustion を hard error にします。
strict は証明済み `Reachable` lane 上の unknown/fork-inactive opcode だけを拒否し、
`MayReachable` は確定 fact を生成しません。HighIR の selector/receive/fallback は root
lane と成功 terminal に制限されます。共有 selector は独立した standard evidence では
なく、standard ごとの `KnownFunctionVariantInfo` と成功 terminal の厳密な return shape
が一致したときだけ variant と return list を選びます。

interpreter は opcode 固有の side effect より前に typed stack preflight を実行します。
`EVMForkSemantics.def` は byte `0x44` を Paris より前の `DIFFICULTY`、Paris 以降の
`PREVRANDAO` と定義します。`REVERT`、fault、step limit、resource exhaustion は state を
rollback します。allocation failure は `ExecutionFaultKind::ResourceExhausted` であり、
entry snapshot 自体が作れなければ `HasPersistentStateSnapshot` は false で commit 不能です。

### EVM public boundary と budget regression

public API test は canonical
`Code`/`Fork`/`Instructions`/`JumpDestinations` と、すべての LowIR table、range、ID、lane、edge
reference を個別に改ざんします。`execute` は instruction lookup 前に `llvm::Error` を返し、
`lowerToMedIR` は index 構築や入力比例 allocation の前に、完全な malformed/over-budget LowIR
を拒否しなければなりません。`lowerToMedIR` については option validation、resource validation、
structure validation の順序を強制し、field ごとの `canonical decode replay` と
`lowerCanonicalLowToMedIR` より前に完了させます。public HighIR recovery は外部 LowIR/MedIR を
replay 検証し、`analyze` だけが自身の canonical IR に `lowerCanonicalLowToMedIR` と
`recoverCanonicalHighIR` を使用できます。これにより recursive/duplicate replay を避けつつ、
すべての HighIR option/resource budget を引き続き適用します。
interpreter は `EVMInterpreterLimits.def` の全 limit を exact
boundary/+1 で検証します。`MaxSteps` は専用 `StepLimit`、`MaxMemoryBytes`、
`MaxTraceEntries`、`MaxLogEntries`、aggregate `MaxLogDataBytes`、runtime
`MaxPersistentStateEntries` の exhaustion は `ResourceExhausted` で transaction effect を
rollback します。初期 aggregate `MaxHostReturnDataBytes` または persistent state の超過は API
error です。初期 `MaxCalldataBytes`、`BlockHashes`/`Balances`/`CodeHashes`/`ExternalCode`/
`BlobHashes` 全体の aggregate `MaxHostEnvironmentEntries`、aggregate
`MaxExternalCodeBytes` も API error です。`const execute preflight` は environment、snapshot、
result の copy より前にこれらを拒否します。return-data `ArrayRef` view と sort 済み table の
`lower_bound` lookup も、buffer copy や PC map なしで検証します。

独立した LowIR boundary test は aggregate diagnostic limit
`MaxLowDiagnostics` と `MaxLowDiagnosticBytes` を検証し、linear decode/CFG construction が
正確な count/最終 bytes を precharge して zero を拒否することを確認します。
HighIR safety test は lane ごとの sort 済み `Any/Exact/Excluded` domain、equality
match/exclusion、raw `XOR(selector, constant)` の false-edge match/true-edge mismatch、zero
word/calldata size/call value refinement、unknown condition の fail-closed を網羅します。
さらに `EQ` と `raw XOR` の両方の back-jump regression を検証し、別の function によって
`arguments`、`mutability`、`return shape`、`region` が汚染されないことを保証します。
`EVMAnalysisLimits.def` の `MaxHighDispatchCandidates`、aggregate
`MaxHighRecoveredArguments`、`MaxHighDiagnostics`、`MaxHighDiagnosticBytes`、
`MaxHighReferenceVisits`、`MaxHighMemoryTransferCells`、`MaxHighMemoryValueVisits` は exact
boundary/-1 で検証されます。fixed malformed diagnostic を含む全 output diagnostic は allocation
前に count と最終 bytes を課金しなければなりません。LowIR と HighIR の diagnostic budget は
独立に検証し、default root CFG region は block-PC list の reserve/copy より前に
`MaxHighRegionBlockReferences` を課金しなければなりません。
外部 CALL/CREATE result は nondeterministic host outcome として 2 本の正確な CFG edge を
検査するため ERC-1167 fallback recovery が保たれます。読めない selector condition は Unknown
のままで、fallback/function fact を作れません。

control-flow test は `EVMLowFaultKinds.def` の `InvalidJumpDestination` を
`end-of-code JUMPI` に適用します。invalid target かつ確実に true なら successful tail はなく
definite fault、確実に false なら成功です。unknown は成功し得る false path を残し、lane 全体を
definite fault としません。

ABI test は `EVMABIParserLimits.def` の grammar boundary と `EVMABITableLimits.def` の public
table cardinality/text boundary を exact limit/+1 で検証します。また invalid
kind/standard/evidence enum、metadata mismatch、noncanonical signature/return list、誤って
independent とされた shared selector、dangling/duplicate variant、word width でない event-topic
`APInt` を indexed selector/sorted topic lookup より前に拒否します。

`NeverDEVMOpcodeTests` は metadata architecture も強制します。割り当て済み opcode の
encoding/typed-value roundtrip、family boundary、hardfork alias、derived stack/host
maxima を検証します。

### Solana SBF 差分バックエンド

SBF メタデータテストは、各バージョン機能、オペコード衝突境界、Murmur3 syscall hash、リロケーション、ELF machine、レジスタ、VM アドレス定数を検証します。Loader fixture は vendored バイナリを使わず、従来の v0-v2 section レイアウトと section を持たない厳格な v3/v4 program-header レイアウトの両方を生成します。

`NeverDSBFISAConformanceTests` は v0-v4 の各 version について、すべての byte
encoding を独立監査済みの typed manifest と照合します。
`NeverDSBFExternalOracleTests` は activation と boundary の判断を、別途 build
した official Anza process と比較します。`NeverDSBFUpstreamConformanceTests`
は pinned Anza revision にある 23 個すべての ELF に明示的な outcome を割り当てます。

`NeverDSBFSemanticTests` は検証済み命令バイトを直接実行し、MedIR を消費しません。このため、正規化 IR の変更や破損によって source oracle と backend が偶然一致することはありません。非単調な v2 セマンティクス、メモリ、syscall、内部 call frame、fault、trace、resource limit を網羅します。LLVM module は検証され、生成 C は warning を error として、Rust は `-D warnings` 付きでコンパイルされます。公開 API テストは生成した厳格な SBF ELF から、全 IR 段階、逆アセンブル、CFG、メタデータ、LLVM、C、Rust を通過します。

## 一括ターゲット

カスタムターゲットは依存関係をビルドし、ホスト CPU から決めた並列度で CTest を
実行します。

| CMake ターゲット | 選択範囲 |
|------------------|----------|
| `check-neverd` | 登録済みの全テスト |
| `check-neverd-semantic` | `NeverDSemanticTests` のみ |
| `check-neverd-sbf` | すべての `NeverDSBF*Tests` ターゲット/ケース |
| `check-neverd-patch-full` | `NeverDPatchFullTests` のみ |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests` のみ |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests` のみ |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests` のみ |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` と `NeverDAvxUpperXformTests` には現在
`check-neverd-*` の便宜ターゲットがありません。下記のとおりビルドしてラベルを
選択してください。`check-neverd-semantic` にも独立した変換や patch-full の
バイナリは含まれません。完全な集約には `check-neverd` を使います。

## 増分 CTest ワークフロー

所有する実行ファイルを先にビルドしてからラベルを選択します。無関係な大規模
セマンティックターゲットの再リンクを避けられます。

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# すべての重点 EVM ターゲット/ケース
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# すべての重点 Solana SBF ターゲット/ケース
cmake --build build-release --target check-neverd-sbf --parallel 4
```

GoogleTest 由来の CTest 名を使って単一の回帰を実行します。

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

便利なセレクター：

| コマンド | 目的 |
|----------|------|
| `ctest --test-dir build-release -N` | 検出ケースを実行せず一覧表示 |
| `ctest --test-dir build-release -L '<regex>'` | テストバイナリのラベルを選択 |
| `ctest --test-dir build-release -R '<regex>'` | ケース名を選択 |
| `ctest --test-dir build-release --output-on-failure` | 失敗時だけ診断を表示 |
| `ctest --test-dir build-release --stop-on-failure` | 最初の失敗で停止 |
| `ctest --test-dir build-release --parallel 4` | 最大 4 ケースを並列実行 |

GoogleTest の検出は `DISCOVERY_MODE PRE_TEST` を使うため、CTest が列挙する前に
対応するテストバイナリが必要です。ケースごとの timeout と独立した検出 timeout は
`cmake/AddNeverD.cmake` に定義され、実測で重いケースがあるスイートだけ拡大できます。

## コード変更に伴うテスト

| 変更領域 | 最初に実行 | 次に検討 |
|----------|------------|----------|
| アーキテクチャ lifter または decode | `NeverDLiftTests` の名前付きケース | 対応 ISA のセマンティックラウンドトリップ |
| LowIR CFG、関数検出、ジャンプテーブル | Lift CFG/switch ケース | `NeverDSwitchXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests` |
| MedIR、ABI、フラグ、型、SSA | MedIR/呼出規約 lift ケース | ISA 横断の `NeverDSemanticTests` ケース |
| HighIR または構造化 C | HighIR/decompile ケース | `NeverDCFGLoopXformTests` と生成 C のコンパイル検査 |
| PE/ELF/Mach-O loader または入力 relocation | 対応する `unittests/lift` の形式 fixture | そのセルの全段階読込み/デコンパイルテスト |
| Rewrite codegen または出力 relocation | `RewriteCodegenRTTests` ケース | `NeverDPatchFullTests` と利用可能なリンク済み patch fixture |
| patch で使う LLVM IR 変換 | 重点変換バイナリ | `NeverDPatchFullTests` の合成 pass グリッド |
| C API または CLI | 直接 SDK/query テストと `unittests/semantic/CLIEndToEndTests.cpp` | 関連 pipeline/形式スイート |
| EVM loader、opcode、IR、backend | 所有する最小の `NeverDEVM*Tests` ターゲット | 全 EVM ターゲットと生成 C/Solidity のコンパイル |
| SBF loader、ISA、IR、backend | 所有する最小の `NeverDSBF*Tests` ターゲット | 全 SBF ターゲットと生成 C/Rust のコンパイル |
| Libc 認識 | `NeverDLibCTests` | 動作変更時のセマンティック call/ABI ケース |
| ヒープ寿命監査またはコピー越境ハント | `NeverDSafetyTests` | `NeverDSafetyIntegrationTests` の全 6 セル |
| プロセス実行または quoting | `NeverDTestProcessTests` | 対応各ホストの影響を受ける CLI/セマンティックケース 1 件 |

テストは最も低い安定した境界で契約を表現してください。LowIR 形状テストは lifter
への帰属に有用です。妥当に見える 2 つの IR 形状が異なる動作をし得る場合は
セマンティックラウンドトリップが必要です。小さな opcode、CFG、観測状態の assertion
で十分なら関数全体の golden dump は避けてください。

## CI との関係

CI は Linux、macOS、Windows でテストを有効にした Release をビルドし、検出した
一覧を監査してからプラットフォーム固有のラベル除外を適用します。プロファイルは
`.github/workflows/ci.yml` と `scripts/audit_ci_test_inventory.py` にあります。
`NeverDSafetyTests` と `NeverDSafetyIntegrationTests` はすべての matrix ホストで
必須であり、各実行は同じチェックイン済み PE、ELF、Mach-O × x86-64、AArch64 fixture を読みます。高コストスイートのすべてを表す単一 matrix shard はないため、必要なクロスツールが揃うマシンではローカルの `check-neverd` が最も明確な完全マージ前シグナルです。

## 現在の Solana SBF conformance / sanitizer profile

この current list は上の短い SBF list を置き換えます。source differential suite は
clang に加えて `rustc` が必要で、compiler skip は coverage 欠落です。完全な aggregate
には `NeverDSBFProgramImageTests`、`NeverDSBFMalformedCorpusTests`、
`NeverDSBFISAConformanceTests`、`NeverDSBFUpstreamConformanceTests`、
`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests` と、metadata、
loader、analyzer、semantic、emitter、integration target が含まれます。integrated
profile は変動する総数ではなく、named target と結果を記録します。

sanitizer profile は `build-sbf-asan-ubsan` に分離して build します。revision を固定した
prebuilt package は必要な fork-only header を含むため、integration も同じ fail-fast
ASan/UBSan profile で実行します。

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

### pinned SBF evidence snapshot（2026-08-24）

gate は Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`、Agave
`ef210d67f2fabeee1730498188fa78854260c679`、Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd` を固定します。official ELF manifest
は 23/23 を通過し、`NeverDSBFExternalOracleTests` は 1,411 opcode/boundary
case を `SBFOfficialOracleProtocol.def`、`SBFOfficialVerifierCases.def`、
`SBFOfficialExecutionConstants.def` 経由で
照合します。`SBFOfficialELFMutations.def` が malformed ELF の table-driven
contract であり、変動する総数は固定しません。
別軸の `41-case strict ELF differential` は strict-v3 matrix 全体を official
`verify-elf-batch` と NeverD に通します。この 41 case は 1,411 total に含みません。
`NeverDSBFAgaveConformanceTests` は Firedancer test-vectors の
`68bb4af40235562e8852fa23d5727e49c2a0b862` を認証し、loader fixture 1,955 `sol_compat_elf_loader_v1` 個
（accept 1,399、reject 556）を照合し、accept された各 ELF について `entry_pc`、`text_off`、
`text_cnt`、`rodata_hash`、`calldests_hash` を比較します。この gate は後段の instruction verifier を実行しません。

追加の official execution matrix は別枠です。active `(Version,Opcode)` case が
正確に 508、boundary case が 58、合計 566 の exact execution case です。1,411 の
verifier probe や `41-case strict ELF differential` を置き換えず、その総数にも含みません。
Linux Release CI は `--print-pinned-revision`、`--print-test-vectors-revision`、
`--print-toolchain` を使い、`NEVERD_SBPF_ORACLE` と
`NEVERD_AGAVE_CONFORMANCE_ROOT` を export するため両 external gate は必須です。
明示 oracle/corpus env がない local run は case を discover しますが skip できます。

`SBF_RUNTIME_VERSION` により `RuntimeVersionPolicy::ChainProfile` は historical
cluster/slot を反映し、official feature account activation に従って maximum ISA を
V0→V1→V2→V3 と進めます。現在は V3 です。明示 v4 は offline 分析用の
`RuntimeVersionPolicy::UpstreamToolchain` を使います。
現在の 10 MiB 上限は正確に `10'485'760` byte、65,536 は historical
provenance/test のみです。`SBFFaultCodes.def` は execution fault の安定値、
`SBFSourceStatuses.def` は別レイヤーの generated-source ABI を持ちます。

10,000 scale fixture が worklist、function ownership、multi-latch を守り、
machine 固有時間は固定しません。cluster/account/slot row は通常 test を
deterministic/offline に保ったまま `RPC activation audit` を可能にします。
