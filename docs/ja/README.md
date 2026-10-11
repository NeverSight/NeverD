**Languages**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 6241c9252ec89317b9c488c7e2b6b8c7bd60329dfdd734df27e1a7a7139b1eff -->

[← NeverD プロジェクト](project.md)

# NeverD ドキュメント

プロジェクト概要・ビルド・CLI はリポジトリ README にあります。コントリビューター向けの設計・テスト資料をここにまとめています。

**モバイル対応（実験的 CLI）：** `neverd mobile` は [Android](android.md) の APK、DEX、smali から Java を、[iOS](ios.md) の IPA、`.app`、Mach-O からネイティブ C と対応する Objective-C/Swift ソースを復元します。JSON レポートは結果とカバレッジを示します。[モバイル概要](mobile.md)から始め、各プラットフォームのガイドでコマンドと制約を確認してください。

英語のガイドは `docs/` 直下にあります。翻訳は `ar/`、`de/`、`es/`、`fr/`、`it/`、`ja/`、`ko/`、`ru/`、`zh-CN/`、`zh-TW/` に分かれています。各言語のディレクトリにはドキュメント索引 `README.md`、プロジェクト概要 `project.md`、各ガイド、`CONTRIBUTING.md`、`ATTRIBUTION.md`、`roadmap.md` があります。共有画像は `assets/` にあります。

| 文書 | 説明 |
|------|------|
| [プロジェクト説明（日本語）](project.md) | 概要、クイックスタート、ビルド、SDK、CLI |
| [貢献ガイド](CONTRIBUTING.md) | 開発環境、ビルドプロファイル、ワークフロー、スタイル、PR 要件 |
| [アーキテクチャ](architecture.md) | IR 経路、コンポーネント境界、strict lifting、サポート深度、変更箇所 |
| [オフライン Web 解析（英語）](../web-analysis.md) | C++ による成果物・ソース・バインディング・マップの検査、メタデータ方針、SDK と現在の検証範囲 |
| [Bun 単体実行ファイルのプロファイル（英語）](../web-bun-profile.md) | 固定 ELF 抽出、元の証拠範囲、ソース復号と固定テスト素材の出所 |
| [ASAR 抽出プロファイル（英語）](../web-asar-profile.md) | 格納済み・外部ファイルの取得対応、整合性状態、メンバーの利用側とネイティブ Unicode 依存関係 |
| [Electron 証拠プロファイル（英語）](../web-electron-profile.md) | 取得した入口パス、マニフェストとソースの範囲、ウィンドウ・ブリッジの証拠と IPC チャネル候補 |
| [HTML ソースプロファイル（英語）](../web-html-profile.md) | 有界な C++ スクリプト一覧、取得したローカル参照と元のインラインソース位置 |
| [テスト](testing.md) | テストスイート、生成 fixture、Unicorn ラウンドトリップ、増分コマンド |
| [デスクトップワークベンチ (英語)](../gui.md) | 従来の逆アセンブラ風レイアウト、独立ワーカー、プロジェクトデータベース、多言語対応、MCP 接続 |
| [ライブラリ認識（英語）](../library-recognition.md) | 証拠に基づく STL、ATL/MFC、COM、libc の識別、プロファイル、元に戻せる C コードの折りたたみ |
| [デスクトップ検証記録 (英語)](../gui-qualification.md) | GUI の実測証拠、パッケージの境界、残るプラットフォーム検証 |
| [インタープリターのソース復元](interpreter-recovery.md) | 実験的な x64 インタープリター特化、HighC/LLVMC 出力、実行前提、証拠と制限; 入れ子ループの証明候補; 明示的な探索予算とバージョン付き C API; 正確なネイティブから LLVM への証明 API |
| [Windows 例外再構築](windows-exception-reconstruction.md) | SEH/C++ サポート表、IR 契約、ネイティブ patch 規則、PE 検証 |
| [CPU 実行とゲスト環境](emulation.md) | バックエンドの選択、ゲスト環境、ネイティブ検証、現在の制限 |
| [CPU 実行](cpu-execution.md) | 構成、機能照会、バックエンド可用性、型付き結果 |
| [Bitvector 証明バックエンド](solver.md) | オプションの Z3 証明、証明付き合成、独立検査、query export |
| [ゲストプロセスのエミュレーション](process-emulation.md) | Linux ELF プロファイル、起動、サービス、制限、テスト |
| [macOS/iOS プロセス環境](darwin-emulation.md) | Mach-O 起動、デバイスとシミュレータの区別、Darwin サービスとページ規則 |
| [macOS HVF](macos-hvf.md) | ホストと同じ ISA のハードウェア実行、署名権限、パッケージと検証 |
| [Windows ドライバーエミュレーション](driver-emulation.md) | 有界 x64 WDM/KMDF ライフサイクル、要求、ハードウェアシナリオ、SEH、PnP サブセット、バックエンド選択と制限 |
| [メモリ安全性の監査とハント](memory-safety.md) | ヒープ寿命とコピー越境解析：形式ごとの識別契約、シンク／ソースカタログ、判定、予算、JSON スキーマ |
| [ネイティブプラグイン](plugins.md) | 純粋 C descriptor ABI、callback と event、build/link workflow、discovery、互換性規則 |
| [Python プラグイン](python-plugins.md) | プラグイン作成、セッション／イベント API、分離、テスト、公開 |
| [モバイル対応の概要](mobile.md) | Android / iOS の入力、ソース出力、CLI の流れと制限 |
| [Android の Java 復元](android.md) | APK（multidex）・DEX・smali ファイル／ディレクトリ → Java。CLI 手順、実行環境、オプション、JSON レポート、エラー処理、検証の限界 |
| [iOS ソース復元](ios.md) | IPA・`.app`・Mach-O → ネイティブ C と対応する Objective-C / Swift ソース。入力選択、メソッド本体と配置、CLI/export、JSON カバレッジレポート、制限と実行検証 |
| [EVM 逆コンパイル](evm.md) | 入力、hardfork、段階 IR、C/LLVM host ABI、Solidity 復元、制限 |
| [Solana SBF 逆コンパイル](sbf.md) | SBF v0-v4、LLVM IR、C/Rust 出力、検証、既知の制限 |
| [ロードマップ](roadmap.md) | 状態：native format、EVM、Solana SBF を実装済み |
| 各言語のドキュメント | 上部の言語リンクから各言語の索引とプロジェクト概要を開けます |
| [パックされた実行ファイルのアンパック](unpack.md) | 有界なゲストプロセスの観測によるパックされた PE32+ イメージの復元：エントリの規則、再構築されたイメージ、識別、制限 |
