**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 1eae101089e730c29ba3fda92db515fe148fe41b758d6f5b69156252591d9b3f -->

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
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++；Mach-O x64/ARM64 C/C++；PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | 原生物件 | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | 原生物件 | ELF x64/ARM64 C/C++，需要相容的 Itanium 執行階段; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
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
## 原實作回呼、配置擴充與新增相依項

繫結對應外部宣告。`function` 驗證原函式或匯入樁；`data` 要求可寫儲存；`const` 要求常數 IR 宣告。物件資料繫結必須給出非零範圍，不得跨越儲存邊界或授予 RELRO 寫入權限。呼叫被替換函式的原實作時，必須明確使用 `original`。公開入口仍進入替換函式；儲存的入口執行按完整指令重定位的前綴，再接回原函式後續部分。原始碼、外部 IR 與原生物件共用此契約，包括浮點與堆疊參數及多個回呼。

回呼必須驗證函式邊界、指令模式、繼續執行位址與展開證據。未知指令、不安全的 PC 相對參照、跳入複製前綴的分支、不支援的載入重定位、重疊回呼與未建模的含處理器序言，皆會在發布前失敗。呼叫端仍須提供 ABI 相符的宣告。憑據以 `original_callbacks` 記錄儲存的入口位址。

PE32/PE32+ 可移動檔案內的儲存範圍並更新已建模的偵錯、符號、憑證及附加資料指標，擴充已滿的區段標頭表，同時保留原 RVA。Mach-O x64/ARM64 PIE 在相鄰 page-zero 保護區與末端 linkedit 區段符合條件時，可透過經檢查的標頭前綴擴充取得命令與展開儲存，原程式碼／資料位址不變。此設定不要求 `-Wl,-headerpad,0x400` 或相鄰的空閒展開頁。明確標頭位址參照、未知載入命令、混合程式碼／資料歸屬、損壞範圍與不支援的配置會被拒絕。安裝前統一規劃標頭空間與展開容量。

新程式碼維持 RX，可變狀態維持 RW。明確的 `--dependency` 請求透過 ELF 動態中繼資料、PE 匯入描述元／IAT 或 Mach-O dylib／bind 中繼資料，新增函式或資料匯入。原匯入身分、序號與版本記錄維持穩定；槽位與可呼叫樁保留各自含義。程式庫名稱／路徑須符合目標 OS 載入器規則。下例使用 ELF 共用程式庫名稱。執行時缺少程式庫或符號，必須產生實際載入器錯誤。TLS、IFUNC、新符號版本契約、載入期建構子與未建模重定位仍不支援；ELF `.data.rel.ro` 與 CET/IBT 仍需額外契約。 原生 COFF 物件須以 `__declspec(dllimport)` 宣告新增函式／資料匯入，使重定位明確指向 IAT；一般直接外部參照會被拒絕。

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

追加式、依結構大小檢查的相依項選項適用於原始碼、外部 IR 與原生物件請求。憑據包含 `added_imports`；重新載入候選檔案後，再核對全部回呼、匯入、儲存權限與安裝位元組。失敗會保留輸入及任何既有目標檔案。

<!-- i18n-section: examples -->
## 可執行範例

[完整 Windows/macOS/Linux 範例](../../examples/source-patch/README.md) 包含原程式、替換函式與相依程式庫原始碼、建置命令，以及真正宿主中的 Python SDK 外掛。請在儲存庫根目錄執行，使用已啟用 Python 的 NeverD 建置與新的輸出目錄。

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

預設執行兩條路徑；可加 `--api cli` 或 `--api sdk` 單獨選擇。目標位址會自動尋找。輸入 `7` 時，原程式輸出 `8`，兩種補丁皆輸出 `36`。驅動程式檢查實際執行、憑據及缺少程式庫的載入失敗；完整建置與 patch 命令儲存在 `commands.json`。

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
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## 建置與驗證

六個選定的原生 OS／ISA 工作皆零失敗、零跳過：3669 次 GoogleTest 執行、1008 次原生行程觀測（504 次功能、504 次 C++ 例外）、12 次實際載入拒絕、96 個映像及 84 份發布憑據。

| 主機 | 原始碼 | Harness | CI 工作 | 測試 | 觀測／拒絕 |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

這些產物僅驗收下表的實際原始碼與 harness 提交；後續 dev 整合及文件提交另有本機與文件檢查。早期失敗或取消的工作仍保留為失敗。歷史 216 次補充觀測、PE ARM32 及舊 32 位元 Apple 的 CPU／相容層結果，不構成新能力的原生硬體或 C++ 例外證據。

Windows ARM64 驗收使用原生 ARM64 Unicorn C DLL：以固定子模組與已驗證的 LLVM-MinGW 工具鏈實際建置，產生 MSVC 匯入程式庫並明確覆寫 CMake 目標。隔離的建置副本驗證三項主機修正：非 SEH 跳躍函式對、具指令快取同步的 ARM64 頁面提交回呼，以及適用 Windows LLP64 的 64 位元立即數遮罩。子模組原始碼及版本不變。這證明 CPU 矩陣測試主機可用，不表示預設 MSVC ARM64 JIT 已受支援。五項主機復原檢查與補丁行程的原生執行、C++ 例外證據分開記錄。

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

原生生產語料固定 NeverC `182fd2c5dc`，搭配獨立原程式與目前 NeverD 安裝器。`check_source_patch_hosted_pe_runtime.py` 每種 Windows 架構要求 36 次 C IR/物件/hosted 與兩層 hosted C 展開觀測；`check_source_patch_elf_runtime.py` 每種 Linux 架構要求 120 次 C、hosted 傳播及九種 Clang C++ 例外情境觀測。憑據、摘要及真實主機架構必須相符；不表示 CI 主機執行了 NeverC 編譯器本身。 Linux 清單在 x64、ARM64 分別涵蓋 Clang IR、Clang 原生物件與 GCC 原生物件各九種例外情境；NeverC 仍僅處理 C。

舊翻譯設定的 204 次觀測僅為歷史記錄，不能證明目前純 C 契約。目前三條 NeverC 路徑均使用 C，Clang C++ FH3 獨立驗證。開發用 Apple Silicon Mac 上的 macOS x64 執行使用 Rosetta 2；Unicorn/QEMU 的 CPU 結果不能替代原生作業系統與例外展開驗證。

AArch64 指令重定位要求顯式 RELA 加數。隱式 REL 指令加數會在發布前遭到拒絕，不會把指令位元欄位當成一般資料。支援的 PREL64 資料重定位在 REL、RELA 下都保留完整有號加數。

純 C 生產端驗證 [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213) 已通過全部 312 次原生程序觀測：Windows x64、ARM64 各 36 次，Linux x64、ARM64 各 120 次。Linux 分別驗證 Clang IR、Clang 原生物件及 GCC 原生物件的九種例外情境；NeverC 的 IR、物件及 hosted DynCode 輸入皆為 C。

重新建置安裝器的驗證 [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600) 已通過 19 項 PE/物件專項檢查、全部 143 項 CPU 矩陣及 474 次 Windows/Linux 原生程序觀測。Windows x86 程序在 Windows x64 主機執行；x64、ARM64 使用相符主機。獨立的 [Linux 驗收](https://github.com/NeverSight/NeverD/actions/runs/38585759996) 已通過原有 108 項及額外 3 項 LLVM-C、3 項 Clang/GCC C++ 檢查，零失敗、零略過。

[合併後驗收 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097) 已在 Windows/Linux 的 x64 與 ARM64 原生主機上重新通過全部 312 次程序驗證。30 個最終映像與先前驗證的位元組完全一致；原始碼摘要及純 C 發布回執已更新並獨立核驗。

[0a6f6d768 的擴充 Linux 驗收](https://github.com/NeverSight/NeverD/actions/runs/38626866257) 執行了 429 項檢查，零失敗、零略過：原始 108 項、完整 Swift 55 項、CPU 目標矩陣 143 項、原生 Session 117 項、LLVM-C 3 項及 Clang/GCC C++ 3 項。該合併版本的本機完整二進位修補迴歸也通過全部 22,961 項。原生程序次數與 CPU 模擬驗證範圍分開統計。

[0a6f6d768 的 Windows 安裝器重建驗證](https://github.com/NeverSight/NeverD/actions/runs/38627581732)通過全部 182 項專項檢查和 474 次原生程序執行，零失敗、零跳過。獨立產物稽核核對了每個映像的雜湊、架構、情境和重複次數。隨後合入 ABI/展開修正的 `e1cac949f` 再次通過全部 22,961 項完整二進位 patch 回歸，零失敗、零跳過。這些結果遵守 NeverC 僅支援純 C 的約定；C++ 替換使用 Clang/GCC。

<!-- i18n-section: runtime-coverage -->
## macOS 與 32 位元程序驗證

程序映像工作流程要求在 macOS ARM64（`macos-15`）及 Intel x64（`macos-15-intel`）上原生執行。每種架構執行 96 次觀測，涵蓋 Clang C 原始碼／物件安裝、可寫入指標狀態、浮點／堆疊參數、九種 Clang C++ 例外情境，以及現有 NeverC 純 C 的 IR／物件／hosted 輸出。執行前嚴格檢查 Mach-O 簽章；安裝的 hosted 程式碼必須與編譯器負載逐位元組相同。

相同工作流程要求 Windows x64 上的 Windows x86 執行 30 次、Linux x64 上的 Linux x86 執行 48 次，並透過 QEMU 使用者模式執行 96 次 Linux ARM32 觀測，ARM 與 Thumb 各占一半。Linux 檢查包含符合目標的 GCC 物件。每個情境重複三次。報告區分 `native`、`compatibility`、`rosetta` 與 `qemu-user`，並記錄目標及實際宿主架構。Rosetta 不符合 Intel 原生執行要求。

全部 14 個 CPU 目標設定仍由 283 項原始碼／IR／物件矩陣涵蓋。PE ARM32、Mach-O i386、ARMv7 iOS 與 ARMv7k watchOS 目前具備 CPU 驗證，不據此宣稱原生 OS 載入器或 C++ 展開器驗證通過。NeverC 維持僅支援 x64／ARM64 純 C；32 位元編譯由 Clang 與符合目標的 ELF GCC 驅動負責。驗證器不會建置或修改 NeverC。

Windows x64 與 ARM64 也會各自執行 36 次 Clang C++ FH3 觀測。

Mach-O 的可寫入指標用例在執行時建立指標；靜態位址初始化仍需要尚未支援的執行期重定位註冊，並有明確拒絕的回歸測試。ELF 零大小 COMDAT 函式僅在簽名是其可執行節起點處的唯一函式，且 CFI 精確涵蓋整個節時才接受。節內偏移、共用、填補、無群組及缺少 CFI 的情況仍拒絕。

ELF ARM 與 Thumb 使用獨立設定。原始碼編譯器明確接收 `-marm` 或 `-mthumb`，原生函式繫結保留原函式的指令狀態位元。32 位元 ELF 程序使用具備實際動態重定位的 PIE 映像。ARM32 QEMU 執行會記錄明確的 glibc `--loader` 與 sysroot：QEMU 的 AUXV 計算不像 Linux 核心一樣處理遷移後的程式標頭。此路徑驗證 QEMU 下的 glibc 載入與執行，不代表 ARM32 實機或原生核心載入器驗證。

對於這些套用補丁後的 ELF 映像，Linux x86 與 ARM32 的 `qemu-user` 路徑都需要明確使用符合目標架構的 sysroot 中的 glibc `--loader`。Linux x86 的 `compatibility` 路徑使用 Linux x64 宿主核心的 32 位元相容支援，並與 QEMU 執行分別記錄；它不能證明在原生 32 位元 x86 主機上執行。

[`fcee81c9a5e2f20a71090ade23ae833175f64c59` 的程序驗證](https://github.com/NeverSight/NeverD/actions/runs/38694434082)通過全部 750 次觀測：相符主機上的 576 次原生執行、78 次 x86 相容模式執行，以及 QEMU 使用者模式下的 96 次 ARM／Thumb 執行。獨立產物稽核核對了全部 126 個映像的雜湊、執行身分與完整情境清單。[Windows 重建驗收](https://github.com/NeverSight/NeverD/actions/runs/38694628242)通過 194 項專項檢查和 474 次 Windows／Linux 程序觀測。[Linux 重建驗收](https://github.com/NeverSight/NeverD/actions/runs/38694638358)通過 444 項檢查，包含原來全部 62 項失敗和 46 項略過，零失敗、零略過。這些結果保留上述硬體與載入器驗證界線；NeverC 仍為純 C，未修改其儲存庫。

補充的 [ARM/Thumb 驗證](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263)在 `ubuntu-24.04-arm` 上執行了 `fcee81c9a5e2f20a71090ade23ae833175f64c59` 的 32 個 C 測試映像，全部 96 次觀測通過（ARM 與 Thumb 各 48 次）。Linux 核心透過 `/lib/ld-linux-armhf.so.3` 直接載入映像，未使用 QEMU 或明確指定的載入器命令。這屬於 ARM64 主機上的 `compatibility`，不是原生 32 位元主機驗證。測試入口提交 `3ad17ac9608d6bd9f5a4d44953ec375524e5730b` 與映像提交分別記錄。獨立產物稽核核對了映像雜湊、全部 30 份發布回執、完整情境及重複次數，並確認沒有符合這些映像的 `binfmt_misc` 直譯器規則。

另外執行的[託管 runner 探測](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719)確認，所提供的 Windows 11 ARM64 runner 以 `WinError 216` 拒絕啟動 ARM32 程式。在 macOS 15.7.9 與 Xcode 16.4 環境中，現有 SDK 無法連結 macOS i386 或 iOS ARMv7 程式。watchOS ARMv7k 程式連結成功，但未取得實體裝置執行證據。探測工作成功不等於這些平台通過驗證；其原生硬體覆蓋仍未補齊。NeverC 未被建置或修改。

補充的[個人 Actions Linux 驗證](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274)在完整 QEMU TCG ARM32 客體中通過了 96 次 ARM/Thumb 執行，客體使用 32 位元 Linux 核心和 glibc；同一客體內的 Wine 另通過 30 次 PE ARM32 執行。ELF 映像由客體核心直接載入，這是整機模擬，與需要明確載入器的 `qemu-user` 路徑分別記錄。ARM64 主機相容層上的 Wine 登錄安裝僅作為安裝證據，計入零次修補執行。客體內的 Wine 初始化及每次量測執行都必須在原定時限內成功。

[Apple 相容性驗證](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358)通過了 Linux 上 Darling 的 30 次 Mach-O i386 執行，以及 Intel macOS 上 touchHLE 的 60 次 ARMv7/ARMv7k 執行。每個原始映像以及原始碼 IR、編譯物件、外部物件替換結果均執行三次。獨立稽核驗證了這些補充執行中的全部 72 個映像、66 份修補發布紀錄和 216 次執行，包括 GitHub 產物大小、SHA256、精確提交及完整案例清單。Linux 輸入仍固定於 `fcee81c9a5`；PE 輸入產生於 `a542f97ba`，Mach-O 輸入產生於 `8f5f06762`。驗證指令碼的提交編號在對應執行中單獨記錄。

Mach-O 修正依所選函式認證 Thumb 模式，避免被映像整體的 ARM/Thumb 混合模式覆蓋；僅正規化有證據的 Thumb 匯出指標，並統一 ARM32 展開程式碼位址，不改變資料指標或範圍長度。整合 LLVM 原始碼建置在本機通過全部 283 項 CPU 矩陣檢查及 258 項例外契約檢查，零失敗、零略過。舊 Mach-O 測試程式使用已記錄的開源連結器並預留展開中繼資料容量；i386 使用正確對齊的啟動入口和 64 KiB 頁零區段。

這些結果證明指定模擬器及相容層下的 C 程式碼執行。Wine 不等於 Microsoft Windows，Darling 不等於 macOS；iOS HLE 中的 ARMv7k 執行不能證明 watchOS、WatchKit 或 Apple 實機支援。不能據此推斷舊 Apple SDK 或原生 32 位元硬體涵蓋率，這些測試也不認證 C++ 例外展開。既有原生與相容模式證據繼續分開記錄。NeverC 未建置、未修改。
