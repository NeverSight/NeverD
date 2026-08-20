**語言**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← 文件索引](README.zh-TW.md)

# 記憶體安全稽核與獵取

NeverD 對已載入二進位做兩類記憶體安全分析，並以結構化 JSON 回報結果。兩條軌道都跑在格式無關的提升 IR 上，因此 **PE/COFF、ELF、Mach-O 是同等的一等目標**——發現不會藏在某一種格式的掃描器或匯入表後面。

| 軌道 | 命令 | 報告內容 |
|------|------|----------|
| **稽核（Audit）** | `neverd audit <binary>` | 堆積物件生命週期缺陷：洩漏、重複釋放、釋放後使用 |
| **獵取（Hunt）** | `neverd hunt <binary>` | 危險拷貝越界，並給出可重現的具體見證 |

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

### 名稱來源優先級

每條發現都帶 `name_source`，說明被調名來自何處，依以下優先級選擇：

1. `rename` — 呼叫方提供的重新命名
2. `import` — IAT（PE）、PLT（ELF）或 dyld-bind / stub（Mach-O）條目
3. `pdb` / `dwarf` / `map` — 除錯符號，依載入器種類
4. `export` / `symbol` — 映像匯出表或符號表條目
5. `sig` — 簽章庫匹配
6. `synthetic` — 為未命名常式鑄造的佔位名

DWARF 命名的靜態連結 `memcpy` 報告 `dwarf`；匯入的 `memcpy` 在所有格式上都報告 `import`。簽章匹配從不覆蓋除錯器或匯入表已經給出的名字。

---

## Sink 與 source 目錄

目錄是可設定表，不是寫死的集合。每個 **sink** 條目宣告弱點類別、角色（copy、format、alloc、free、realloc）以及相關參數槽（目的、來源、長度、容量）。每個 **source** 條目命名一個受攻擊者影響的輸入提供者。

內建目錄涵蓋常見 C 執行階段拷貝族（`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…）、帶明確目的容量的加固 `_chk` 變體、配置與釋放族（`malloc`/`calloc`/`realloc`/`free`、operator `new`/`delete`），以及可選的 Win32 堆積 API。輸入來源包括 POSIX（`getenv`、`read`、`recv`、`fgets`、`fread`、`scanf`、程式引數）**以及** Win32（`GetCommandLineA/W`、`ReadFile`、`GetEnvironmentVariable*`），因此 PE 獵取不限於 POSIX 輸入。

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

- **常數長度** 直接與容量比較 → SAFE 或 UNSAFE。
- **加固** `_chk` 拷貝帶執行階段目的上界 → SAFE。
- **可證明有界** 的長度（回傳長度的呼叫、遮罩、鉗位）作為 SAFE skip 退出，並記錄原因。
- **受攻擊者影響** 且容量已知的長度交給位向量求解器：若存在大於容量的可行長度，裁決為 UNSAFE，求解器模型作為具體見證。
- 其餘情況——未知長度或未知容量——為 UNKNOWN。

每一項恢復出的容量都是真實物件大小的 **上界**，因此被證明的越界不會是誤報。

---

## 稽核：堆積生命週期裁決

對每次配置，稽核在控制流圖上追蹤控制代碼（含堆疊 spill/reload），並套用逃逸摘要（回傳、寫入非堆疊位址，或交給不透明被調）：

- **洩漏** — 控制代碼既未釋放也不允許逃逸。
- **重複釋放** — 某條路徑上第二次釋放在第一次之後可達。
- **釋放後使用** — 釋放之後仍可達解參考或不透明使用。

配置與釋放 **包裝函式** 透過逐函式逃逸摘要識別，因此 `malloc`/`free` 轉發器不會掩蓋缺陷。互斥分支上的釋放不報告為重複釋放。

---

## 預算、輸出與繫結

獵取探索與求解器受預算約束（`--max-paths`、`--max-steps`、`--max-loop`、`--solver-conflicts`）；預算耗盡給出 UNKNOWN。兩條命令都列印 JSON，並尊重 `-o`。結束碼：乾淨執行為 `0`，存在 UNSAFE 發現為 `2`，出錯為 `1`。

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "17 bytes" } }
}
```

---

## 誤報邊界與範圍

- 容量始終是上界，因此 UNSAFE 反映真實越界。宣告大小不可用的過小緩衝區可能被報 SAFE 而不是 UNSAFE（保守漏報，絕非誤報）。
- 長度受限的拷貝作為 SAFE skip 退出；這優先保證獵取要證明的受攻擊者控制案例的精度。
- **P0**（本發行，三種格式）：sink 目錄、參數預過濾、拷貝越界獵取、堆積生命週期稽核。
- **P1**：堆疊/全域越界、未初始化讀、格式字串、更豐富的 PDB 堆疊型別、更多平台配置器。
- **P2**：patch 插入的執行階段檢查、程序間攻擊者可達性。
