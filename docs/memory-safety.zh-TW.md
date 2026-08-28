**語言**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← 文件索引](README.zh-TW.md)

# 記憶體安全稽核與獵取

NeverD 對已載入二進位做兩類記憶體安全分析，並以結構化 JSON 回報結果。兩條軌道都跑在格式無關的提升 IR 上，因此 **PE/COFF、ELF、Mach-O 是同等的一等目標**——發現不會藏在某一種格式的掃描器或匯入表後面。

| 軌道 | 命令 | 報告內容 |
|------|------|----------|
| **稽核（Audit）** | `neverd audit <binary>` | 堆積物件生命週期缺陷及未初始化區域堆疊讀取 |
| **獵取（Hunt）** | `neverd hunt <binary>` | 危險拷貝越界，並給出符號證據與候選輸入值；僅在存在完整 `process-input-v1` 計畫時為 `replayable=true` |

引擎重用 NeverD 自研符號執行與位向量求解器產生見證並確認可達性；不依賴外部求解器、虛擬機或容器。

---

## 核心不變量：失敗即閉合

未提升的操作、ABI 未能恢復參數的呼叫、未解析的間接目標，或預算耗盡，一律給出 **UNKNOWN**，從不給出 SAFE。無法恢復容量的目的緩衝區也是 UNKNOWN。嚴格提升保持原樣；安全層只在其上疊加保守裁決。

呼叫效果採用封閉世界語意：只有前置條件與所有相關效果均已知時才套用摘要。未知效果或只能部分套用的摘要保持 UNKNOWN；分析不會把缺口假定為無效果或呼叫成功。

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

目錄是可設定表，不是寫死的集合。每個 **sink** 條目宣告弱點類別、角色（copy、format、alloc、free、realloc）以及相關參數槽（目的、來源、長度、容量）。JSON 中的 copy 或 format sink 還會提供可執行的呼叫 effect。每個 **source** 條目命名一個受攻擊者影響的輸入提供者。

內建條目位於 [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) 與 [`SafetySources.def`](../include/neverd/safety/SafetySources.def)，涵蓋常見 C 執行階段拷貝族（`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…）、帶明確目的容量的加固 `_chk` 變體、配置與釋放族（`malloc`/`calloc`/`realloc`/`free`、operator `new`/`delete`），以及可選的 Win32 堆積 API。輸入來源包括 POSIX（`getenv`、`read`、`recv`、`fgets`、`fread`、`scanf`、程式引數）**以及** Win32（`GetCommandLineA/W`、`ReadFile`、`GetEnvironmentVariable*`），因此 PE 獵取不限於 POSIX 輸入。

各格式拼寫摺疊到同一條目：去掉前導底線（`_malloc`、`___strcpy_chk`），透過別名匹配重整後的 operator new/delete。

若 JSON copy 或 format sink 省略 `effect`，則依所引用的最高參數槽推導適用性：copy 要求精確的參數個數，format sink 接受從該最小參數個數到可變參數上限的呼叫。可選的 `effect` 物件可用 `min_arity` 與 `max_arity`（或 `"variadic"`）明確設定可接受的參數個數範圍，包括超出所推導 copy 精確參數個數的額外 wrapper 參數；`min_arity` 必須至少為最高被引用角色槽加一，而 `formats` 與 `abis` 用於限制適用性。若呼叫的參數個數、目標格式或 ABI 不相符，就不套用摘要，並依封閉世界規則保持 UNKNOWN。

可用規格檔擴充或覆蓋目錄：

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 },
    { "name": "my_format", "kind": "format", "dst": 0, "fmt": 2,
      "effect": { "min_arity": 3, "max_arity": "variadic",
                  "formats": ["elf"], "abis": ["sysv"] } }
  ],
  "sources": [
    { "name": "my_read", "out": 1, "return_tainted": true }
  ]
}
```

對於自訂 source，`out` 與 `return_tainted` 僅是發現中繼資料，並不建立可執行的記憶體、回傳值或 taint effect。目前的 source schema 缺少這些語意所需的型別化成功條件、記憶體修改、目標格式與 ABI 契約，因此依賴自訂 source effect 的分析必須保持 UNKNOWN。內建 source 不受影響：其經過適用性檢查的型別化描述符會繼續提供可執行 effect。

不會因存在同名 source 條目就推導出無界的僅目的參數自訂 sink。類似 `gets` 的自訂 sink 必須明確設定 `"unbounded": true`；將同名函式加入 source 目錄不會賦予它可執行 effect，互相矛盾的來源/長度欄位會以交易方式拒絕。

---

## 獵取：拷貝越界裁決

對每個拷貝 sink，獵取依此順序恢復目的容量——除錯宣告的陣列大小，然後是已知大小的堆積配置點，然後是可靠的堆疊框上界——並透過反向 SSA 行走（跟隨堆疊槽 spill/reload）對決定寫入長度的參數分類：

- **常數長度** 在精確容量內時為 SAFE。常數越界只有在已佐證路徑上可達 sink 時才為 UNSAFE；否則保持 UNKNOWN。
- **加固** `_chk` 拷貝帶執行階段目的上界。請求被拒絕，或該上界已證明不超過恢復出的物件容量時為 SAFE；存在越過物件的可行寫入時為 UNSAFE；上界未恢復或結論不足時為 UNKNOWN。
- **可證明有界** 的長度（回傳長度的呼叫、遮罩、鉗位）在求解前退出並記錄原因。只有目的大小精確時才是 SAFE；若只有包含區域上界，則仍為 UNKNOWN。
- **受攻擊者影響** 且容量已知的長度交給位向量求解器：若存在大於容量的可行長度，裁決為 UNSAFE。只有能建立完整 `process-input-v1` 計畫時，候選值才可重播。初始範圍僅包括精確的字面環境值，以及至多第一次受支援的 `read(0)` 系列標準輸入消耗所回傳的位元組。argv、檔案、網路、自訂或有歧義的輸入保持不可重播並附帶原因。
- 其餘情況——未知長度或未知容量——為 UNKNOWN。

每一項恢復出的容量都是真實物件大小的 **上界**，因此被證明的越界不會是誤報。

### 格式化輸入

對於 `scanf`/`fscanf` 及其帶版本拼寫，可讀的常數格式會把每個未抑制轉換映射到其實際的可變參數輸出參數。無界 `%s`/`%[` 輸出會把 taint 傳播到後續字串使用；數值與字元輸出會污染從被寫物件載入的值，但不污染輸出指標值本身。`sscanf` 僅在其輸入字串已受攻擊者影響時傳播這些 effect。`%Ns`/`%N[` 等有界文字輸出會連同包含終止符的 `MaxBytes` extent 一起傳播 taint；寬字元變體使用平台的 `wchar_t` 寬度計算該位元組 extent。被抑制的轉換、多餘參數、位置相依或不受支援的格式以及 `%n` 保持 UNKNOWN，不作猜測。

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "replay": { "adapter": "process-input-v1", "reason": "argv input is not supported by process-input-v1" }, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

`replayable` 是衍生證據，而非獨立承諾：僅當 `replay` 包含供 `process-input-v1` 轉接器使用的完整輸入計畫時才為真。計畫記錄精確的環境位元組、使用時第一次受支援的 `read(0)` 系列標準輸入位元組序列，以及從求解器賦值 ID 到這些輸入的綁定；無法建立時由 `replay.reason` 說明原因。這些欄位以增量方式加入；頂層 `schema_version` 仍為 `1`。

---

## 誤報邊界與範圍

- 容量要麼精確、要麼是真實物件大小的上界，因此 UNSAFE 反映真實越界。若精確大小不可用，而包含區域上界又不足以證明安全，結果為 UNKNOWN。
- 長度受限的拷貝在求解前退出並計入 `skipped`；精確容量可證明 SAFE，只有上界時仍為 UNKNOWN。
- 已入目錄的寬字元與追加拷貝，在元素位元組寬度或目的字串既有長度未恢復時保持 UNKNOWN。出參配置器與條件 `realloc` 的所有權轉移無法證明時也保持 UNKNOWN。
- **P0**（本發行，三種格式）：sink 目錄、參數預過濾、拷貝越界獵取、堆積生命週期稽核。每個測試主機都執行 PE、ELF、Mach-O × x86-64、AArch64 六個已檢入樣例。
- **P1**：堆疊/全域越界、未初始化區域讀取與格式字串檢查已提供；更豐富的 PDB 堆疊型別和更多平台配置器仍是增量覆蓋項，缺少精確摘要時保持 UNKNOWN。
- **P2**：patch 插入的執行階段檢查、程序間攻擊者可達性。
