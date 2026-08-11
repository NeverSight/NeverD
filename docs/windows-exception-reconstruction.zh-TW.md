**語言**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Windows 例外重建

[← 文件索引](README.zh-TW.md)

NeverD 在載入、提升、反編譯及二進位重寫的完整流程中攜帶 Windows 表格驅動例外資訊。
例外 metadata 是函式可執行契約的一部分：只有能證明產生的程式碼、runtime-function
record、語言表與防護表彼此一致時，NeverD 才允許重寫。

本文區分三種支援層級：

- **分析**：把原生表示解碼成經過檢查的正規化 record，並提供給 IR pipeline。
- **反編譯**：把可規約保護區表示為明確的 HighIR 例外節點；其他形狀保留確定性的
  原生註解，不遺失 handler 或狀態轉移。
- **原生重建**：patch 模式可要求 LLVM 發射完整替代例外契約，並安裝到最終 PE。

支援分析不代表支援原生重建。

## 支援矩陣

| 原生形式 | 提升與分析 | 高階輸出 | Patch 模式 |
|----------|------------|----------|------------|
| x64 unwind v1/v2 | 完整、經檢查的 unwind record、operation、chain、handler data 與來源 | frame/unwind 摘要，並在適用時提供結構化語言區域 | 支援完整 primary record；產生的 `.pdata` 與 `.xdata` 取代被覆蓋的 closure |
| x64 unwind v3/APX | 獨立的 v3 payload、epilog 與 operation 計數 | 明確 v3 註解 | 僅分析；拒絕修改涉及的函式 |
| ARM32/ARM64 packed unwind | 函式範圍、packed 欄位、primary/fragment 身分 | frame/unwind 摘要 | 僅當 record 完整、沒有語言 handler，且映像無可獨立定址 fragment 時支援 |
| ARM32/ARM64 unpacked unwind | 經檢查的 xdata header/code 範圍、handler 關聯與 fragment | frame/unwind 摘要 | 僅當 record 完整、沒有語言 handler，且映像無可獨立定址 fragment 時支援 |
| `__C_specific_handler` | scope 範圍、filter、finally target、handler 與 continuation target | 可規約區域變為 `__try`/`__except`/`__finally`；不完整或不可規約區域保留註解 | 對完整且可表示的 scope graph 執行原生 x64 重建 |
| `__CxxFrameHandler3` | unwind map、try map、catch、catch-object/frame offset、continuation 與 IP-to-state map | 可規約狀態區間變成明確 C++ HighIR，並帶 C 相容型別註解 | 對下文所述嚴格受限且 verifier-clean 的子集執行原生 x64 重建 |
| `__CxxFrameHandler4` | 有界變長解碼到共用 C++ graph，包括 action kind 與 object offset | 同一 HighIR graph 並保留 FH4 來源 | 僅分析；拒絕修改涉及的函式 |
| `__GSHandlerCheck_SEH/EH/EH4` | 包裝後的 personality 與經檢查的 GS cookie 來源 | 基礎語言 graph 加 wrapper 註解 | 僅分析；拒絕修改涉及的函式，不做降級 |
| x86 registration-chain EH | 與表格驅動 EH 明確區分 | 不支援形式的註解 | 不重建 |

畸形 record 絕不會當成一般完整 record。部分解碼 record 仍可用於檢查，但不能授權產生
原生 metadata。如果 ARM xdata header 仍可證明有界可執行 fragment 範圍，而後續
unwind body 已損壞，反組譯仍可使用該範圍；record 會標記為 malformed，且不會升級為
可 patch 函式。

## 正規化模型

`ExceptionInfo` 由 `BinaryImage` 擁有。每個 `ExceptionFunction` 包含：

- 經檢查的半開 code range；
- primary、chained 或 fragment 身分；
- 原生 unwind encoding 及精確 runtime/unwind 來源；
- 正規化 unwind operation 與 epilog；對語意未完全理解的 operation 保留 opaque
  operand bytes；
- 精確 personality 身分及 handler data；
- 可選 SEH scope、C++ state map 與 GS cookie data；
- `Complete`、`Partial` 或 `Malformed` 狀態，以及確定性 diagnostic。

loader 不透過此模型暴露原始檔案指標。原生 RVA 保留供 diagnostic 與 patch 取代；
IR consumer 只操作已驗證 VA 與 range。

全映像 index 允許 chained/fragment record 重疊，並回傳覆蓋地址的最具體函式。任何
損壞 directory、range、pointer、count、state transition、compressed integer、chain
cycle 或 decode budget 耗盡都會降低相關 parse status。

語言表限制同時按每張原生表與單一函式的完整正規化 graph 累計執行。因此，即使多個
try-map entry 重複使用同一 handler map，解析工作也不能超過總預算。共享相同
`FuncInfo` 與 personality 的 FH3 record 會當作有界函式群組解碼，使父函式的
IP-to-state map 能合法指向其 catch funclet，同時排除不相關 runtime function 地址。

## IR 契約

例外 metadata 貫穿每一種 IR 表示，不改變一般 CFG 的意義：

- LowIR 在 protected-range boundary、state transition、filter、handler、cleanup
  action 與 continuation target 處拆分 basic block。
- 例外 successor/predecessor 與一般 successor/predecessor 分開保存，因此現有
  dominator 與 structuring 演算法不會把 runtime dispatch edge 誤認為 machine branch。
- MedIR 保留正規化 function descriptor 與穩定例外 edge。
- HighIR 使用不同的 `SEHTry` 與 `CxxTry` statement。clause descriptor 保留原生
  target VA、type descriptor、adjective、catch-object/parent-frame offset、cleanup
  action kind/object offset、state 與 continuation VA。

HighIR structurer 對區間採保守策略。它只移動地址完整位於 complete protected range
中的連續 statement slice，並由內向外處理巢狀區域。交叉區域、partial graph、無地址
歧義邊界及 out-of-line funclet 保留原控制流程，並增加函式 unstructured-EH 計數。

C backend 為可規約 single-clause SEH 區域發射 MSVC SEH 語法。HighC 是 C backend，
因此 C++ catch 與 cleanup state 以確定性 C 相容註解輸出，不聲稱產生可編譯 C++。
out-of-line 原生 funclet 保留精確地址。

## LLVM metadata schema

每個與已發射函式關聯的已解析例外函式都會取得無損 LLVM metadata，即使不能使用原生
WinEH lowering：

- function attachment：`neverd.windows.eh`；
- native-lowering marker：`neverd.windows.eh.native`；
- module table：`neverd.windows.eh.functions`；
- current schema version：`3`。

固定函式 record 攜帶 parse status、encoding、code range、原生 runtime/unwind RVA、
runtime-record kind 與 chain 來源、packed-unwind word、frame description、canonical
與 resolved personality name、handler data、精確原生 unwind bytes、正規化 operation
（含 native slot count）與 epilog、SEH scope、C++ header/map、GS data、diagnostic 與
regeneration flag。patch 驗證要求 schema version 精確相符，且 range 與載入映像完全
一致。具有例外契約的自動命名 lifted function 不能靜默省略 attachment。

原生 x64 SEH lowering 使用 LLVM WinEH 結構；只有完整 scope graph 可表示時才發射
verifier-clean `invoke`/funclet 控制流程。原生 FH3 lowering 更嚴格，要求：

- x64 COFF、unwind v1/v2、完整 metadata、有效同步 FH3 state graph；
- 不含 `noexcept`、asynchronous、separated-funclet、GS-wrapper、FH4 或未知 flag 語意；
- protected interval 巢狀或互不相交，不可交叉；
- 不含 destructor/unwind action、catch-object construction 或 parent-frame dependency；
- handler 是 lifted function 中無一般 predecessor 且無 call 的 block；
- 每個可能 unwind 的 protected operation 都以 LLVM `invoke` 表示。

任一條件不符時，lifted LLVM 仍可分析並保留無損 metadata，但 patch planning 會拒絕
取代原生語言表。PE entry point、TLS callback 與 CRT callback root 是保留邊界，
不是一般 ABI rewrite candidate。

## Patch 交易

對於受支援的重寫，NeverD 把例外重建視為單一 PE 交易：

1. 根據載入的例外 graph 與 LLVM metadata attachment 驗證每個受影響函式。
2. 編譯替代程式碼，同時保留 section identity、alignment、allocation flag、code/data
   trait 與 semantic symbol-index reference。code generation 前將本地模型化的 Windows
   personality externalize，使 emitted xdata 綁定到已證明的原始 executable handler，
   而不是重新編譯 private ABI routine。
3. 保留未受影響的 runtime-function entry，並移除每個受影響 primary function 被取代的
   完整原生 closure，包括相關 chained record。
4. relocation generated code/xdata，合併 generated/retained pdata，依 begin RVA 排序並
   拒絕重疊；證明每個 redirected language-EH entry 都由相同 personality class 的
   generated runtime-function record 覆蓋，然後安裝唯一 replacement PE exception
   directory。
5. 保留輸入 CFG instrumentation mode，解析 `.gfids` semantic reference，並將 target
   與 redirected entry 合併到原 Guard CF table。解析 `.gehcont` reference 為 generated
   executable VA，合併到原 Guard EH continuation table，並在保留 guard flag 時更新
   load-config pointer/count。未解析 CFG dispatch/check helper 會中止交易。需要不同
   code-generation contract 的 guard mode（CFW、return-flow guard、retpoline、XFG）
   僅分析並拒絕重寫。
6. 寫入磁碟前重新解析完整 byte image。

LLVM fork extension 刻意保持通用：final-image writer 保存 object section 扁平化時會
遺失的 section trait 與 semantic symbol-index reference。PE parsing、MSVC language
table decoding、policy、directory merging、load-config update 與 final validation
仍位於 NeverD。

原 Guard CF 與 Guard EH continuation entry 會保留，因為原 entry trampoline 仍是有效
indirect target。generated target 必須指向 emitted code；結果表必須嚴格依 RVA 排序。

## 最終映像驗證

除非滿足下列全部條件，否則拒絕 patched PE：

- LLVM 接受 bytes 為 COFF object，且 PE machine、class、section table、optional-header
  directory bounds、image base 與 image extent 一致；
- 每個 section 的 raw/virtual extent 在界內，且 section range 不重疊；
- exception-directory extent 由檔案承載並位於映像內；
- runtime-function entry 已排序、非空、無重疊且完全位於 executable region；
- x64 unwind RVA 對齊，header/code array 由檔案承載，version/flag 受支援，handler
  target 可執行，chained record 無環且符合 depth limit；
- 在記憶體重建最終 import、export 與 COFF symbol，使已知 SEH/FH3 personality 可從
  完整 bytes 再解析 scope/state table；
- ARM runtime entry 與 xdata 識別有效且受支援的 version/range；
- guard flag 宣告 table 時，load-config 存在 Guard CF 與 Guard EH continuation field；
- guard pointer/count/stride 同時位於 PE image 與檔案範圍內，且每個 entry 嚴格排序並
  指向 executable target。

驗證失敗會中止 patch。NeverD 不會在驗證失敗後寫出 best-effort image。

## 聚焦驗證

建置 lift suite，並執行 Windows EH model、parser、IR、codegen 與 PE integration case：

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

受保護 x64 fixture 使用 `/guard:cf` 與 `/guard:ehcont` cross-assemble 及 link。整合測試
載入其 SEH scope 與 guard table，檢查 structured HighC output、patch image、重新載入，
並驗證更新後 table count、排序與 executable target。

獨立連結的 x64 FH3 fixture 透過相同完整交易涵蓋受支援 C++ closure。它驗證原固定表、
HighC state annotation、personality binding 保留、重建 try/catch graph，以及 patch 後
重新載入的 IP-to-state map。

修改 parser 時也要執行現有 ARM format case，因為 ARM packed/unpacked xdata 共用
正規化模型與 final runtime-entry check。

## 擴充原生支援

新增原生重建支援時，必須在同一變更中包含：

- 完整有界 parser 與正規化模型 invariant；
- HighIR 及 LLVM metadata round-trip coverage；
- 每種新接受 graph shape 對應的 verifier-clean native IR；
- 必要的 emitted-section 與 semantic-reference retention；
- 對精確 architecture/personality/version 的 linked PE fixture；
- exception-directory、load-config 與 final-image structural validation；
- 對最接近不支援形狀的明確 rejection test。

不能只因能解碼新 record 就擴大 allow-list。接受標準是最終 linked image 中的 runtime
exception behavior 得到保留。
