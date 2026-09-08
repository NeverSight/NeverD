**語言**: [English](../../ATTRIBUTION.md) | [简体中文](../zh-CN/ATTRIBUTION.md) | [繁體中文](ATTRIBUTION.md) | [日本語](../ja/ATTRIBUTION.md) | [한국어](../ko/ATTRIBUTION.md) | [Français](../fr/ATTRIBUTION.md) | [Deutsch](../de/ATTRIBUTION.md) | [Español](../es/ATTRIBUTION.md) | [Italiano](../it/ATTRIBUTION.md) | [Русский](../ru/ATTRIBUTION.md) | [العربية](../ar/ATTRIBUTION.md)

# 署名與引用

本頁是[英文指南](../../ATTRIBUTION.md)的譯文；具體條款以 [LICENSE](../../LICENSE) 為準。

NeverD 由 **NeverD 貢獻者**開發。原始碼儲存庫為
[NeverSight/NeverD](https://github.com/NeverSight/NeverD)。

## 重用程式碼時的授權義務

NeverD 的原創內容採用
[GNU AGPL 第 3 版（僅限此版本）](../../LICENSE)。散布受授權條款規範的副本或
改作版本時，須保留適用的著作權、授權與免責聲明，包括 [NOTICE](../../NOTICE)
中的專案聲明。任何個別作者聲明也須保留。修改後受授權條款規範的原始碼必須附有
醒目的聲明，說明修改內容及其相關日期。

這些義務適用於手動重用、藉助 AI 助手或大型語言模型（LLM）複製或改作，
以及透過 LLVM IR、編譯、反編譯或其他程式語言轉換的受授權條款規範的內容。
更改名稱、格式、語言或工具本身並不能免除這些義務。重用 NeverD 內容時，
應將 NeverD 標明為其來源；僅註明 AI 模型或 LLVM 無法指明該來源。

散布原始碼以及為所散布的二進位檔案提供對應原始碼時，須附上完整的授權條款和適用聲明。
對於二進位檔案和網路服務，還須遵守 AGPL 第 6 條和第 13 條的適用規定。
僅有引用、連結或致謝，**不能**取代 AGPL 對授權、修改聲明或原始碼可取得性的要求。

本指南說明現有授權條款，不增加第 7 條所述的限制或附加條款。
權威條款見 [LICENSE](../../LICENSE)，特別是第 0、2、4–6 和 13 條；也可從
[自由軟體基金會](https://www.gnu.org/licenses/agpl-3.0.html)取得。

## 讓來源可追溯

對於每一處重用內容，我們建議在程式碼旁或專案聲明中記錄原始檔案或符號、
確切的提交或發行版本，以及簡短的修改說明。請使用包含完整提交雜湊值的
GitHub 永久連結，以便引用始終指向同一份原始碼。
這些額外的來源資訊屬於引用建議，並非附加授權條件。

例如，將方括號內的欄位替換為實際來源資訊：

```text
This project includes material from NeverD.
Copyright (C) 2026 NeverD contributors (https://github.com/NeverSight/NeverD)
License: GNU Affero General Public License, version 3 only (AGPL-3.0-only).
Original source: https://github.com/NeverSight/NeverD/blob/[full-commit]/[path]
Changes: [description], [YYYY-MM-DD].
The applicable license and notices are included with this distribution.
```

在 AI 輔助工作流程中，請將這些來源資訊與所選原始碼上下文一併保留，
並將其帶入您發布的任何受授權條款規範的程式碼中。分享前，請檢查產生的程式碼及其聲明。
對於包含受授權條款規範的 NeverD 原始碼的資料集，散布這些原始碼時，
須保留適用的聲明和授權資訊。

## 研究、參考與輸出

在使用 NeverD 或借鑑其實作的論文、文件、基準測試和專案中，請引用 NeverD。
[CITATION.cff](../../CITATION.cff) 提供機器可讀的軟體引用中繼資料；
也可使用以下純文字引用，並註明您實際使用的版本或提交：

```text
NeverD contributors. NeverD: Binary analysis and decompilation engine.
https://github.com/NeverSight/NeverD. Version or commit: [revision used].
```

僅研究某個想法或演算法，並不會自動使獨立實作受 NeverD 授權條款規範。
同樣，在他人的程式上執行 NeverD，也不會自動使輸出採用 AGPL：
根據第 2 條，只有當輸出內容構成受授權條款規範的著作時，輸出才受該授權條款規範。
這項區分同樣適用於 AI 輸出；使用 NeverD 進行訓練或讀取其內容，
並不會自動使模型的每一份輸出都成為受授權條款規範的著作。
對於這些不含受授權條款規範內容的用途，我們請求引用，作為學術與工程實務，
而非將其設為新的授權條件。

## 第三方內容與早期副本

LLVM、Capstone 和 Unicorn 等元件保留各自的授權條款。
請查閱其原始碼聲明、[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)
以及任何目錄專用的授權條款，包括
[測試語料庫授權條款](https://github.com/NeverSight/testbins/blob/9d9362d2cdfe0b4b0347bd0e12b3b8fac67d3a5b/LICENSE)。
重用這些內容時，須保留原有第三方署名，並遵守相應授權條款。
本指南不對第三方內容重新授權，也不撤銷此前授予早期副本的權限。
