**語言**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← 文件索引](README.zh-TW.md)

# 測試 NeverD

NeverD 的測試回答三個不同問題：表示形狀是否符合預期、完整 pipeline 路徑能否
處理二進位 fixture，以及產生的程式碼是否維持行為。先選擇能回答本次變更問題的
最小套件；對高風險提取請求，再執行較廣的彙總測試。

## 設定測試建置

除非啟用 `BUILD_TESTING`，否則不會建置測試。完整套件通常使用 Release；Debug
保留斷言與單步能力，但刻意不最佳化，不代表解碼基準效能。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

完整 fixture 集合要求 `clang` 能跨目標編譯，並要求 LLVM linker（`ld.lld`
與 `lld-link`）位於 `PATH`。CMake 無條件建置許多可重定位 fixture，並在存在
對應 linker 時建置已連結 ELF/PE fixture。因主機無法編譯或連結 fixture 而略過
的測試屬於未執行覆蓋，不代表該目標通過。

複製、建置設定與 macOS 預先建置 LLVM 說明見
[CONTRIBUTING.md](i18n/CONTRIBUTING.zh-TW.md)。

## 測試配置

`add_neverd_unittest` 建立一個 GoogleTest 可執行檔，並為每個發現的案例指定
與該可執行目標同名的 CTest 標籤。

| 原始碼區域 | 目標與 CTest 標籤 | 涵蓋內容 |
|------------|-------------------|----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | 跨平台子行程呼叫、引號、重新導向與結束碼 |
| `unittests/libc` | `NeverDLibCTests` | 已知 libc 名稱與分類 |
| `unittests/safety` | `NeverDSafetyTests`、`NeverDSafetyIntegrationTests` | 匯目錄、身分優先序、參數預過濾、拷貝越界獵取、堆積生命週期稽核，以及強制執行的 PE/ELF/Mach-O × x86-64/AArch64 六單元矩陣 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR 形狀、IR 階段、loader、重定位、格式 fixture、反編譯與代表性 patch 流程 |
| `unittests/semantic` 中的大多數檔案 | `NeverDSemanticTests` | 指令、ABI、控制流、C 運算式與 lift/recompile 差分語意 |
| `unittests/evm` | `NeverDEVMOpcodeTests`、`NeverDEVMBytecodeTests`、`NeverDEVMLoaderTests`、`NeverDEVMAnalyzerTests`、`NeverDEVMSemanticTests`、`NeverDEVMEmitterTests`、`NeverDEVMIntegrationTests` | 硬分叉中繼資料、輸入正規化、CFG/SSA/還原、interpreter 語意、LLVM/C/Solidity 差分執行及公共 API routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFProgramImageTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFVerifierTests`、`NeverDSBFISAConformanceTests`、`NeverDSBFAgaveConformanceTests`、`NeverDSBFSemanticTests`、`NeverDSBFEmitterTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`、`NeverDSBFMalformedCorpusTests`、`NeverDSBFUpstreamConformanceTests`、`NeverDSBFExternalOracleTests`、`NeverDSBFSolanaModelTests`、`NeverDSBFIntegrationTests` | v0-v4 中繼資料與 ELF 配置、嚴格 verifier/loader 行為、23 個固定 ELF 成品、獨立 official oracle、完整 opcode 可用性、惡意輸入、CFG/還原及已執行的 LLVM/C/Rust 差分 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 四 ISA×三物件格式的重寫/混淆等價性 |
| `unittests/semantic` 中的聚焦轉換檔案 | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 從大型語意二進位拆出的快速重新連結探針 |
| `unittests/corpus`（submodule） | `NeverDWindowsEHCorpusTests`、`NeverDRustEHCorpusTests`、`NeverDGoEHCorpusTests`、`NeverDCxxItaniumEHCorpusTests`、`NeverDObjCEHCorpusTests` | 從 317 個釘住的真實二進位讀出的例外與執行期 metadata，每一個都在 manifest 裡宣告了其復原必須達到的下限 |

註冊的事實來源是
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) 與
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt)、
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) 與
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt) 和
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt)。

### 釘住的二進位 corpus

其他每個測試套件都自己建置被測對象，corpus 不是：它是一個 submodule，裝的是真實
工具鏈在本儲存庫搆不到的主機上、為搆不到的目標產出的二進位，每一個都按摘要釘住，
旁邊的 manifest 宣告了它的復原必須達到的下限。要回答「NeverD 從一個 `-O2`
stripped 的 `armv7` 共用程式庫裡到底讀出了什麼」這類問題，只有這裡給得出答案而不
是論斷。

這些套件只在 configure 被告知去找它們時才建置，所以這個開關就是它們是否受測的全部：

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` 跑全部產線；`check-neverd-windows-eh-corpus`、
`check-neverd-rust-eh-corpus`、`check-neverd-go-eh-corpus`、
`check-neverd-cxx-itanium-eh-corpus` 與 `check-neverd-objc-eh-corpus` 各跑一條。三個
CI 主機都帶著這個開關配置並跑全部五條產線：位元組到處都一樣，但讀位元組的東西不一
樣，在一台主機上跑通不能說明另外兩台。`scripts/audit_ci_test_inventory.py` 會拒絕缺
少五個標籤中任何一個的清單——建置悄悄不再讀 corpus 是一種沒有任何測試能捕捉的迴歸，
因為消失的正是那個測試。

EVM 操作碼稽核每次執行都會對官方
[go-ethereum repository](https://github.com/ethereum/go-ethereum)的 remote `HEAD` 執行
淺層 `git fetch`，並報告實際稽核的精確 commit。它會重複使用已忽略的 bare cache
`build/evm-opcode-audit/go-ethereum.git`，但在讀取封閉的操作碼清單與 byte assignment
之前一定會更新 cache：

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

CI 會在每次 push、pull request、手動觸發及每日排程中執行同一項線上稽核，因此即使
NeverD 沒有變更也能發現 upstream drift。離線測試或重現歷史版本時，才明確指定既有
checkout：

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

此稽核僅允許 `EVMUpstreamOpcodePolicy.def` 中明確列出的排除項；任何未表示或未經明確
review 的 upstream opcode 都會使命令失敗。parser 與 drift diagnostic 在 CI 中有獨立
Python unit coverage，可用下列命令執行：

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

修改 EVM control flow 時，先執行 fixed-point 與 height-domain contract：

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

這些案例涵蓋跨基本塊 internal return、有限 multi-target merge、loop convergence 與
deterministic edge ordering、path-dependent stack height、bounded widening、correlation
造成的 Cartesian over-approximation、unknown jump、exact invalid target，以及 strict
與 relaxed stack fault。接著應執行全部七個 EVM binary 和 upstream metadata audit；
即使 analyzer 的局部形狀正確，CFG 修改仍可能影響 emitter 與 integration 行為。

修改 MedIR/HighIR dataflow 時，另需執行 constant-phi、selector、typed-operand、
malformed-graph 與 deep-chain contract：

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

這些案例驗證相同與衝突的 cyclic phi、不相鄰及跨基本塊的 selector expression、等式兩種
operand order、exact ABI width check、typed storage/event/calldata operand、malformed
MedIR 的 deterministic handling，以及針對 16,384 個值的 iterative producer walk。

## fixture 如何產生

### Lift 與格式 fixture

`unittests/lift/CMakeLists.txt` 在建置期間跨目標編譯 C 與組合語言原始碼。Clang
target triple 產生 x86-64、i386、AArch64、ARM32 ELF 物件、PE/COFF 物件與
已連結映像，以及 PIC/no-PIC Mach-O i386 物件。存在 LLD 時，選定物件還會連結為
patch 測試所需的可執行檔。`NeverDLiftTests` 依賴 `lift-test-objects` 目標，
因此正常建置該測試二進位會更新產生的 fixture。

多數 lift 測試使用 `NeverDLiftFixture.h` 呼叫建置出的 `neverd` CLI，並檢查
LowIR、MedIR、HighIR、LLVM IR、產生的 C 或重寫後的二進位。聚焦手動實驗可用
`NEVERD` 環境變數覆寫 CLI 路徑；一般 CTest 執行使用 CMake 內嵌的可執行檔。

### 記憶體安全 fixture

`unittests/safety/fixtures/binaries` 檢入了 x86-64 與 AArch64 的 PE、ELF、
Mach-O 映像，以及各格式對應的 PDB 或 dSYM 伴生檔，每個映像還附帶一份連結器
MAP。MAP 是被 strip 的建置唯一還會留下的身分資訊，因此每個單元還會明確指定
MAP 再分析一次，用來釘住在既無型別也無原始碼行號時結論還能說什麼。
`NeverDSafetyIntegrationTests` 在每台主機上執行全部六個單元；任何必需映像或
伴生檔缺失都會在設定階段失敗，測試不存在依宿主工具鏈跳過的路徑。

六個等價二進位來自同一個原始檔。`make` 只重建宿主原生 smoke fixture；完整
矩陣用：

```bash
make -C unittests/safety/fixtures matrix
```

完整重建需要 Clang 的 Linux／Windows 交叉目標、LLD COFF 工具、兩個 Darwin
架構與 `dsymutil`。規則會重新映射除錯路徑並關閉 CodeView 命令列記錄，避免
檢入的伴生檔捕捉開發者工作區絕對路徑。

### Windows 例外重建

修改 Windows 表格驅動例外時，既要測試表示層，也要對已連結 PE 執行 patch 測試。
下列聚焦 lift-suite 篩選器涵蓋正規化 unwind/SEH/C++ 模型、損壞輸入處理、
例外 CFG 邊、HighIR、LLVM WinEH 產生、例外目錄替換，以及 Guard CF/EH
continuation 重建：

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

受保護的 x64 組合語言 fixture 需要 Clang Windows target 與 `lld-link`；其 CMake
連結使用 `/guard:cf` 與 `/guard:ehcont`。因缺少交叉連結器而 skip，不能作為
final-image 路徑的有效證據。整合案例通過後，才證明重寫後的 PE 可以重新載入，
且 runtime-function、unwind、load-config、Guard CF 與 Guard EH continuation
表維持排序、由檔案承載，並只指向可執行目標。

已連結的 FH3 fixture 獨立涵蓋原生 C++ closure：固定狀態表、HighC 註解、
personality 保留、產生的 catch 目標，以及重新載入後的 IP-to-state 圖。

分析/原生支援矩陣與 fail-closed patch 契約請見
[Windows 例外重建](windows-exception-reconstruction.zh-TW.md)。

### 語言例外模型

除 Windows 表模型以外的一切都集中在一個聚焦 target 中。
`NeverDLanguageEHTests` 涵蓋 DWARF 框架鏈、Itanium 語言特定資料區、ARM EHABI、
Darwin compact unwind、Go 執行期框架中繼資料、Rust panic 機制，以及三種
Objective-C 執行期：

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

本套件中的表是逐位元組手工組裝而非編譯出來的，因為其中大多數要驗證的組合
沒有任何單一工具鏈會同時產出。Objective-C 是最典型的例子：三種執行期都發出
Itanium LSDA，差別只在型別表槽位裡放什麼——而這個差別是徹底的，不是程度問題。
Apple 的槽位指向 `objc_typeinfo`，其前兩個欄位刻意模仿 `std::type_info`；
GNUstep 的 Objective-C++ 槽位指向真正的 `std::type_info` 子類別；GNU 執行期的
槽位根本不是指標，而是類別名字串本身。把一種執行期的約定套到另一種的表上
不會報錯，只會報出一個從別的東西中間讀出來的類別名——所以在讀任何槽位之前，
先由框架的 personality 確定執行期。

同一套件還釘住兩個容易混為一談、但混淆即錯誤的區分。`@catch(id)` 與
`@catch(...)` 是不同的處理器——前者接收任意 Objective-C 物件，並放外來例外
從旁邊繼續傳播——而每種執行期對兩者的拼寫都不同，所以把兩者都報成 catch-all
的解碼器，等於給那些本會飛過去的例外安上了處理器。另外，setjmp/longjmp 的
call-site 表索引的是呼叫點序號而不是位址，因此沒能認出某個 SJLJ personality
的讀取器不會報錯，而是會憑空造出程式從未指定過的保護區間和 landing pad。

認出這種形式，和拒絕解碼它，是兩回事。一條 SJLJ 條目是一對 ULEB128 值——
一個派發選擇子和一個動作偏移——而這個動作偏移在此處的含義與位址形式中完全
一致，所以動作鏈、catch 型別、例外規格，全都能從一張根本不指名任何程式碼的
表裡讀出來。唯一讀不出的是每條條目守護的區間，因為說明它的是函式自己對
call-site 槽位的寫入，而不是表裡的任何東西。該套件還釘住了此處唯一不可信的
那個位元組：GCC 把 call-site 編碼寫成 `DW_EH_PE_uleb128`，LLVM 寫成
`DW_EH_PE_udata4`，兩者隨後都照樣發射 ULEB128，而沒有任何 personality 會去
讀它——所以解碼器也不許讀。

personality 身分同樣在這裡釘住，因為它決定了上面每張表該怎麼讀。GNAT 用
GCC 給每個前端的那三種拼法命名自己的常式——`_v0`、`_sj0`、`_seh0`——並且在
Windows 上註冊一個符號卻轉發到另一個，所以這四種拼法都必須落到 Ada 上。D
則是鏡像的情形：三個編譯器，同一個常式的三個名字，背後是同一套表。

### Unicorn 差分往返

語意 fixture 測試行為而非文字形狀：

1. 撰寫小型 C/組合語言案例，或建構 LLVM IR。
2. 使用 Clang/LLVM 為要求的目標編譯。
3. 在 Unicorn 中執行原始機器碼，並擷取預期回傳值或 fixture 定義的其他狀態。
4. 透過 NeverD 載入並提升，發射 LLVM IR，再把結果編譯回機器碼。
5. 以相同 ABI、輸入、記憶體配置與 CPU 模型執行重新產生的程式碼。
6. 比較可觀察結果。

主要實作位於
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h)。
patch-full fixture 使用 `Codegen::compileForRewrite`（與 patch 操作相同的重寫
backend），接著在完整 4×3 ISA/格式網格中比較基準與轉換後程式碼。

確定性的 NeverD 語意失敗應成為失敗測試。略過只適用於明確的外部能力邊界，並應
閱讀 skip 原因：缺少跨目標 linker 的綠色摘要不能證明該格式路徑實際執行。

### EVM 差分後端

EVM interpreter test 提供確定性 256 位元 oracle。emitter suite 直接編譯執行生成
LLVM，以 Clang lower 生成 C23 並在同一 host harness 執行；安裝 `solc`、`anvil`、
`cast` 與 `jq` 後，亦將 generated Solidity harness 部署至本機 Anvil。測試比較
status、storage 與 instruction trace count。獨立 raw-bytecode corpus 直接於 Anvil
native EVM 執行 pre-Fusaka scalar ALU、calldata/memory copy、重疊 `MCOPY`、Keccak
與 return data。

`NeverDEVMOpcodeTests` 亦約束 metadata architecture：全部 150 opcode 於 byte encoding
與 typed value 間 roundtrip，測試 family helper boundary 和 hardfork alias，完整 stack
contract 與 host argument maximum 保持推導而不在 backend 重複。

### Solana SBF 差分後端

SBF 中繼資料測試會驗證每個版本特性、操作碼衝突邊界、Murmur3 syscall hash、重定位、ELF machine、暫存器和 VM 位址常數。Loader fixture 不依賴 vendored 二進位，直接產生舊式 v0-v2 section 配置和無 section 的嚴格 v3/v4 program-header 配置。

`NeverDSBFISAConformanceTests` 依 v0-v4 的每個版本，將每一種 byte encoding
與獨立稽核的 typed manifest 比對。`NeverDSBFExternalOracleTests` 接著把 activation
與 boundary 決策和另外建置的官方 Anza 程序比較。
`NeverDSBFUpstreamConformanceTests` 為固定 Anza revision 中的全部 23 個 ELF
指定明確結果。

`NeverDSBFSemanticTests` 直接執行已驗證的指令位元組而不取用 MedIR，因此修改或破壞正規化 IR 不會讓來源 oracle 與後端意外達成一致。涵蓋範圍包括非單調的 v2 語意、記憶體、syscall、內部呼叫框架、fault、trace 和資源限制。LLVM module 會被驗證；產生的 C 以 warnings-as-errors 編譯，Rust 使用 `-D warnings`。公共 API 測試從產生的嚴格 SBF ELF 出發，走過所有 IR 階段、反組譯、CFG、中繼資料、LLVM、C 與 Rust。

## 一次性目標

自訂目標會建置相依，再以主機 CPU 推導的平行度執行 CTest：

| CMake 目標 | 選擇範圍 |
|------------|----------|
| `check-neverd` | 所有已註冊測試 |
| `check-neverd-semantic` | 僅 `NeverDSemanticTests` |
| `check-neverd-sbf` | 所有 `NeverDSBF*Tests` 目標/案例 |
| `check-neverd-patch-full` | 僅 `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | 僅 `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | 僅 `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | 僅 `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` 與 `NeverDAvxUpperXformTests` 目前沒有
`check-neverd-*` 便利目標；請依下文先建置，再以標籤選擇。
`check-neverd-semantic` 也不包含個別的轉換或 patch-full 二進位；完整彙總應使用
`check-neverd`。

## 增量 CTest 工作流程

先建置所屬可執行檔，再選擇其標籤。這可避免重新連結無關的大型語意目標。

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# 所有聚焦的 EVM 目標/案例
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# 所有聚焦的 Solana SBF 目標/案例
cmake --build build-release --target check-neverd-sbf --parallel 4
```

使用 GoogleTest 衍生的 CTest 名稱執行單一迴歸：

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

常用選擇器：

| 命令 | 用途 |
|------|------|
| `ctest --test-dir build-release -N` | 列出已發現案例而不執行 |
| `ctest --test-dir build-release -L '<regex>'` | 選擇測試二進位標籤 |
| `ctest --test-dir build-release -R '<regex>'` | 選擇案例名稱 |
| `ctest --test-dir build-release --output-on-failure` | 只為失敗顯示診斷 |
| `ctest --test-dir build-release --stop-on-failure` | 第一個失敗後停止 |
| `ctest --test-dir build-release --parallel 4` | 最多平行執行四個案例 |

GoogleTest 發現使用 `DISCOVERY_MODE PRE_TEST`，因此 CTest 列舉前必須存在對應測試
二進位。每案例 timeout 與獨立發現 timeout 定義在 `cmake/AddNeverD.cmake`，
只有存在量測到的重型案例時才應放寬。

## 哪些測試應隨程式碼變更？

| 變更區域 | 從這裡開始 | 接著考慮 |
|----------|------------|----------|
| 架構 lifter 或 decode | `NeverDLiftTests` 中的命名案例 | 對應 ISA 語意往返 |
| LowIR CFG、函式偵測、跳躍表 | Lift CFG/switch 案例 | `NeverDSwitchXformTests`、`NeverDCFGLoopXformTests` 或 `NeverDTwoTableXformTests` |
| MedIR、ABI、旗標、型別、SSA | MedIR/呼叫慣例 lift 案例 | 跨 ISA 的 `NeverDSemanticTests` 案例 |
| HighIR 或結構化 C | HighIR/decompile 案例 | `NeverDCFGLoopXformTests` 與產生 C 編譯檢查 |
| PE/ELF/Mach-O loader 或輸入重定位 | 對應的 `unittests/lift` 格式 fixture | 該單元格的全階段載入/反編譯測試 |
| 重寫 codegen 或輸出重定位 | `RewriteCodegenRTTests` 案例 | `NeverDPatchFullTests` 與存在時的已連結 patch fixture |
| patch 使用的 LLVM IR 轉換 | 聚焦轉換二進位 | `NeverDPatchFullTests` 組合 pass 網格 |
| C API 或 CLI | 直接 SDK/query 測試與 `unittests/semantic/CLIEndToEndTests.cpp` | 相關 pipeline/格式套件 |
| EVM loader、opcode、IR 或 backend | 最小的所屬 `NeverDEVM*Tests` 目標 | 所有 EVM 目標，以及產生 C/Solidity 的編譯檢查 |
| SBF loader、ISA、IR 或後端 | 最小的所屬 `NeverDSBF*Tests` 目標 | 所有 SBF 目標，以及產生 C/Rust 的編譯檢查 |
| Libc 辨識 | `NeverDLibCTests` | 行為變更時的語意 call/ABI 案例 |
| 堆積生命週期稽核或拷貝越界獵取 | `NeverDSafetyTests` | `NeverDSafetyIntegrationTests` 的全部六個單元 |
| 行程執行或 quoting | `NeverDTestProcessTests` | 每個支援主機上的一個受影響 CLI/語意案例 |

測試應在最低穩定邊界表達契約。LowIR 形狀測試適合歸因至 lifter；若兩種看似
合理的 IR 形狀可能行為不同，則必須使用語意往返。若小型 opcode、CFG 或可觀察狀態
斷言已足夠，應避免儲存整個函式的 golden dump。

## 與 CI 的關係

CI 在 Linux、macOS 和 Windows 上以 Release 開啟測試建置，先稽核發現的測試清單，
再套用平台特定標籤排除。設定定義於 `.github/workflows/ci.yml` 與
`scripts/audit_ci_test_inventory.py`。每個矩陣主機都必須包含 `NeverDSafetyTests`
與 `NeverDSafetyIntegrationTests`，而且每次都讀取同一組已檢入的 PE、ELF、Mach-O × x86-64、AArch64 fixture。由於沒有單一矩陣 shard 代表所有昂貴套件，當機器具備全部跨目標工具時，本機 `check-neverd` 仍是最清楚的完整合併前訊號。

## 目前 Solana SBF 一致性與 sanitizer profile

本節的目前清單取代上方較短的 SBF 清單。source differential suite 除 clang 外還
需要 `rustc`；compiler skip 代表 coverage 缺失。完整 aggregate 包含
`NeverDSBFProgramImageTests`、`NeverDSBFMalformedCorpusTests`、
`NeverDSBFISAConformanceTests`、`NeverDSBFUpstreamConformanceTests`、
`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`，以及 metadata、
loader、analyzer、semantic、emitter、integration target。integrated profile 記錄
命名 target 與結果，不固定快速變動的匯總 case 數。

sanitizer profile 分開建置於 `build-sbf-asan-ubsan`。focused target 以 fail-fast
方式執行且沒有 ASan/UBSan report；prebuilt package 缺少必要的
fork-only header，因此 integration 在 integrated LLVM build 執行。

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```

### 固定的 SBF 證據快照（2026-08-24）

gate 將 Anza `sbpf` 固定於
`2510663bb8d894e8e3094be351e4bb4b604f1f84`、Agave 固定於
`ef210d67f2fabeee1730498188fa78854260c679`、Solana SDK 固定於
`122f32e571ce39face4beffaccea733e37c207fd`。官方 ELF manifest 全部 23/23 通過；
`NeverDSBFExternalOracleTests` 經 `SBFOfficialOracleProtocol.def` 與
`SBFOfficialVerifierCases.def` 與 `SBFOfficialExecutionConstants.def` 對照
1,411 個 opcode/verifier boundary case。
`SBFOfficialELFMutations.def` 是畸形 ELF 的 table-driven contract；總數仍會演進，
因此不固定。
另有獨立的 `41-case strict ELF differential`，將完整 strict-v3 mutation matrix 送入
官方 `verify-elf-batch` 與 NeverD；這 41 個 case 不計入 1,411 總數。
`NeverDSBFAgaveConformanceTests` 驗證 Firedancer test-vectors 的
`68bb4af40235562e8852fa23d5727e49c2a0b862`，比對全部 1,955 `sol_compat_elf_loader_v1` 個 loader fixture
（接受 1,399、拒絕 556），並針對每個接受的 ELF 比較 `entry_pc`、`text_off`、`text_cnt`、
`rodata_hash` 與 `calldests_hash`。此 gate 不執行後續 instruction verifier。

額外的官方執行矩陣分開統計：恰有 508 個 active `(Version,Opcode)` case，另有
58 個 boundary case，共 566 個 exact execution case。它既不取代、也不計入
1,411 個 verifier probe 或 `41-case strict ELF differential`。
Linux Release CI 使用 `--print-pinned-revision`、`--print-test-vectors-revision` 與
`--print-toolchain`，並匯出 `NEVERD_SBPF_ORACLE` 和
`NEVERD_AGAVE_CONFORMANCE_ROOT`，因此兩個 external gate 都強制執行；一般本機執行
未提供明確 oracle/corpus env 時仍會發現 case，但允許 skip。

`SBF_RUNTIME_VERSION` 讓 `RuntimeVersionPolicy::ChainProfile` 依歷史 cluster/slot
計算：官方 feature account activation 使最大 ISA 由 V0 依序推進至 V1、V2、V3；
目前仍為 V3。明確 v4 使用 `RuntimeVersionPolicy::UpstreamToolchain` 做 offline 分析。
目前 10 MiB 上限精確為
`10'485'760` byte；65,536 僅是歷史 provenance/test。`SBFFaultCodes.def` 固定
execution fault 的穩定值；`SBFSourceStatuses.def` 獨立擁有 generated-source ABI。

10,000 規模 fixture 守護 worklist、function ownership 與 multi-latch，不固定特定機器
的耗時。cluster/account/slot row 支援 `RPC activation audit`，一般測試仍保持
deterministic 與 offline。
