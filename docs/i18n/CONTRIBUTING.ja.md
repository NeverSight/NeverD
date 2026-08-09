**Languages**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md) | [日本語](CONTRIBUTING.ja.md) | [한국어](CONTRIBUTING.ko.md) | [Français](CONTRIBUTING.fr.md) | [Deutsch](CONTRIBUTING.de.md) | [Español](CONTRIBUTING.es.md) | [Italiano](CONTRIBUTING.it.md) | [Русский](CONTRIBUTING.ru.md) | [العربية](CONTRIBUTING.ar.md)

# NeverD への貢献

NeverD はセマンティクスを最優先するバイナリ解析プロジェクトです。有用な貢献は
焦点が明確で、未対応の動作を明示的に失敗させ、変更した契約を証明する最小限の
テストを含みます。

編集前に[アーキテクチャガイド](../architecture.ja.md)を読んでください。
スイートの選択には[テストガイド](../testing.ja.md)、製品計画には
[ロードマップ](../roadmap/README.ja.md)を参照してください。

## 前提条件

- 再帰的サブモジュールを扱える Git
- CMake 3.20 以降
- Ninja
- C++20 対応コンパイラ
- 完全なクロスターゲット fixture セットに必要な Clang と LLD
  （`ld.lld` および `lld-link`）

再帰的サブモジュールには NeverD の LLVM fork、Capstone fork、Unicorn、
シグネチャデータが含まれます。変更を検証するときに、任意のシステム版へ
置き換えないでください。

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
cmake --build build-release --parallel
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
cmake --build build-debug --parallel
```

Debug ビルドのデコードまたはリフトのスループットを報告しないでください。
ベンチマークには Release、シンボルが必要なプロファイルには RelWithDebInfo を
使用します。

### macOS のプリビルド LLVM

Apple Silicon のコントリビューターは LLVM fork のローカルビルドを省略できます。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake はリポジトリで設定されたリリースパッケージをダウンロードし、SHA-256 を
検証して、以後のビルドで展開済みユーザーキャッシュを再利用します。プリビルド
チャネルは macOS arm64 のみをサポートします。Intel Mac とユニバーサルビルドは
デフォルトのローカル LLVM ビルドを使用してください。`NEVERD_LLVM_PREBUILT_TAG`、
ミラー URL、キャッシュディレクトリ、明示的なチェックサムなどの高度な上書きは
`cmake/NeverDLLVMPrebuilt.cmake` に記載されています。

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

## コードスタイル

C と C++ は LLVM のコーディング規約に従い、`.clang-format` をリポジトリの
整形基準とします。変更したファイルだけを整形してください。

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

局所的な修正のためにリポジトリ全体を再整形しないでください。周囲のファイルの
命名と分割パターンに従い、プラットフォーム固有の動作を該当する loader、lifter、
backend の境界に置き、純粋 C SDK から内部 C++ 型を公開しないでください。

Markdown は簡潔で、ソースから検証できる内容にします。リポジトリ内のファイルには
相対リンクを使い、CLI の動作、公開 API、サポート表明、ビルドフラグ、テスト
コマンドが変わる場合は同じプルリクエストで文書も更新します。

## テストの実行

集約ターゲットで登録済みの全テストを実行します。

```bash
cmake --build build-release --target check-neverd
```

開発中は最小の関連ターゲットまたは CTest ラベルを使用します。

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

[テストガイド](../testing.ja.md)には、すべての便宜ターゲット、ラベルのみの
変換スイート、単一テスト正規表現、fixture のコンパイル、Unicorn ラウンドトリップが
記載されています。クロスコンパイラやリンカがなくターゲットがスキップされた場合は、
その制約を報告し、未実行の経路を合格と説明しないでください。

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
- 動作、サポート、フラグ、コマンド、テスト所有範囲が変わる場合は公開文書と
  コントリビューター文書を更新する。

公開プルリクエストとして開始すべきでないセキュリティ上の報告は、
[SECURITY.md](../../SECURITY.md)に従ってください。
