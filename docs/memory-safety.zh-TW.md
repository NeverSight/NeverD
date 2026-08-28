**語言**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← 文件索引](README.zh-TW.md)

# 記憶體安全稽核與獵取

NeverD 對已載入二進位做兩類記憶體安全分析，並以結構化 JSON 回報結果。兩條軌道都跑在格式無關的提升 IR 上，因此 **PE/COFF、ELF、Mach-O 是同等的一等目標**——發現不會藏在某一種格式的掃描器或匯入表後面。

| 軌道 | 命令 | 報告內容 |
|------|------|----------|
| **稽核（Audit）** | `neverd audit <binary>` | 堆積物件生命週期缺陷：洩漏、重複釋放、釋放後使用 |
| **獵取（Hunt）** | `neverd hunt <binary>` | 危險拷貝越界，並給出符號證據與候選輸入值；在處理程序輸入轉接器映射至實際位元組前為 `replayable=false` |

引擎重用 NeverD 自研符號執行與位向量求解器產生見證並確認可達性；不依賴外部求解器、虛擬機或容器。

---

## 核心不變量：失敗即閉合

未提升的操作、ABI 未能恢復參數的呼叫、未解析的間接目標，或預算耗盡，一律給出 **UNKNOWN**，從不給出 SAFE。無法恢復容量的目的緩衝區也是 UNKNOWN。嚴格提升保持原樣；安全層只在其上疊加保守裁決。

---

## 依格式的身分契約

兩條軌道都要求 lift 管線（它負責恢復每次呼叫的參數），並透過 NeverD 其餘部分共用的身分視圖為每條發現命名被調函式。除錯資訊發現順序不變：

| 格式 | 除錯資訊（優先級由高到低） | 匯入 / thunk 解析 |
|------|----------------------------|-------------------|
| **PE/COFF** | `--pdb`、除錯目錄或同級 `.pdb`，然後是 MSVC `/MAP` | IAT 槽與 `__imp_` thunk、序號匯入 |
| **ELF** | 映像內 DWARF、拆分 `*.debug`，然後是 GNU/LLD MAP | PLT stub 解析為匯入名 |
| **Mach-O** | 映像內 DWARF、相鄰 `.dSYM`，然後是 ld64 `-map` | dyld bind / 間接符號槽與 stub helper |

`--pdb` / `--map` 指定權威伴生檔：讀取失敗是錯誤，不是靜默回退。`--no-debug` 在所有格式上都只讀映像本身。

PDB 程序簽章用來區分有回傳值的配置函式與 `void` 釋放函式。PDB 區域變數與堆疊型別的完整恢復仍然有限；無法確認精確物件大小時，獵取會回退到框架／配置點模型並給出 UNKNOWN，而不會虛構容量。

### 名稱來源優先級

每條發現都帶 `name_source`，說明被調名來自何處，依以下優先級選擇：

1. `rename` — 呼叫方提供的重新命名
2. `import` — IAT（PE）、PLT（ELF）或 dyld-bind / stub（Mach-O）條目
3. `export` / `symbol` — 映像已陳述的匯出、符號表條目或其他非佔位名
4. `pdb` / `dwarf` / `map` — 為佔位名建立身分或與映像既有名稱一致的除錯符號
5. `sig` — 簽章庫匹配
6. `synthetic` — 為未命名常式鑄造的佔位名

僅由 DWARF 命名的靜態連結 `memcpy` 報告 `dwarf`；匯入的 `memcpy` 在所有格式上都報告 `import`。伴生檔不會覆蓋映像已陳述的不同非佔位名，簽章匹配也不會覆蓋任何已陳述身分。

---

## Sink 與 source 目錄

目錄是可設定表，不是寫死的集合。每個 **sink** 條目宣告弱點類別、角色（copy、format、alloc、free、realloc）以及相關參數槽（目的、來源、長度、容量）。每個 **source** 條目命名一個受攻擊者影響的輸入提供者。

內建條目位於 [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) 與 [`SafetySources.def`](../include/neverd/safety/SafetySources.def)，涵蓋常見 C 執行階段拷貝族（`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…）、帶明確目的容量的加固 `_chk` 變體、配置與釋放族（`malloc`/`calloc`/`realloc`/`free`、operator `new`/`delete`），以及可選的 Win32 堆積 API。輸入來源包括 POSIX（`getenv`、`read`、`recv`、`fgets`、`fread`、`scanf`、程式引數）**以及** Win32（`GetCommandLineA/W`、`ReadFile`、`GetEnvironmentVariable*`），因此 PE 獵取不限於 POSIX 輸入。

各格式拼寫摺疊到同一條目：去掉前導底線（`_malloc`、`___strcpy_chk`），透過別名匹配重整後的 operator new/delete。

可用規格檔擴充或覆蓋目錄：

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 }
] }
```

---

## 獵取：拷貝越界裁決

對每個拷貝 sink，獵取依此順序恢復目的容量——除錯宣告的陣列大小，然後是已知大小的堆積配置點，然後是可靠的堆疊框上界——並透過反向 SSA 行走（跟隨堆疊槽 spill/reload）對決定寫入長度的參數分類：

- **常數長度** 在精確容量內時為 SAFE。常數越界只有在已佐證路徑上可達 sink 時才為 UNSAFE；否則保持 UNKNOWN。
- **加固** `_chk` 拷貝帶執行階段目的上界。請求被拒絕，或該上界已證明不超過恢復出的物件容量時為 SAFE；存在越過物件的可行寫入時為 UNSAFE；上界未恢復或結論不足時為 UNKNOWN。
- **可證明有界** 的長度（回傳長度的呼叫、遮罩、鉗位）在求解前退出並記錄原因。只有目的大小精確時才是 SAFE；若只有包含區域上界，則仍為 UNKNOWN。
- **受攻擊者影響** 且容量已知的長度交給位向量求解器：若存在大於容量的可行長度，裁決為 UNSAFE，並將求解器模型報告為符號證據與候選值；在處理程序輸入轉接器可用前不可重播（`replayable=false`）。
- 其餘情況——未知長度或未知容量——為 UNKNOWN。

每一項恢復出的容量都是真實物件大小的 **上界**，因此被證明的越界不會是誤報。

---

## 稽核：堆積生命週期裁決

對每次配置，稽核在控制流圖上追蹤控制代碼（含堆疊 spill/reload），並套用逃逸摘要（回傳、寫入非堆疊位址，或交給不透明被調）：

- **洩漏** — 控制代碼既未釋放也不允許逃逸。
- **重複釋放** — 某條路徑上第二次釋放在第一次之後可達。
- **釋放後使用** — 釋放之後仍可達解參考或不透明使用。

配置與釋放 **包裝函式** 透過逐函式逃逸摘要識別，因此 `malloc`/`free` 轉發器不會掩蓋缺陷。互斥分支上的釋放不報告為重複釋放。

堆狀態機先產生候選事件序列（配置、釋放、使用或返回出口）；只有第二遍在符號 LowIR 路徑上依序重放這些事件，並由求解器證明路徑謂詞可滿足後，發現才會成為高信心 UNSAFE。缺少 LowIR、不透明操作、無摘要呼叫、求解器不確定或探索預算耗盡，都會把候選降為 UNKNOWN。可能別名造成的記憶體 havoc 另行追蹤，因此一般堆疊框架寫入不會否決本來精確的可達性證據。

---

## 預算、輸出與繫結

獵取探索與求解器受預算約束（`--max-paths`、`--max-steps`、`--max-loop`、`--solver-conflicts`）；預算耗盡給出 UNKNOWN。兩條命令都列印 JSON，並尊重 `-o`。結束碼：SAFE 為 `0`，UNSAFE 為 `2`，UNKNOWN 或出錯為 `1`。

同一分析也可透過 C API（`neverd_session_audit_json` / `neverd_session_hunt_json`，帶版本化 `neverd_safety_options`）和 Python SDK（`Session.audit()` / `Session.hunt()`）使用。

### 發現 schema

```json
{
  "class": "buffer_overflow",
  "function": "parse_header",
  "name": "strcpy",
  "name_source": "import",
  "call_va": "0x11a4",
  "source": "reader.c:42",
  "sink": "strcpy",
  "arg_index": 1,
  "flow": "TAINTED",
  "verdict": "UNSAFE",
  "confidence": "HIGH",
  "capacity": 16,
  "capacity_kind": "exact",
  "corroboration": "path predicate and overflow are jointly satisfiable",
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

---

## 誤報邊界與範圍

- 容量要麼精確、要麼是真實物件大小的上界，因此 UNSAFE 反映真實越界。若精確大小不可用，而包含區域上界又不足以證明安全，結果為 UNKNOWN。
- 長度受限的拷貝在求解前退出並計入 `skipped`；精確容量可證明 SAFE，只有上界時仍為 UNKNOWN。
- 已入目錄的寬字元與追加拷貝，在元素位元組寬度或目的字串既有長度未恢復時保持 UNKNOWN。出參配置器與條件 `realloc` 的所有權轉移無法證明時也保持 UNKNOWN。
- **P0**（本發行，三種格式）：sink 目錄、參數預過濾、拷貝越界獵取、堆積生命週期稽核。每個測試主機都執行 PE、ELF、Mach-O × x86-64、AArch64 六個已檢入樣例。
- **P1**：堆疊/全域越界、未初始化讀、格式字串、更豐富的 PDB 堆疊型別、更多平台配置器。
- **P2**：patch 插入的執行階段檢查、程序間攻擊者可達性。
