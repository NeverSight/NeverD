**Languages**: [English](../../CONTRIBUTING.md) | [简体中文](../zh-CN/CONTRIBUTING.md) | [繁體中文](../zh-TW/CONTRIBUTING.md) | [日本語](CONTRIBUTING.md) | [한국어](../ko/CONTRIBUTING.md) | [Français](../fr/CONTRIBUTING.md) | [Deutsch](../de/CONTRIBUTING.md) | [Español](../es/CONTRIBUTING.md) | [Italiano](../it/CONTRIBUTING.md) | [Русский](../ru/CONTRIBUTING.md) | [العربية](../ar/CONTRIBUTING.md)

<!-- i18n-source: af6c34901bacab00e5d1ed68b031f0a48a3ff64ac85ee208695647ec935b9790 -->

# NeverD への貢献

NeverD はセマンティクスを最優先するバイナリ解析プロジェクトです。有用な貢献は
焦点が明確で、未対応の動作を明示的に失敗させ、変更した契約を証明する最小限の
テストを含みます。

既存のコードを取り込む際は、AI/LLM ツールで複製・改変したコードも含め、
著作権、ライセンス、出典の表示を保持してください。
[帰属表示と引用のガイド](ATTRIBUTION.md)に従い、再利用した成果物の元の出典と
リビジョンを明記し、提出前にライセンスを確認してください。
第三者に関する表示は、その元のライセンスに従って保持してください。

編集前に[アーキテクチャガイド](architecture.md)を読んでください。
スイートの選択には[テストガイド](testing.md)、製品計画には
[ロードマップ](roadmap.md)を参照してください。

<!-- i18n-section: prerequisites -->
## 前提条件

- 再帰的サブモジュールを扱える Git
- CMake 3.20 以降
- Ninja
- C++20 対応コンパイラ
- Python プラグイン有効時は CPython 3.10+ と埋め込み用開発ファイル
- 完全なクロスターゲット fixture セットに必要な Clang と LLD
  （`ld.lld` および `lld-link`）

再帰的サブモジュールには NeverD の LLVM fork、Capstone fork、Unicorn、
シグネチャデータが含まれます。変更を検証するときに、任意のシステム版へ
置き換えないでください。

<!-- i18n-section: clone -->
## クローンと初期化

開発成果はリポジトリのデフォルトブランチでもある `dev` に統合されます。
すべてのサブモジュールを含めてクローンします。

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

既存のクローンでは、初回ビルド前と、記録されたサブモジュールのリビジョンを
変更するコミットの後に同期してください。

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

<!-- i18n-section: build -->
## ビルドプロファイルの選択

| プロファイル | 用途 | 重要な動作 |
|--------------|------|------------|
| Release | 通常の開発、全テスト、デコード/リフトのベンチマーク | 最適化済み。代表的なスループット |
| RelWithDebInfo | 最適化されたホットパスのプロファイルまたはデバッグ | 最適化済みでデバッグシンボル付き |
| Debug | アサーション、ソースレベルのステップ実行、局所的な正しさの確認 | 未最適化。デコードベンチマークは意図的に大幅低速 |

タスクが Debug 固有の動作を必要としない限り Release を使用します。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
python scripts/build.py build-release --parallel
```

デフォルトでは `third_party/llvm-project` を統合依存関係としてビルドします。
初回ビルドには通常 30～60 分かかり、以後は増分ビルドです。
`CMakePresets.json` にも `release`、`relwithdebinfo`、`debug` の構成/ビルド
プリセットがありますが、上ではテスト設定を明示するため個別のビルドディレクトリを
使っています。

ソースレベルでデバッグする場合は Release ツリーを再構成せず、別ディレクトリを
使用します。

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
python scripts/build.py build-debug --parallel
```

Debug ビルドのデコードまたはリフトのスループットを報告しないでください。
ベンチマークには Release、シンボルが必要なプロファイルには RelWithDebInfo を
使用します。

<!-- i18n-section: allocator -->
### プロセスアロケータ

Linux、macOS、Windows MSVC/clang-cl では `NEVERD_ENABLE_MIMALLOC` が既定で有効です。
共通 CMake ヘルパーで作るツール、解析 worker、テストは NeverC のホスト方式に従い、
NeverSight mimalloc fork の既定ブランチを使います。Unix は実行ファイルごとに一度組み込み、
Windows は mimalloc DLL と同じアーキテクチャの `mimalloc-redirect*.dll` で動的 CRT の
割り当て（libneverd、ネイティブプラグインを含む）をリダイレクトします。両 DLL は実行ファイル横と
インストール先 `bin` に配置されるため、一緒に配布してください。Windows は `/MD` または `/MDd`
が必要で、静的 CRT では無効にします。SDK 自体は埋め込み先にアロケータを強制しません。
Windows ARM64 はネイティブ ARM64 ビルドを使用します。x64 DLL は x64 エミュレーションの
ARM64EC CRT をリダイレクトできません。MinGW はシステムアロケータを維持します。
[上流の Windows 要件](https://github.com/NeverSight/mimalloc/blob/main3/bin/readme.md)を参照してください。

システム基準や外部アロケータには `-DNEVERD_ENABLE_MIMALLOC=OFF` を使います。
旧 `NEVERD_WORKER_MIMALLOC=OFF` は worker のみ無効にします。CMake は address、thread、
memory、leak sanitizer で自動抑止し、複合フラグや構成別のコンパイラ/リンカーフラグにも対応します。
UBSan 単独は互換です。ターゲット固有のフラグだけで sanitizer を指定する場合も OFF にします。
Windows は `MIMALLOC_DISABLE_REDIRECT=1` で再ビルドなしの比較ができ、
`MIMALLOC_VERBOSE=1` で有効化を確認できます。

正式な入口 `python scripts/build.py BUILD_DIR` は毎回、5 つの NeverSight fork をリモートの
既定ブランチから更新します。これらも Capstone、Unicorn と同じ `third_party` の Git サブモジュールです。
スクリプトは実際のブランチを解決し、5 つを更新してコミット済みソースを
`BUILD_DIR/third_party/<library>/<commit>/src` に書き出します。未コミット/分岐したローカル変更や
ネットワーク障害はビルドを停止し、変更を上書きしません。更新、再構成、コンパイルを通じてビルド
ディレクトリをロックし、次の呼び出しまで同じスナップショットを保持します。増分ビルドも新コミットを取り込みます。

`neverd-dependency-inputs.json` は構成入力、コミット、エクスポートの SHA-256 を記録し、
インストール製品に付属しますが、バイナリの認証ではありません。`neverd-build-receipt.json` は
要求ターゲット/構成のみの成否を記録し、CI は両方を保持します。インストールは失敗、入力変更、
未ビルド構成を拒否し、製品 CI は全インストール対象をビルドしてからインストールを検証します。
主流ブランチは実行間に変わり得るため、複数プラットフォームを一括評価する前にコミットを比較します。
直接の `cmake --build` は鮮度を確認し古いスナップショットを拒否します。更新は Python 入口を使い、
その自動再生成は同じスナップショットを維持します。`FETCHCONTENT_SOURCE_DIR_NEVERD_*` は隔離した
ローカル fixture 用で、主流ビルド入口では拒否されます。複数構成ジェネレーターは `--config` が必須です。
入口でコンパイルし、テストと `cmake --install` は別途実行します。直接の `install` ターゲットはコンパイル前に拒否されます。

スモークテスト/性能比較は[アロケータ検証](../testing.md#allocator-validation)を参照してください。

<!-- i18n-section: acceleration -->
### 高速化ライブラリ

通常は Highway バイト走査、simdutf UTF-8 検証、simdjson トランスポート解析、oneTBB
パイプラインスケジューリングを有効にし、プロセスアロケータを維持します。JSON の nlohmann DOM/
シリアライズは維持し、有効な整数のみのフレームは SIMD tape からコピーします。小数/負のゼロは
従来の数値変換を使い、simdjson 範囲外の入力も従来の受け入れ経路を使います。不正な外部テキストの
バイトを保存するエスケープも維持します。これらと mimalloc は `third_party/forks.json` の
NeverSight fork 由来で、互換修正は各既定ブランチに置きます。ビルドは現行ライセンスをコピーし
正確なコミットを記録します。5 依存を古いリリースに固定しません。

`NEVERD_ENABLE_HIGHWAY`、`NEVERD_ENABLE_SIMDUTF`、`NEVERD_ENABLE_SIMDJSON`、
`NEVERD_ENABLE_TBB` は既定 `ON`、比較時は個別に `OFF` にできます。oneTBB はバッチ間で
worker を保持し、8 MiB スタックを確認します。`tbbmalloc` やその proxy はビルド/ロードしません。
大きなスタックの呼び出し元は余裕を再利用し、小さい場合はネイティブ調整スレッドを使います。
初回使用後に fork するホスト、SDK を動的アンロードするホストはネイティブスレッド基準
`NEVERD_ENABLE_TBB=OFF` が必須です。保持型 scheduler はこれらのライフサイクルをサポートしません。
oneTBB は共有コアに静的リンクされ、そのシンボルはコア内に限定されます。CLI からのコンポーネント呼び出しと
GUI worker への要求は同じ scheduler を使用し、TBB DLL/共有ライブラリは配布しません。
`neverd_shared` をリンクするターゲットは共有 scheduler を自動的に選択します。別のターゲットを経由して
SDK を使用し、コンポーネントのアーカイブもリンクするクライアントは、最終ターゲットに
`neverd_use_shared_task_scheduler` を適用する必要があります。独立したコンポーネントツールは
独自のランタイムを維持します。
この限定的な統合により、同一プロセス内で複数の NeverD scheduler が動作することを防ぎます。
上流では引き続き [oneTBB の静的ビルド](https://uxlfoundation.github.io/oneTBB/main/intro/static_linking.html)
を非サポートとし、一般的な用途での使用を推奨していません。

ネイティブ検証、無効化比較、互換範囲は[高速化検証](../testing.md#acceleration-libraries)を参照してください。

<!-- i18n-section: prebuilt-llvm -->
### macOS のプリビルド LLVM

Apple Silicon のコントリビューターは LLVM fork のローカルビルドを省略できます。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
python scripts/build.py build-release --parallel
```

CMake はリポジトリで設定されたリリースパッケージをダウンロードし、SHA-256 を
検証して、以後のビルドで展開済みユーザーキャッシュを再利用します。プリビルド
チャネルは macOS arm64 のみをサポートします。Intel Mac とユニバーサルビルドは
デフォルトのローカル LLVM ビルドを使用してください。`NEVERD_LLVM_PREBUILT_TAG`、
ミラー URL、キャッシュディレクトリ、明示的なチェックサムなどの高度な上書きは
`cmake/NeverDLLVMPrebuilt.cmake` に記載されています。

LLVM の修正は [NeverSight LLVM fork](https://github.com/NeverSight/llvm-project) で管理します。
まず fork にコミットしてマージし、NeverD の LLVM サブモジュールのリビジョンを更新します。
統合ビルドはそのソースを直接コンパイルします。NeverD に LLVM パッチファイルやビルドディレクトリのソース上書きを追加しないでください。
同じ動作を検証するには、修正済み fork からプリビルドパッケージを再構築する必要があります。

<!-- i18n-section: workflow -->
## ブランチとプルリクエストの流れ

最新の `dev` から、目的を絞ったトピックブランチを作成します。

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

プルリクエストの宛先は想定上のリリースブランチではなく `dev` です。コミットを
レビューしやすく保ちます。目的は一つに絞り、生成されたビルド出力や無関係な
整形を含めず、提案の一部でない限りサブモジュールのリビジョンを変更しません。

<!-- i18n-section: code-style -->
## コードスタイル

C/C++ は [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html) に従い、
[`.clang-format`](../../.clang-format) を書式、[`.editorconfig`](../../.editorconfig) を
文字コード/改行/空白の基準とします。C++ バージョンは上流文書ではなく `CMakeLists.txt` に従います。
変更ファイルを整形します：

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

CI のバージョンは [`scripts/check_clang_format.py`](../../scripts/check_clang_format.py) に固定されます。
再現には同じ版を使います。整形ではヘッダーの責任範囲やポリシーの直書き数値を検証できません。
局所修正で全体を整形せず、周囲の命名/分割に従い、プラットフォーム動作は loader/lifter/backend
境界に置きます。純 C SDK に内部 C++ 型を公開しません。

<!-- i18n-section: headers -->
### ファイルとヘッダーの構成

- 新規 C++ ファイルは LLVM のファイル名/目的バナー、`NeverD Decompiler`、Doxygen `\file`
  で始めます。`lib/debug/PDBLoader.cpp` を参照し、著作権/ライセンス表示を保持します。
- 自己完結した `.h` に依存を明示し、実装は対応インターフェースを最初に include します。
  残りは `.clang-format` 順（隣接非公開、公開 `neverd/`、引用符 LLVM/Capstone、山括弧）。
  偶然の順序や推移的 include に頼りません。
- 新規ヘッダーはパス由来の `NEVERD_..._H` guard を使います。`neverd/sdk/Limits.h` は
  `NEVERD_SDK_LIMITS_H` です。非公開/ツールのディレクトリを十分含め衝突を防ぎ、`#endif`
  に名前をコメントします。既存ヘッダーの明示的統一時に適用し、無関係なものは変更しません。
- 共有インターフェースは `include/neverd/<component>/`、非公開ヘッダーは `lib/` または
  `tools/` のソース横です。場所は安定 C SDK API を意味しません。依存は非循環とし、コアモデル/
  reader をセッション/トランスポートから独立させます。アルゴリズムは所属先に置き、CLI/SDK/worker
  に意味処理を重複させません。[アーキテクチャ](architecture.md)を参照してください。
- 所属名前空間と明確な名前を使用します。新規型/フィールドは `UpperCamelCase`、関数は
  `lowerCamelCase`、ポリシー定数は `kUpperCamelCase`。既存インターフェース、公開 C/Python
  契約、STL 慣習の綴りを維持し、ヘッダーに名前空間全体の `using namespace` を加えません。
- 意図的な複数 include 表は `.def`/`.inc` とマクロ契約を維持します。独立 `.h` ではなく、
  再展開を阻む guard を付けません。生成ヘッダーは生成元を変更します。

独立したポリシーヘッダーの例：

```cpp
//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstdint>

namespace neverd::sdk {

/// Traversal ceiling for a function without a recorded instruction extent.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
```

<!-- i18n-section: policy -->
### 上限、既定値、設定の責任範囲

インクルード検索パスに直接追加するツールディレクトリでは `ResourceLimits.h` を使用します。大文字小文字を区別しないファイルシステムでは、ローカルの `Limits.h` がシステムの `<limits.h>` を隠す可能性があります。

ポリシーは呼び出し元が一つでも、所属サブシステムの limits/configuration ヘッダーを唯一の定義元に
します。I/O、再帰/グラフ、作業/反復予算、ヒューリスティック閾値、キャッシュ、ページ/バッチ、再試行、
タイムアウトが対象です。`constexpr`、マクロ、直書き比較、`min`/`clamp`、初期化子、既定引数も含みます。

まず既存の定義元を選びます：

| 所属 | 定義場所 |
| --- | --- |
| 共有コア/パイプライン | [`include/neverd/Limits.h`](../../include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](../../include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](../../include/neverd/web/Limits.h) または既存の専用定義 |
| その他のライブラリ | 既存ヘッダー/定義表、なければ所属層の専用 `Limits.h` |
| CLI、worker、GUI のみ | ツール内 `ResourceLimits.h`/`Config.h`。コアは依存しない |
| 呼び出し元の選択設定 | 所属 `Config.h`、`*Options.h`、`*Configuration.h` モデル |

軽量ヘッダーで目的別にまとめ、既存 `*Limits.def`/設定表を再利用し自己完結 C++ ビューを公開します。
`Limits.h`、`Constants.h`、`Config.h` で同じ定義を競合させません。コア方針を SDK/ツールに置かず、
無関係な全モジュールをルート `Limits.h` に集めません。

C++ スカラーは型付き `inline constexpr` と既存名前空間（`neverd::limits`、`neverd::sdk`）を使います。
`kMaxSourceBytes`、`kDefaultPageRows`、`kRetryDelayMilliseconds` のように目的/単位を明示し、
ゼロ/番兵や特殊な境界を説明します。異なる意味の既定値とハード上限は分離します。Options モデルが
既定値を持つ場合、利用側はモデル/既定ファクトリー/共通定数を使い、関連値は共通定義から導出します。

アダプター、永続化検証、CLI/GUI/worker、Python/MCP、schema 上限や数値診断は同じ定義元を使用します。
言語間契約は定義表/生成 binding から導出し、困難なら drift 検査を加えます。純 C ヘッダー互換を維持し、
厳しいプレビュー/ページ制限には別名と所属を付けます。値の一致だけで同じポリシーとは判断しません。

バイナリ配置、ABI/プロトコル/命令定数、整数範囲、証明アルゴリズム固有の課金は意味モデルに置きます。
状態初期化、添字、割り当てヒント、独立テスト入力は自動的に対象とはなりません。独立した期待境界を、
検証対象の実装定数に置き換えません。

宣言だけでなく複数行 guard、ループ、マクロ、既定値、タイマー、binding を含め直書きも確認します。
触れる各方針の意味/所属を調べ、`Max*` 検索だけで済ませません。集約では型、値、比較方向、番兵、
枯渇/エラー動作を保持し、方針変更は分けて[該当テスト](testing.md)を実行します。

<!-- i18n-section: artifacts -->
### 文書とローカル成果物

Markdown は簡潔でソース検証可能にし、内部リンクは相対形式にします。CLI/API、対応範囲、ビルドフラグ、
テスト命令変更は同じ PR で記載します。本ガイドの全対応言語を同期し、訳を確認してから `i18n-source`
SHA-256 を更新します。章、例、API/オプション、リンク先を揃え、`python3 scripts/check_docs_i18n.py` を実行します。

マシン固有の性能報告、生トレース、時間集計は無視対象の build/benchmark-results に置きます。
再利用可能な計測ツール/回帰テストをコミットし、ローカル報告や結果表のコピーは含めません。

<!-- i18n-section: tests -->
## テストの実行

集約ターゲットで登録済みの全テストを実行します。

```bash
python scripts/build.py build-release --parallel
cmake --build build-release --target check-neverd
```

開発中は最小の関連ターゲットまたは CTest ラベルを使用します。

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
python scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

[テストガイド](testing.md)には、すべての便宜ターゲット、ラベルのみの
変換スイート、単一テスト正規表現、fixture のコンパイル、Unicorn ラウンドトリップが
記載されています。クロスコンパイラやリンカがなくターゲットがスキップされた場合は、
その制約を報告し、未実行の経路を合格と説明しないでください。

<!-- i18n-section: python-sdk -->
### Python プラグイン SDK

SDK 変更時は純 Python/drift suite の後にネイティブアダプターテストを実行します：

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
python scripts/build.py build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

公開 C 宣言、Python ABI 署名、package/CMake バージョン、配布 workflow 方針は一つの審査対象契約です。
同じ変更で更新します。[Python プラグインガイド](python-plugins.md)を参照してください。

<!-- i18n-section: review -->
## プルリクエストのチェックリスト

レビューを依頼する前に：

- メンテナーが希望する手順で最新の `dev` を rebase または merge し、
  サブモジュールの変更を意図的に解決する。
- 影響するターゲットを Release でビルドする。別プロファイルが必要なら理由を示す。
- 狭い回帰テストと、実行可能な範囲で最も広い関連スイートを実行し、正確なコマンドと
  スキップを PR の説明に記載する。
- strict lifting を維持する。未対応命令を推測した演算や `NOP` へ暗黙変換しない。
- 動作変更にはテキスト IR スナップショットだけでなく、セマンティックカバレッジを追加する。
- 無関係な整理、生成ファイル、ローカルビルド成果物を差分に含めない。
- [ヘッダーとポリシー所属規則](#コードスタイル)、直書き数値 guard/既定値、binding drift を確認する。
  整形成功だけでは不十分。
- 動作、サポート、フラグ、コマンド、テスト所有範囲が変わる場合は公開文書と
  コントリビューター文書を更新する。

公開プルリクエストとして開始すべきでないセキュリティ上の報告は、
[SECURITY.md](../../SECURITY.md)に従ってください。
