**Languages**: [English](../../ATTRIBUTION.md) | [简体中文](ATTRIBUTION.zh-CN.md) | [繁體中文](ATTRIBUTION.zh-TW.md) | [日本語](ATTRIBUTION.ja.md) | [한국어](ATTRIBUTION.ko.md) | [Français](ATTRIBUTION.fr.md) | [Deutsch](ATTRIBUTION.de.md) | [Español](ATTRIBUTION.es.md) | [Italiano](ATTRIBUTION.it.md) | [Русский](ATTRIBUTION.ru.md) | [العربية](ATTRIBUTION.ar.md)

# 帰属表示と引用

このページは[英語版ガイド](../../ATTRIBUTION.md)の翻訳です。[LICENSE](../../LICENSE) の条項が適用されます。

NeverD は **NeverD コントリビューター**によって開発されています。ソースリポジトリは
[NeverSight/NeverD](https://github.com/NeverSight/NeverD) です。

## コードを再利用する際のライセンス上の義務

NeverD の独自の成果物には
[GNU AGPL バージョン 3 のみ](../../LICENSE)が適用されます。ライセンスの対象となる
複製物や改変物を頒布する場合は、[NOTICE](../../NOTICE) のプロジェクトに関する表示を含め、
該当する著作権、ライセンス、保証に関する表示を保持してください。
個々の著作者に関する表示も保持してください。対象のソースコードを変更した場合は、
変更内容とその関連日付を明示する目立つ表示が必要です。

これらの義務は、対象となる成果物を手作業で再利用した場合、AI アシスタントや
大規模言語モデル（LLM）で複製・改変した場合、LLVM IR、コンパイル、逆コンパイル、
または別のプログラミング言語を通じて変換した場合にも適用されます。
名前、書式、言語、ツールを変えること自体で義務がなくなるわけではありません。
NeverD の成果物を再利用する際は、NeverD がその出典であることを明記してください。
AI モデルや LLVM だけを記載しても、その出典を示したことにはなりません。

ソースの頒布物および頒布するバイナリに対応するソースには、ライセンスの全文と
該当する表示を含めてください。バイナリやネットワークサービスについては、
AGPL 第 6 条と第 13 条の該当する規定にも従ってください。引用、リンク、謝辞だけでは、
AGPL が求めるライセンス、変更表示、ソースの入手可能性に関する要件を**代替できません**。

このガイドは既存のライセンスを説明するものであり、第 7 条に基づく制限や追加条項を
設けるものではありません。正式な条項は [LICENSE](../../LICENSE)、
特に第 0 条、第 2 条、第 4～6 条、第 13 条に記載されており、
[フリーソフトウェア財団](https://www.gnu.org/licenses/agpl-3.0.html)でも確認できます。

## 出典を追跡できるようにする

再利用した部分ごとに、元のファイルまたはシンボル、正確なコミットまたはリリース、
変更内容の簡単な説明を、コードの近くやプロジェクトの告知文に記録することを推奨します。
引用が常に同じソースを示すよう、完全なコミットハッシュを含む GitHub の固定リンクを
使用してください。この追加の出典情報は引用に関する推奨事項であり、
追加のライセンス条件ではありません。

例えば、角括弧内の項目を実際の出典情報に置き換えてください。

```text
This project includes material from NeverD.
Copyright (C) 2026 NeverD contributors (https://github.com/NeverSight/NeverD)
License: GNU Affero General Public License, version 3 only (AGPL-3.0-only).
Original source: https://github.com/NeverSight/NeverD/blob/[full-commit]/[path]
Changes: [description], [YYYY-MM-DD].
The applicable license and notices are included with this distribution.
```

AI を利用するワークフローでは、選択したソースのコンテキストとともに出典情報を保持し、
公開する対象コードにも引き継いでください。共有する前に、得られたコードとその表示を
確認してください。ライセンスの対象となる NeverD のソースを含むデータセットについては、
そのソースを頒布する際に、該当する表示とライセンス情報を保持してください。

## 研究、参考利用、出力

NeverD を使用する、またはその実装を参考にする論文、文書、ベンチマーク、プロジェクトでは、
NeverD を引用してください。[CITATION.cff](../../CITATION.cff) には機械可読のソフトウェア
引用メタデータが含まれています。以下のプレーンテキスト形式の引用も、
実際に使用したバージョンまたはコミットを記載して利用できます。

```text
NeverD contributors. NeverD: Binary analysis and decompilation engine.
https://github.com/NeverSight/NeverD. Version or commit: [revision used].
```

単にアイデアやアルゴリズムを研究しただけでは、独立した実装に NeverD のライセンスが
自動的に適用されるわけではありません。同様に、他者のプログラムに NeverD を実行しても、
その出力に AGPL が自動的に適用されるわけではありません。第 2 条により、出力が対象に
なるのは、その内容がライセンスの対象となる著作物を構成する場合だけです。
この区別は AI の出力にも当てはまります。NeverD を学習に使用したり読み込んだりしても、
モデルのすべての出力が自動的に対象著作物になるわけではありません。
対象となる成果物を含まないこうした利用については、学術・工学上の慣行として引用を
お願いするものであり、新たなライセンス条件として課すものではありません。

## 第三者の成果物と以前の複製物

LLVM、Capstone、Unicorn などのコンポーネントには、それぞれ独自のライセンスが適用されます。
各ソース内の表示、[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)、および
[テストコーパスのライセンス](https://github.com/NeverSight/testbins/blob/9d9362d2cdfe0b4b0347bd0e12b3b8fac67d3a5b/LICENSE)を含む
ディレクトリ固有のライセンスを確認してください。これらの成果物を再利用する際は、
第三者に対する元の帰属表示を保持し、それぞれのライセンスに従ってください。
このガイドは、第三者の成果物のライセンスを変更したり、以前の複製物について
すでに与えられた許諾を取り消したりするものではありません。
