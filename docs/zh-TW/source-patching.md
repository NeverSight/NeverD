**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: eb2a70d2b1d15c943ee92c3ca762c2328fdf4f9c742fc05a8e52771179931675 -->

[← Documentation Index](README.md)

# 原始碼與原生物件 patch

NeverD 使用指定編譯器編譯替換函式，再安裝到明確的原函式入口。編譯器負責 C/C++ 語言語意；NeverD 負責位址繫結、重定位、儲存、展開記錄與發佈。二進位修復與提升後的重寫繼續使用各自管線，不需重建程式其餘部分。

<!-- i18n-section: compiler -->
## 選擇編譯器

使用 `--compiler=clang`、`--compiler=gcc` 或 `--compiler=neverc`。Clang 與普通模式 NeverC 可產生 LLVM IR；`object` 製品保留編譯器產生的機器指令。CLI 對 GCC 預設選擇物件；Python SDK 對 GCC 也預設選擇物件。選定的編譯器不會靜默退回其他編譯器。

使用 `--compiler-arg` 或 SDK 的 `compiler_args` 傳入包含路徑、巨集定義及受支援的原始碼選項。禁止覆寫目標或輸出模式，也禁止在任何參數位置傳入回應檔標記（`@file`），包括獨立選項的值。一般的 `-I` 路徑和 `-D` 巨集定義仍然受支援。

示例位址須替換為已確認的原函式入口。替換宣告必須符合原機器 ABI，包括呼叫慣例與結構配置；位址本身不能證明宣告正確。C++ 需要 `--language=c++` 與正確的 Clang++/G++ 路徑。建議使用 `extern "C"` 入口，否則傳入精確的修飾名稱。允許額外輔助函式。`--entry` 與 `--func` 共同明確指定替換目標；舊入口 `--from-c` 亦遵守指定位址。

也可明確指定 `artifact="object"`。

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## 已驗證設定與限制

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C 與 hosted DynCode**

NeverC 在 x64/ARM64 ELF、PE、Mach-O 上只接受純 C 原始碼。NeverD 會在啟動編譯器前拒絕 NeverC 與 `--language=c++` 的組合，不會隱式翻譯或切換編譯器。C++ 與例外替換使用 Clang，或已驗證 ELF 物件設定下相符的 GCC。hosted C 框架可透過真實展開記錄，讓外圍 C++ 程式拋出並捕捉的例外通過；這不表示 NeverC 支援 C++。

`dyncode`: `--compile-mode=dyncode` 使用 NeverC `-fdyncode-hosted`，要求原始碼、一個明確入口及原生物件，不允許既有符號繫結。CLI 與 Python 對此模式及 GCC 預設選擇物件。安裝前驗證物件與最終載荷摘要、程式碼位元組和入口偏移，並使用真實 CFI/SEH 或完整 native compact 展開記錄。可寫/TLS 狀態、隱含 syscall/PEB/heap/string 改寫、未解析匯入及變動或重排的程式碼明確失敗；指標可由參數傳入。

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

Clang LLVM IR 路徑接入 Windows ARM64 C++ FH3，驗證 xdata、父框架 ABI 與 funclet/FuncInfo 歸屬。原生物件語言處理器仍不支援。`check_source_patch_cpp_runtime.py` 在相符的 x64/ARM64 原生 Windows 主機執行九種例外補丁情境；CPU 與 OS 展開器證據分別記錄。

| 編譯器 | 製品 | 已驗證的作業系統執行設定 |
|---|---|---|
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++；Mach-O x64/ARM64 C/C++；PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3 |
| Clang 22.1.6 | 原生物件 | ELF x64 C/C++; PE x86/x64/ARM64 C |
| GCC/G++ 13.3.0 | 原生物件 | ELF x64 C/C++，需要相容的 Itanium 執行階段 |
| NeverC 3389.1.5 普通模式 | LLVM IR | ELF x64 C；Mach-O x64/ARM64 C |
| NeverC 3389.1.5 普通模式 | 原生物件 | ELF x64 C |
| NeverC 3389.1.5 C | LLVM IR / 原生物件 | ELF / PE / Mach-O x64/ARM64: 純 C 原始碼 |
| NeverC 3389.1.5 `hosted-v1` | 原生物件 + 封存載荷 | ELF / PE / Mach-O x64/ARM64: 純 C 原始碼與穿過 C 框架的例外傳播 |

上表是已驗證的作業系統執行設定，不保證所有編譯器版本與選項。ELF、PE、Mach-O 原生安裝器皆接入四種架構。GCC 需要相符的 ELF 驅動，不能產生 LLVM IR。未知重定位、TLS、載入時的全域建構函式、LTO、common/IFUNC 符號及未解析匯入皆明確失敗。JSON 憑據記錄編譯器身分、參數、繫結及安裝位址，原生物件另記錄 SHA-256。

原生物件憑據中的 `target` 欄位來自已載入映像的 ABI，包括 ARM ELF 浮點旗標以及不同的 iOS/watchOS 平台。編譯產生的物件與外部物件使用同一項身分規則。

CPU 矩陣要求十二種組合的原始碼、外部 IR 及原生物件皆成功安裝。另分別涵蓋 ARMv7k watchOS、ARMv7 iOS、可寫／唯讀狀態、原程式浮點與堆疊參數繫結，以及改變載入位址後的入口執行。CPU 執行無法證明作業系統載入器、C++ 展開器或任意 ABI 正確；應依據上表的執行設定。

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU` 表示原始碼／IR／物件安裝、重新載入及精確函式執行。ARM ELF 從映像確定 EABI5、ARM/Thumb 狀態與浮點 ABI，並註冊完整排序的 `EHABI` 索引。PE 保留 x64/ARM 執行階段記錄與 `HIGHLOW`/`DIR64`/`MOV32T` 基底重定位；x86 使用編譯器註冊程式碼。Mach-O 依 CPU 子類型區分 watchOS DWARF 與 iOS `SjLj`。原始碼 ARM/Thumb 入口使用相對分支，可隨映像改變載入位址。

原生物件依自身有界函式符號、輔助／區段定義及展開記錄驗證範圍，不虛構 MC 擁有者 ID，也不重新編譯 IR。COFF 函式範圍須來自精確輔助長度、單函式區段（`-ffunction-sections`）或解碼後的原生執行階段函式記錄，不能依下一個符號猜測長度；watchOS 編譯轉接器保留葉函式堆疊框架以取得真實展開記錄。原生 COFF/Mach-O 語言處理常式尚不屬於已驗證物件設定；C++ 例外應使用已驗證 IR 路徑。固化 exidx 註冊表的靜態 ARM 展開器、未註冊 Mach-O 絕對指標及不支援 RELRO 儲存皆明確拒絕。

GCC C 物件亦在 ELF x86、ARM32、ARM64 上完成安裝並透過 Linux QEMU user mode 執行。轉接器從映像選擇 ARM/Thumb 模式，GNU property 記錄依 ELF 指標寬度對齊。

<!-- i18n-section: references -->
## 原程式參照與儲存

繫結對應外部宣告：`function` 要求已確認的原函式或匯入樁；`data` 要求可寫儲存；`const` 要求 IR 中為 const 宣告。物件資料繫結須提供非零範圍。範圍不可跨越儲存邊界或賦予 RELRO 寫入權限。不能回呼被替換函式的原始本體：此設定尚無保存原序言的回呼跳板。

新程式碼保持 RX，新可變狀態保持 RW。僅重用既有匯入，並區分 GOT/IAT 槽與可呼叫樁。Mach-O 需要空餘載入命令空間，例如 `-Wl,-headerpad,0x400`；PE 需要空餘區段標頭空間。空間不足會失敗。ELF `.data.rel.ro` 與原 CET/IBT 保護需要額外契約，目前明確拒絕。

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Mach-O 展開表增長另需相鄰的空餘檔案與虛擬空間；只預留標頭空間並不滿足此要求。測試樣本分別處理這兩種空間限制。

<!-- i18n-section: exceptions -->
## C++ 例外

ELF 使用編譯器 FDE/LSDA；Mach-O 使用緊湊展開/DWARF。PE x64/ARM64 支援編譯器 FH3 表，要求原程式已有相容的 MSVC 執行階段與型別身分；type-info 資料可能需要明確繫結。FH4、自訂 personality、SEH 原始碼與啟用的 CFG/EHCont/XFG 模式會被拒絕，原保護旗標維持不變。

目前明確拒絕具有 IBT 要求的 ELF x86/x64 原程式：原始碼安裝器尚未提供保留 ENDBR 的跳板。

執行案例涵蓋正常返回、內部捕捉、新例外由原呼叫者捕捉、原被呼叫函式的例外穿過替換函式、恰好一次 RAII 清理、重新擲出、noexcept 終止及未替換函式。原呼叫點必須允許例外傳播；patch 無法恢復原編譯器因函式不擲出例外而刪除的捕捉路徑。

<!-- i18n-section: publication -->
## SDK 與發佈

追加式 C API 為 `neverd_patch_source`，選項結構為 `neverd_source_patch_options_v1`；`neverd_patch_source_report` 傳回憑據。物件輸入須提供位元組長度。舊 API `neverd_patch_from_c` 仍可使用。

輸入先保存快照，候選檔暫存於目的地旁，重新載入並核對每個請求入口後才獨占發佈。不覆寫現有檔案或符號連結別名。缺少任一映射會使整個請求失敗。拒絕 `--mode=inplace`。工作階段 IR 變換不能用於原生物件。Mach-O 嚴格驗證簽章並重新簽署；不會靜默移除 PE 簽章。

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## 建置與驗證

透過 `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` 指定相容的原始碼編譯器（已驗證 Clang 22.1.6）。整合建置僅修正建置目錄中的 LLVM 副本，不改子模組或共用預建快取。r3/r4 預建版的 x64 PLT 問題可能導致 IR patch 被拒絕；原生物件避開此 MC 路徑。`NEVERD_TEST_REQUIRE_FIXED_LLVM=1` 要求產生精確且已修正的 PLT 參照。

`NeverDSourcePatchTests` 包含原生 C/API/物件/例外案例；`NeverDSourcePatchMatrixTests` 以 Unicorn 檢查所有目標組合。設定 `NEVERD_TEST_WINDOWS_CRT_LIB`、`NEVERD_TEST_WINDOWS_UCRT_LIB` 及 `NEVERD_TEST_WINDOWS_SDK_LIB` 後，`SourcePatchPE` 執行七種 C++ 場景。非 Windows 宿主另需 `NEVERD_TEST_WINDOWS_RUNNER`。`NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` 將缺少執行輸入視為失敗。Windows 專項工作流程要求原生執行且拒絕略過案例。

開發 Mac 上的 Linux 證據來自 Docker 與明確的 Linux 載入器；PE 證據使用 Wine 11。Mach-O ARM64 在 Mac 本機執行。Windows 原生 CI 結果須另行查看：設定工作流程不等於執行通過。歷史數量、基線失敗與模擬環境限制見[詳細契約與驗證記錄](../source-patching-details.md)。

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

LLVM 建置副本亦修正 Mach-O x64 的 GOT 資料參照：Clang 型別捕捉表以四位元組重定位欄位末尾為基準。r3/r4 預建版會偏移四位元組，發佈前檢查會拒絕未修正的結果。開發 Mac 的 x64 Mach-O 案例透過 Rosetta 2 執行，ARM64 案例直接執行。

Windows x64 原生驗收 [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) 的 16 個 PE/物件/trampoline 專項案例與 143 個 CPU 目標矩陣案例全部通過，沒有跳過。FH3 案例執行了七種 C++ 執行場景；目標矩陣使用 Unicorn。此輪使用 Clang 22.1.6、原生 MSVC、Windows SDK 與整合 LLVM。

Windows 工作流程還會為 x86、x64 和 ARM64 產生 27 個 PE 替換產物。`scripts/check_source_patch_pe_runtime.py` 透過原始碼 IR、編譯物件及外部物件三條路徑檢查非葉函式呼叫、可變指標狀態與浮點／堆疊參數。每個原始程式和替換產物均執行三次，即每種架構 30 次程序觀測。x86/x64 在 Windows x64 上執行，ARM64 在 `windows-11-arm` 上執行。驗證器要求原生主機架構、提交與檔案摘要相符，並檢查案例清單完整；外部執行器的結果另行標記。這組程序測試不代表 ARM64 C++ 例外支援。

可透過 `NEVERD_TEST_MATRIX_NEVERC` 選擇矩陣的選用編譯器。NeverC 3389.1.5 在 x64/ARM64 × ELF、PE、Mach-O 的 66 項檢查中全部通過，包括 IR 與原生物件安裝及 CPU 執行。此編譯器版本不支援 x86 或 ARM32；這些目標使用 Clang，ELF 也可使用相符的 GCC 驅動程式。NeverC 編譯 PE 時，適配器加入 `-fno-autolink`：patch 在現有映像中解析參照，不另行連結一套 CRT。帶有 `.drectve` 連結契約的外部物件仍會被拒絕。

Windows 原生程序驗收 [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817) 的 90 次觀測全部通過：x86、x64 和 ARM64 各 30 次，涵蓋原始碼 IR、編譯物件與外部物件三條路徑。x86/x64 在 Windows x64 上執行；ARM64 在 `windows-11-arm` 上執行，使用同一批經過雜湊驗證的映像。同一輪的 16 個 PE/物件/trampoline 專項檢查與 143 個 Unicorn 目標矩陣檢查也全部通過，沒有跳過。FH3 案例繼續驗證七種 x64 C++ 執行場景。

原生生產語料固定 NeverC `182fd2c5dc`，搭配獨立原程式與目前 NeverD 安裝器。`check_source_patch_hosted_pe_runtime.py` 每種 Windows 架構要求 36 次 C IR/物件/hosted 與兩層 hosted C 展開觀測；`check_source_patch_elf_runtime.py` 每種 Linux 架構要求 66 次 C、hosted 傳播及九種 Clang C++ 例外情境觀測。憑據、摘要及真實主機架構必須相符；不表示 CI 主機執行了 NeverC 編譯器本身。

舊翻譯設定的 204 次觀測僅為歷史記錄，不能證明目前純 C 契約。目前三條 NeverC 路徑均使用 C，Clang C++ FH3 獨立驗證。macOS x64 使用 Rosetta 2；Unicorn/QEMU 的 CPU 結果不能替代原生作業系統與例外展開驗證。