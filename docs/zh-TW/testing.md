**語言**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← 文件索引](README.md)

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
[CONTRIBUTING.md](CONTRIBUTING.md)。

## 直譯器還原檢查

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource)Tests$' \
  --output-on-failure
```

```sh
cmake --build build-release --target NeverDLowIRUndefinedIndependenceTests \
  NeverDOriginalBinaryUndefinedIndependenceTests \
  NeverDX86UndefinedEffectsTests NeverDX86CarryArithmeticFlagTests \
  NeverDX86LogicIdentityTests NeverDX86NoIndexAddressTests --parallel 4
build-release/bin/NeverDLowIRUndefinedIndependenceTests
build-release/bin/NeverDOriginalBinaryUndefinedIndependenceTests \
  --gtest_filter='OriginalBinaryUndefinedIndependence.*'
build-release/bin/NeverDX86UndefinedEffectsTests
build-release/bin/NeverDX86CarryArithmeticFlagTests
build-release/bin/NeverDX86LogicIdentityTests
build-release/bin/NeverDX86NoIndexAddressTests
```

恢復 API 測試涵蓋 v1/v2/v3 預設值、明確預算、截斷結構、各層 reserved 欄位與未來尾部相容性。CLI 測試在兩種 ABI、兩個原始碼後端下檢查欄位／查詢預算耗盡及成功恢復，拒絕非法十進位上限，並要求 `--devirtualize`。預算耗盡不得發布原始碼或部分殘餘圖。

v4 測試固定前綴大小與填充，拒絕截斷配置和未知旗標，保留舊介面/未來擴充行為，並驗證未要求報告時 C 中仍保留有號範圍。獨立 CLI 用例分別要求串接以保留關聯、要求範圍以證明無號堆疊比較；兩個 C 後端皆在 O0/O2 下執行並啟用未定義行為陷阱。關閉探索必須改變依賴探索的恢復結果。解析測試涵蓋零串接、整數極值、溢位、錯誤範圍與缺少前提。Python 檢查配置、旗標、簽章與具有所有權的失敗報告。

`NeverDByteMemoryForwardingTests` 涵蓋重疊寫入的最後寫入者、兩種位元組順序、直到 i128 的整位元組寬度、已定義值、保持關聯的 undef/poison 快照及部分覆寫。反例在未知別名、位址空間轉換、呼叫、有序存取、生命週期變更、缺少位元組、動態或無效位移、分支及迴圈處保留讀取。測試也涵蓋大量輸入的 PHI、零預算、精確預算、預算耗盡及預設拒絕快照。原始與改寫 LLVM 在 O0/O2 下對照獨立算術基準執行；既有 MBA、LLVMC 及直譯器原始碼測試保護管線相容性。

`NeverDMedMutableSourceTests` 和 `NeverDLLVMCValueTests` 在 O0/O2 下執行獨立撰寫的迴圈、區塊重排、入口回邊、執行期堆疊運算、較早讀取、分支匯合、部分別名、布林真值和包含零輸入的位元計數。反例要求在發射前拒絕畸形輸入、截斷目標、歧義承載和預算耗盡。超過 SSA 限制的 CLI 案例要求 LLVMC 輸出可執行，並要求 HighC 明確拒絕。 連續更新和跨區塊儲存運算式鏈也會檢查產生 C 的大小與執行結果。

複合條件回歸在 O0/O2 下執行包含非零常數相等、無符號比較、兩種運算元順序的有符號比較、擴寬布林輸入及全部布林否定組合的合取和析取。C 發射必須保留完整真值表，且不得解引用不存在的零比較運算元。整數位址儲存涵蓋對齊與未對齊的 32/64/128 位元承載；位元組儲存陣列保留明確對齊及精確的首位址與部分存取，不得產生純量對陣列賦值或不相容型別的別名存取。

`NeverDLowIRRefinementTests` 涵蓋實際恢復的殘餘圖、不同結構的有限迴圈、零次迭代、獨立動態產生者、條件見證、重疊輸入視圖、複製與溢出關聯、兩邊不可變讀取證據、強制系統旗標及返回槽保留。錯誤候選、額外寫入、不完整或無限路徑、過期證據、暫存區衝突與共享預算耗盡必須拒絕證書；既有獨立性測試仍拒絕可觀察的任意值。

同一目標中的 `LowIRLoopRefinement.*` 和 `BinaryLowIRLoopRefinement.*` 涵蓋任意 64 位元計數、巢狀字典序排名、真實原生殘餘程式碼、入口前綴範本、重疊視圖及相關溢出。負例拒絕錯誤迴圈本體、縮小入口域、不下降的排名、無號回繞、遺忘先前寫入、遺漏切點、畸形範本和共用預算耗盡。成功的有限分支不能授權不完整的歸納證明。

`LowIRLoopInference.*` 和 `BinaryLowIRLoopInference.*` 使用獨立編寫的計數器、堆疊儲存、提前返回、原生呼叫和封裝旗標案例，涵蓋窄位元算術拓寬，以及運算式不同但語意相等的旗標狀態。格式錯誤的圖、缺失或偽造的來源、不終止／回繞迴圈，以及推導或證明預算耗盡均不得產生憑證。

共用入口與共用回跳區塊的迴歸涵蓋零擴展的 32 位元及完整的 64 位元計數器、非單位步長純量排名、錯誤結果、不進展與回繞路徑，以及純量和組合排名搜尋之間恰好足夠或耗盡的累計預算。`LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`。 另有遞增與重設迴歸，要求不必每輪只展開一個計數器位元即可收斂，並拒絕缺乏進展和無符號回繞。 排程迴歸涵蓋迴圈攜帶的非單位步長累加器、有效非單位步長純量排名旁可能回繞的單位計數器，以及有效組合排在早期視窗之後的三個計數器。恰好足夠和少一次的排名預算檢查確定性的繼續搜尋，並防止重複候選。

同一目標中的 `LowIRLoopPlanPairing.*` 檢查暫存器重新命名、不同算術主體、雙方獨立前綴快照、述詞保留、共用框架輸入、巢狀切點覆蓋和獨立證明預算。缺少關係、錯誤寫入、無效暫存值繫結、不完整配對或中繼資料預算耗盡均不得產生憑證。

`LowIRLoopAlignment.*` 使用獨立編寫的一般與旋轉框架計數迴圈：兩個預設自關係計畫各自證明成功，首次配對失敗，另一個候選切點則證明關係成立。迴歸涵蓋多切點排列、錯誤結果與框架寫入、缺失或過期的原始記錄、明確的未定義值 witness、不遞減或回繞的計數器、格式錯誤的圖、失敗嘗試的累計查詢、恰好足夠的總預算及搜尋限額耗盡。任何拒絕結果都不得包含憑證。 新增案例涵蓋分離的重設與進展階段、移動退出判斷後需要跨族配對的等價迴圈、不重複推斷的共用快取，以及快取中繼資料合計超限。後接獨立迴圈的案例以明確的 16384 次查詢上限驗證完整週期覆蓋。空候選族與重複候選族不消耗符號查詢，切點不足必須拒絕。同時檢查恰好足夠與少一次的全域預算、錯誤結果、缺失進展、原始證據及未定義值 witness。 過濾候選族回歸涵蓋保持結果不變的算術菱形、局部匯合與僅在邊界匯合，以及能繞過可達匯合點退出或返回迴圈邊界的路徑。測試原始過濾族重複時仍嘗試候選過濾族、先成功的過濾計畫由後續完整分支嘗試重用、恰好足夠／少一次／零 `MaxCutSelectionWork`、失敗後累計的 `CutSelectionWork`，以及全域圖工作預算耗盡後不啟動符號推斷。兩種分支候選族都驗證完整週期覆蓋；菱形關係使用明確的推斷與證明查詢上限。

局部計數器回歸涵蓋框架和暫存器、遞增和遞減、低位／中間位／高位、非標準寬度、兩種位元組順序及三位元組框架字。測試檢查延遲發現新區間、竄改保留位元、不進展和無保護回繞、無效新增入口、恰好足夠／不足的推導預算，以及既有單切點搜尋。

前置階段回歸涵蓋兩個及三個順序迴圈重用同一倒數計數字、與原有巢狀迴圈階段組合、恰好足夠與少一次的排名／查詢預算、不進展迴圈，以及重設回前一階段。同寬但錯誤的階段常數、錯誤結果及框架寫入必須由完整檢查器拒絕且不產生憑證；缺失原始證據仍回報不支援。

`NeverDLowIRRefinementTests` 中的 `InterpreterMachineStateModel.*` 使用獨立撰寫的 LowIR 案例，檢查原始入口旗標、狀態碼與客體 RAX 的區分、全部 17 個狀態字、部分暫存器分片、封裝旗標、動態拒絕狀態的持續保留、客體堆疊框架寫入、兩個分支以及循環推斷後的全新證明。錯誤輸出、遺失狀態、記憶體變更、過期指令記錄、非法輸入和產生預算耗盡必須失敗。既有機器狀態原始碼測試也涵蓋兩條 C 路徑的 O0/O2；模型測試本身不證明編譯後的 C。

`NeverDLLVMInterpreterModelTests` 將獨立編寫的 LLVM 與完整狀態 LowIR 參考實作比較，涵蓋位寬、平行 PHI、switch、客體記憶體、獨立狀態碼、poison 檢查、內建函式值域、被拒絕的契約及四種建模預算。測試完成任意字長倒數迴圈的完整證明，並拒絕遭改寫的狀態碼。獨立 C 用例經 O1/O2 編譯後必須滿足相同觀察契約。這些測試驗證受支援的模型；自動不變量發現與編譯器正確性仍是獨立義務。 變數位移用例涵蓋全部四種位寬、經遮罩或分支限制的位移量、邊界及越界位移量、無回繞與精確旗標、嚴格 poison 拒絕，以及 O1/O2 編譯後的 C。

初始化契約回歸涵蓋部分及分離的位元組範圍、固定別名、兩個分支、每個返回點、迴圈首輪讀取與迴圈內先寫後讀。先讀後寫、漏寫、客體寫入、未知別名、特殊記憶體存取、物件外範圍，以及輸入／工作預算耗盡，都必須失敗。獨立的僅輸出狀態字 C 案例在 O1/O2 編譯後保留精確 LLVM 屬性，並通過全新的原生到 LLVM 組合證明。

受條件保護的倒數測試涵蓋拒絕本體模板後的重試、任意字長輸入的完整迴圈頭證明、切點與查詢預算的累計計費，以及真實入口契約違規時立即拒絕。

`NeverDInterpreterLLVMRefinementTests` 檢查全新的原生到 LLVM 組合證明、精確文字／函式綁定、獨立預算、完整觀察項及刻意擴大的原始碼域。修改位元組、殘餘程式、結果、旗標、狀態碼、框架寫入、poison 或錯誤／過期迴圈方案，都必須拒絕組合憑據。任意字長倒數要求兩段歸納前提；獨立 C 案例在 O1/O2 編譯後驗證實際序列化 LLVM 輸入。狀態模型回歸拒絕隱藏入口回邊，對入口集合計費且不複製附屬來源資訊。

```sh
cmake --build build-release --target NeverDLLVMInterpreterModelTests --parallel 4
build-release/bin/NeverDLLVMInterpreterModelTests
cmake --build build-release --target NeverDInterpreterLLVMRefinementTests --parallel 4
build-release/bin/NeverDInterpreterLLVMRefinementTests
```

兩層和三層迴圈的快取相等退出測試涵蓋運算元相關性、變動邊界、計數器重設及損壞的複製。

比較快取回歸涵蓋相等與不等、帶守衛和常數摺疊的初始化、擴寬後才出現的欄位，以及位元組、雙字與四字快取中的第 7/31/63 位元。保留受檢查位元而只改變相鄰位元，也必須由完整狀態比較拒絕。零步長、移動邊界、計數器重設與共用預算耗盡必須拒絕。

泛化前綴回歸涵蓋匯合入口、首個零次迭代見證、隱藏暫存器／堆疊框架差異、非標準布林述詞、原生陷阱約束、相關聯的堆疊保存，以及錯誤或預算耗盡的計畫。獨立兩層／三層等值退出計數器與原生位元組檢查無號輸入界限、零值／最大值輸入域、非單位步長和錯誤原始指令。推導及最終證明均必須拒絕不完整結果。

獨立編寫的分支迴圈迴歸涵蓋兩種分支方向、錯誤迴圈本體、不終止的相鄰分支，以及共用搜尋／證明預算耗盡。 `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

巢狀推導回歸涵蓋兩層與三層迴圈、遞增與遞減計數器、自動階段常數及實際原生迴圈本體切點。不可達或互斥的前綴域、錯誤本體、不終止或回繞轉換，以及共用搜尋／證明預算耗盡均必須拒絕。前綴證據不能取代完整區段涵蓋。

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests` 檢查完整無環 LowIR 圖的兩次執行獨立性。兩側共用一般入口輸入；每次新產生的架構未定義值在複製、重疊寫入、溢出儲存與重新載入中保持來源關聯。控制述詞先於路徑假設接受檢查。憑證要求 `Complete` 效果中繼資料，並精確綁定每條指令的完整邊界與操作摘要。缺少證據、可達迴圈、呼叫、未知別名或預算耗盡都會拒絕憑證。結論受明確觀察項與無故障堆疊框架契約限制，並非原生程式碼到 C 的完整等價證明。

以下行為採用預設的嚴格審計契約。`NeverDOriginalBinaryUndefinedIndependenceTests` 使用獨立撰寫、固定映射的 x64 位元組，驗證實體原生 CALL/RET、改寫的返回目標、有限間接目標全集和不可變載入。同一測試目標也檢查直接分支完整收集、精確位元組／效果／映射／讀取見證綁定、外層返回時入口 RSP 及返回位址槽保持，以及堆疊框架與映像分離前提的可滿足性。缺失或重疊指令、不符合精確陷阱和明確環境投影規則的未審計分支、不終止或超出預算的迴圈、不完整目標列舉、執行設定／契約不符和預算耗盡均須拒絕，且不產生憑證或殘餘程式碼。成功要求每條可行原生路徑完整結束。此選用閘門不認證迴圈不變量、例外分派、啟用 CET 的執行或原生程式碼到 C 的等價性；一般恢復仍獨立可用。該目標也檢查嚴格提升的 `INT3`/`UD2` 終止邊界及其完整位元組、操作摘要綁定。未定義輸出附屬中繼資料的 `Missing` 必須保持不變；僅經符號執行證明不可達的陷阱可進入憑證，任意可行陷阱路徑都須傳回 `ContractViolation`，且無憑證、無殘餘程式碼。不建模陷阱後的循序執行或例外恢復，不使用 `codeFollowsTrap`，靜態 LowIR API 的支援範圍保持不變。

顯式原生交疊測試涵蓋真實 x64 跳入立即數的分支、兩個可行分支的結果，以及位於先前指令內部的間接返回入口。合成提供器測試涵蓋兩種收集順序的包含式交疊、未執行直接分支上的衝突位元組、兩種順序的程式碼／讀取一致性及候選讀取。精確和不足的位元組預算按間接轉移累計計入重複交疊位元組。分支結果改變、靜態或循環介面使用及證據矛盾均必須拒絕憑證；啟用選項或改變額度會改變摘要。

拒絕邊界的明確啟用測試涵蓋不可達的 RCL、記憶體 XADD 和 REP MOVS、符號路徑矛盾、任意值控制的分支，以及入口、間接跳躍、CALL 和 RET 抵達時的精確拒絕。測試也檢查可達後綴的獨立入口、候選與原生位址重合、格式錯誤或不完整的證據、資源耗盡、靜態/迴圈介面拒絕，以及精化證明的三層摘要繫結。修改不可達指令，或在無保留邊界時切換選項，都會改變證書摘要。這些測試驗證宣告的有限證明範圍，不證明未審計指令的語義。

封裝旗標測試涵蓋全部純量入口旗標組合、特權遮罩、兩次執行的 TF/AC 條件、不同未定義產生點、相關副本、原生呼叫、兄弟路徑狀態、強制最終系統狀態觀察、畸形證據及資源計費。有限迴圈必須結束每條可行輸入路徑；安全分支不能掩蓋無限或截斷路徑。RDSSPD/RDSSPQ 檢查涵蓋 16 個通用暫存器和兩種寬度、高位元保持、保留 `Missing` 證據及偽造投影拒絕。機器狀態測試在兩個 C 後端的 O0/O2 下啟用未定義行為陷阱，與獨立使用者模式旗標預言機比較，並檢查環境失敗狀態不會被後續操作清除。 INCSSPD/INCSSPQ 測試涵蓋兩種寬度和全部通用暫存器、不可達邊界保留、安全兄弟路徑完成後的可行陷阱、零運算元及偽造陷阱證據。

`NeverDX86UndefinedEffectsTests` 檢查未定義位元中繼資料、已定義／保留旗標及過期憑證拒絕。`NeverDX86CarryArithmeticFlagTests` 以算術參考實作檢查暫存器和記憶體形式 ADC/SBB 的輔助進位。`NeverDX86LogicIdentityTests` 檢查相同運算元的 AND 在 64 位元模式下寫入 32 位元目的暫存器時，仍清零其所屬 64 位元暫存器的位元 63:32，同時保留窄位寬寫入未涵蓋的位元。

`X86RotateUndefinedEffects.*` 以純量算術基準涵蓋全部原始計數、運算元寬度、CL 重疊、高位元組別名及記憶體目的運算元。`X86BitTestUndefinedEffects.*` 涵蓋暫存器/立即數索引、來源與目的重疊、擴充暫存器、已定義旗標及暫存器高位寫入。中繼資料反例拒絕遭修改的運算元、編碼及不支援的形式。原生證明區分同一任意位的關聯讀取與不同任意位，檢查恰好及不足的產生者預算，並拒絕可觀察的未定義溢位。完整狀態細化檢查接受選定見證，拒絕零位見證或遭竄改的候選。

`X86XaddAudit.*` 透過無號算術基準檢查全部 65,536 對位元組輸入、較寬位元寬度的旗標邊界、暫存器/高位元組重疊、兩次寫回、REX 位元組寬度限制及完整暫存器保留。原生檢查要求不產生新的任意位元，同時保留先前的相依性；兩種見證皆接受未修改的 XADD，竄改總和、交換來源值或已定義旗標則遭拒。`/6` 別名使用完整移位計數矩陣，並透過修改群組編號/解碼 ID 的反例拒絕語義錯配。

`NeverDPEFixedImageTests` 使用獨立建構的 PE 檔案，檢查含重定位的指令與不可變資料、匯入寫入範圍、畸形標頭／表格、別名及來源資訊竄改。原生到 LowIR 與精確 LLVM 證明接受相符候選，拒絕結果、狀態或原始位元組遭修改的候選。準備預算耗盡維持獨立分類，允許明確提高限額後重試；一般載入也接受含 40000 筆有效重定位記錄、超過預設分析預算的檔案。

`FrameOffsets.*`、`NativeStackSpecialization.*` 與 `OriginalBinaryUndefinedIndependence.*` 檢查 2/4/8/16/32 位元組對齊的全部餘數、自由高位、跨呼叫儲存、倒數迴圈、別名破壞、錯誤分派、無關大遮罩、必要分區升級及恰好／少一次預算。獨立原生控制檢查帶分支約束的對齊、內部無符號返回清理、錯誤清理量與帶前綴返回。這些測試不表示分區迴圈已具備自動原生至 LLVM 的完整證明。

暫存器分區回歸涵蓋大小端、高位框架根、覆寫與重疊欄位、後續邊、擴大的前驅、原生 CALL/RET，以及剛好足夠或少一步的預算。兩種 C 路徑在 O0/O2 執行全部四種記憶體情況。原生細化檢查分別綁定兩個選擇器值，並非無約束輸入證明。獨立 LLVM 案例驗證：將假分支移至共享匯合點之前，不可使它在真分支後繼續執行，並檢查 PHI 複製與儲存。

入口對齊回歸涵蓋更細分區、全部允許餘數、不同高位根位址、最後案例失敗，以及精確與少一預算。兩個 C 後端均在 O0/O2 下以不可存取的被拒客體位址及無效旗標驗證：狀態 2 必須保留全部狀態位元組。模型細化檢查相同拒絕語義；C/Python 測試涵蓋 v5 配置、所有權及舊版與未來尾部。原生證明控制明確拒絕未綁定的對齊域。

`StringTransfer.*` 與重複搬移迴歸檢查重疊、零次數、暫存變數隔離、容量／預算限制及指標失效。`MachineStringSourceTests.cpp` 針對四種寬度與兩個方向，將原生執行及兩條 C 路徑的 O0/O2 結果與獨立的全部暫存器、旗標及堆疊參考結果比較。

`ControlDiscovery.*` 與 `NativeStackSpecialization.*` 涵蓋根位址低位元條件、高位元和完整根相依、不完整遍歷、恰好足夠及不足的遍歷預算，以及有限不可變位址證據的保留。

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer` 檢查條件約束下暫存器和框架槽指標的合流、兩種位元組順序、模回繞及高根位址、堆疊還原和 120 個框架位元組。配套反例拒絕損壞的指標，並檢查查詢、操作、求值和細化預算恰好足夠或少一次的情況，以及相依發現和上下文耗盡。

有限值觀察者測試涵蓋提前拒絕、常數、空投影及最後一次 UNSAT 查詢。不可變讀取回歸確保遇到反例後保留執行期載入，對不同讀取範圍重新驗證快取位址域，並在憑證畸形時拒絕發布任何部分證據。

仿射控制測試涵蓋八次求解預算下的低位元條件、兩種位元組順序中的暫存器與框架槽載體、模數回繞、框架位元組觀測及堆疊還原。非字面常數形式的矛盾條件必須剪除不支援的分支；低 32 位元相同而高位元不同的兩個根值必須保留各自的間接跳躍目標。

`FiniteQueryCache.*` 檢查恰好足夠與少一個儲存單位的邊界、命中更新使用順序、一次淘汰多個不同大小記錄、反覆淘汰和變數重新命名後的查詢。重複存入、未命中、格式錯誤的結果及過大候選均不改變使用順序。快取項目淘汰後，已傳回的證明副本仍然有效。

`ControlStateRecovery.*Marginal*` 涵蓋目標與業務欄位的獨立值域乘積超過聯合上限、殘餘程式的具體輸出、聯合關係放寬後由較晚前驅引入新目標、缺少可達目標，以及超限或列舉不完整時捨棄整個值域。這些原創案例驗證值域變更會重新排程分析，且失敗不會發布部分圖。 框架槽位變體驗證聯合關係放寬後，兩種位元組順序下的別名失效處理。

`InterpreterTransferChain.*` 檢查相關值、唯一及動態分支、延遲前驅、框架邊界、別名拒絕與預算。新增原生 CALL/RET 測試檢查重複指令、返回槽位元組及堆疊恢復。生產者重播及原生到 LLVM 控制涵蓋契約不符、憑據變更、錯誤結果與缺少的堆疊寫入。

`NeverDX86NoIndexAddressTests` 檢查 32／64 位元位址寬度下無索引的 x86 SIB 定址：忽略縮放位元、目的暫存器寬度、載入／儲存、完整未定義輸出中繼資料、區段偏移與位址來源。測試拒絕將虛擬暫存器用作基底或寬度錯誤的索引，並保留 REX.X 選取的真實 R12 索引。EVEX 廣播及遮罩移動測試也涵蓋這些形式、非作用中記憶體存取抑制與不一致的 SIB 中繼資料。

移位回歸涵蓋所有八位元原始次數、零次移位旗標組合、兩種 x86 模式、全部純量位寬、CL 與目的地重疊、AH/CH/DH/BH、擴充暫存器及記憶體。以位元組建模的符號執行對照逐位算術模型，檢查已定義結果與守衛條件。關係測試檢查複製與新生旗標、暫存溢出保存、迴圈重訪、未定義值衍生次數、分支拒絕、畸形編碼及摘要和預算失敗。有限不可變讀取涵蓋 1/2/4/8 位元組、輸入相關選擇、路徑內單位址集合、完整讀取見證與上限綁定，並拒絕相依、缺失、可寫、無檔案支援、重定位或無界候選。

核心測試檢查上下文拆分、固定點匯合、動態迴圈、重疊暫存器、別名失效、有限目標派發，以及拒絕時不提供部分替代程式碼。原始碼測試組譯原創的暫存器式、堆疊式與有限位址 x64 機器，還原兩條 C 輸出路徑，在 O0/O2 下啟用未定義行為陷阱編譯，並與獨立的無號算術和記憶體參考實作對照執行。有限位址 fixture 涵蓋輸入選擇的記錄與相關游標／key 控制欄位；原生檢查涵蓋 SysV 和 Win64 呼叫慣例。測試也涵蓋公開 CLI、還原預算及不支援輸入的報告。需要支援跨目標編譯的 Clang 與 LLD；原始 ELF 的執行另需 x64 Linux 主機。缺少工具或主機不符屬於略過的涵蓋範圍，不代表通過。

`ControlStateRecovery.LongTransparentLoop*` 涵蓋獨立撰寫的 20 階段迴圈、動態算術參考實作、未知 selector 拒絕及預算耗盡。`LongTransparentPhasesKeepExactBitDemands` 檢查 selector 同位元組內的無關位元仍是可觀察的執行期資料，不會成為控制需求。`ProducerClosureChargesWorkBeforeAnotherRestart` 檢查反向探索及重播在新圖啟動前消耗共用預算，且不發布部分結果。

`X86ShiftCarry.*` 以連續單位元位移驗證窄位寬算術右移的進位、遮罩後的計數，以及 APX 目的暫存器和旗標抑制行為。
`NarrowArithmeticShiftCarrySurvivesBothSourceBackends` 在 O0/O2 下啟用未定義行為陷阱，執行兩條恢復 C 路徑，涵蓋全部位元組值和原始計數。
`NeverDLLVMCIntrinsicSemanticTests` 也在 O0/O2 下執行 i1/8/16/32/64/128 的有號與無號整數 min/max，檢查賦值與內嵌結果、運算元產生順序及單次求值。不支援的純量位寬和格式錯誤的運算元必須明確失敗。

## 結構化 C 控制流程與呼叫檢查

`HighControlFlowSemantics.*` 檢查移動迴圈出口或尾部時是否保留其他跳躍仍引用的標籤。測試涵蓋直接進入迴圈頭部、尾部出口及替換後的 break，並以獨立回傳值預期執行 O0/O2 編譯的產生 C。

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` 檢查推斷出的必要暫存器引數是否保留末尾未知欄位。讀取未知的必要引數或條件必須明確觸發陷阱；省略、空指標和巢狀運算元不能被悄悄替換為零。已知值和已證明不被讀取的多餘運算元仍可執行。陷阱是診斷邊界，不是還原行為等價的證明。

## CPU 執行測試

`NeverDIntegerABITests` 為 Windows x64、Linux x64 與 Linux ARM64 建置原始 Clang fixture，透過真實十參數函式檢查暫存器／堆疊參數及呼叫框架。Unicorn/KVM/WHP 矩陣會明確略過不可用的主機／ISA 組合；skip 不代表通過。`NeverDExecutionBudgetTests` 不依賴計時 sleep，檢查共用續接預算、預約失敗與絕對 deadline。

`NeverDCPUEmulationTests` 涵蓋 ARM64 指令、控制流程、載入、CPU 內容、別名、快取失效與有界迴圈；軟體設定也會執行 FP/SIMD 與 TLS。`NeverDUserExecutionTests` 檢查 CPL3/EL0 頁面權限、別名、保護錯誤、內容及位址空間切換。`NeverDServiceRequestTests` 驗證 SYSCALL/SVC 在進入傳輸前攔截、保留狀態並恰好消耗一次要求；這是交接協定，不代表完整 OS 服務實作。`NeverDExecutionConfigurationTests` 驗證共用設定解析、區分建置支援與即時探測，並在變更前拒絕不支援要求。公開 SDK/CLI 測試不需 Windows 模型。`NeverDThreadPointerTests` 檢查 FS 基底、`TPIDR_EL0`、內容還原與權限。`NeverDKvmCancellationTests` 使用不會結束的 x64 客體驗證進行中的 KVM 中斷、恢復及呼叫端 signal 狀態不變；沒有 KVM 時明確略過。

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

缺少 ARM64 硬體或 hypervisor 表示原生覆蓋被略過，不是通過。Unicorn 與交叉編譯不能證明原生 KVM/WHP 執行。

## Linux 程序設定檔測試

獨立[程序測試套件](process-emulation.md#驗證)會編譯真實 x64/AArch64 ELF fixture。`NeverDLinuxProcessTests` 檢查啟動、program-header 政策、服務續接、二進位輸出、客體錯誤與資源停止。`NeverDProcessPublicTests` 經由 C API/CLI 測試且不變更分析映像。`NeverDExecutionSessionTests` 檢查兩個 CPU 共用記憶體／預算，以及要求／錯誤恰好消耗一次。`NeverDX64MemoryUpdateTests` 檢查記憶體算術、SETcc、BT、XMM/MXCSR、寫入 observer、REP 邊界及預備裝置讀取。`DriverBackendParityTests.cpp` 執行原始與重定位 WDK fixture，並將完整可觀察報告與 Unicorn 比較；缺少映像／後端會明確略過。

checked x64 亦支援帶遮罩的傳統 `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN`、`MAX` 的 `SS`、`SD`、`PS`、`PD` 形式。`X64SSEInstructions.def` 統一定義運算元寬度、對齊與准入規則。`MaskedSSEArithmeticMatchesIndependentHostExecution` 以獨立本機 CPU 參照驗證暫存器與 RAM 形式，涵蓋四種捨入模式、FTZ、有符號零、次正规輸入及 NaN；`SSEMemoryObserverStopsBeforeResultAndStatusChanges` 驗證停止請求先於效果提交。DAZ、未遮罩例外、x87、AVX 仍未開放。

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

不可用後端會明確略過。交叉編譯與 Unicorn ARM64 不構成原生 KVM/WHP 證據。

## 驅動程式模擬檢查

同時啟用 `NEVERD_ENABLE_DRIVER_EMULATION=ON` 與 `BUILD_TESTING=ON`，即可建置專項執行套件及共享 C API／CLI 檢查：

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

fixture 涵蓋客體初始化、成功與失敗傳回、不支援的行為、記憶體錯誤、嚴格情境解析、有界執行，以及經過 create、傳輸、cleanup、close 和 unload 的同步緩衝／直接 I/O、READ/WRITE、獨立檔案生命週期、MDL 權限、動態匯出、客體可變參數及結構化 CPU 錯誤。使用 [`emulate-driver` CLI](driver-emulation.md) 驗證 JSON 與處理程序結束碼。生產建置可在 `BUILD_TESTING=OFF` 時啟用此功能；`libneverd` 不得依賴僅供測試使用的 Unicorn 設定。

補充測試涵蓋驅動獨立擁有的非分頁池 MDL、描述符與緩衝區的獨立生命週期、登錄查詢配置與短緩衝區、控制代碼權限、刪除與洩漏，以及無輸出 IOCTL 的完整 64 位元 `information_hex`。真實範例驗收亦涵蓋 Zero 的同步直接讀寫與統計查詢。

後端測試驗證完整 CPU 內容（暫存器、旗標、SIMD、FPU、CR8）、共用記憶體及跨後端／故障內容拒絕。編譯後的 `driver_dispatcher.c` 範例實際執行 DPC 與工作項目回呼，涵蓋計時器邊界、通知／同步事件與計時器、原因 `Executive` 的非警示 `KernelMode` 等待、逾時／延遲、多個阻塞堆疊、設定後重設仍保留喚醒、回呼參數及非法 IRQL／生命週期。工作項目測試繼續涵蓋待處理／完成、佇列、停滯與共用預算。這些案例證明所述子集，不代表完整 Windows 非同步支援。

`driver_context_limits.c`: API 的 IRQL 上限來自 `KernelAPIIRQL.def`，參數相關限制由所屬模型檢查。DPC 不能呼叫登錄 API，也不能配置、釋放或存取分頁集區；Unicode `DbgPrint` 轉換要求 `PASSIVE_LEVEL`，支援的 ANSI 輸出與非分頁操作仍可在 `DISPATCH_LEVEL` 使用。回呼堆疊有明確邊界，越界堆疊指標不能進入另一阻塞工作項目的堆疊。裝置擴充中的已啟動計時器會阻止裝置提早回收。這些檢查並未開放一般 IRQL 切換。

`KernelDeviceStackTests.cpp` 檢查獨立的擁有者／附加關係、頂端選擇、失敗原子性、堆疊容量、不透明欄位、開啟控制代碼計數、解除附加／刪除時的工作項目及請求保留，以及檔案身分與派送頂端的區別。原創 `driver_wdm_stack.c` 使用真正 WDK 標頭與內嵌 Copy/Skip/SetCompletion；一般／啟用 CFG 映像由選用的 `NEVERD_WDM_STACK_FIXTURE`／`NEVERD_WDM_STACK_CFG_FIXTURE` 設定。`DriverWDMStackTests.cpp` 涵蓋重新定位、實際下層狀態、完成順序和旗標、延遲 pending 傳播、工作項目／DPC、等待、`STATUS_MORE_PROCESSING_REQUIRED`、直接 MDL 保留、巢狀完成及格式錯誤的游標／控制值。`DriverScenarioPublicTests.cpp` 涵蓋 C API／CLI 轉送與 C API 保留／巢狀完成，包括已設定的 CFG 映像。缺少產物會明確略過；Linux 證據僅證明同驅動程式堆疊子集，不代表 PDO／PnP／電源支援。 `KernelIRPStackTests.cpp` 檢查計數游標、完整內嵌 Copy 前綴、已消耗位置清零、狀態／pending 傳播、MPR 與巢狀完成、續接擁有者檢查及保留路徑。真正 READ/WRITE 與檔案生命週期也使用內嵌 Copy 驗證。

`DriverPnpScenarioTests.cpp` 檢查 JSON／原生預檢一致、明確初始事實、ID／數量限制、欄位互斥、最終匯流排狀態與可空觀測報告。`KernelPnpDeviceTests.cpp`、`KernelPnpRequestTests.cpp` 與 `KernelPnpCompletionTests.cpp` 檢查提供者擁有權、AddDevice 成功／失敗／洩漏、初始 IRP、檔案准入、生命週期回復、延遲完成、MPR／巢狀／等待續接及失敗原子性。原創真正 WDK 範例 `driver_wdm_pnp.c` 使用選用的 `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE`。`DriverWDMPnpTests.cpp` 驗證一般／啟用 CFG 且重新定位的 AddDevice、檔案 I/O、有序移除、延遲啟動／移除、啟動／query 失敗與乾淨／洩漏的 AddDevice 失敗。缺少產物時明確略過。執行證據僅來自 Linux，只證明所述無資源 PnP 子集。 `DriverScenarioPublicTests.cpp` 另透過 C API 與 CLI 驗證一般／啟用 CFG 映像的七要求延遲 PnP 報告。

V9 schema 測試往返驗證八種次要功能名稱，並與生命週期完成共用最終狀態校驗；QueryStop 0x119 在載入映像前拒絕。擴展模型及真正範例檢查 query-stop 回復、cancel-stop、停止／重啟、突然移除、精確成功失敗邊界、停止／待移除狀態的軟體 I/O、突然移除後的客體拒絕、裝置身分及混合 AddDevice 結果。`DriverScenarioPublicTests.cpp` 透過 C API 與 CLI，在一般／啟用 CFG 範例上執行 16 要求的停止／重啟／突然移除序列，保留成功軟體 IOCTL 位元組、客體拒絕的 IOCTL 及最終 cleanup/close/remove。公開執行為循序：保留 IRP 若無目前可用生產者，無法等待後續情境要求來啟動或清理裝置。Remove 排空要求是設定邊界，不是通用 Windows I/O 准入策略。證據仍僅來自 Linux。

`KernelRemoveLocksTests.cpp` 檢查獨立鎖／裝置身分、NULL／重複 Tag、retail／DBG 大小、立即／延遲排空、失敗取得義務、失敗原子性、容量及退役。`KernelRemoveLockBridgeTests.cpp` 檢查附加前初始化、延伸區範圍、不透明儲存、IRQL 和修改前拒絕不安全 Delete／Detach。真正 `driver_wdm_remove_lock.c` 的 retail／DBG、普通／active-CFG 四種範例以 `NEVERD_WDM_REMOVE_LOCK_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` 指定。`DriverWDMRemoveLockTests.cpp` 涵蓋封包退役後釋放、鎖排空後匯流排才完成、最後 release 先於回呼傳回喚醒、工作項目等待及乾淨 AddDevice 失敗。C API／CLI 使用既有 PnP 情境，保留匯流排接收／完成與最終拆除觀測。缺少產物明確略過；Linux 證據不代表完整 Driver Verifier 或一般並行排空。

`DriverResourceScenarioTests.cpp` 檢查明確 JSON／C++ 事實、整數寬度、數量、實體／暫存器重疊、對齊、ID、空暫存器組及設定序列化。`KernelMMIOTests.cpp`、`KernelMMIOFailureTests.cpp`、`KernelResourceBridgeTests.cpp` 和 `UnicornMMIOTests.cpp` 涵蓋暫存器組／映射所有權、別名、資源世代、緊密排列清單的壽命、提供者時序、重新啟動持久性、意外移除／電源可存取性、精確 CPU／API 交易及失敗原子性。原創且以真正 WDK 建置的 `driver_wdm_resources.c` 使用 `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`；`DriverWDMResourceTests.cpp` 執行真正純量與 REP 存取函式、普通／有效 CFG 重定位、子範圍別名、頁尾映射、STOP／重新啟動及非法存取。C API／CLI 測試在載入映像前拒絕非法事實，並執行相同 14 個要求的重新啟動情境，驗證持久 IOCTL 輸出與精確 map／unmap 次數。共用 [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) 需要此樣本的暫存器／IOCTL 協定。缺少產物時明確跳過，證據僅來自 Linux；不會操作主機實體記憶體或一般裝置後端。

`DriverInterruptScenarioTests.cpp` 涵蓋明確原始／轉譯描述元、混合及僅中斷配置、嚴格事件欄位／數量、來源身分與獨立 BOOLEAN 觀測。`KernelInterruptsTests.cpp`、`KernelInterruptBridgeTests.cpp` 與 `SchedulerInterruptTests.cpp` 涵蓋獨佔配置完整比對、不透明權杖、世代／連線擷取、事件壽命、選定 Ex 欄位的精確配置、同一把鎖／IRQL 恢復、回呼所有權、同時刻 ISR 優先順序，以及變更前的容量失敗。`KernelFrameworkRequestTests.cpp` 檢查純取消預覽及批次權杖容量，不發布呼叫或消耗參考。原創且以真正 WDK 建置的 `driver_wdm_interrupts.c` 使用 `NEVERD_WDM_INTERRUPT_FIXTURE`／`NEVERD_WDM_INTERRUPT_CFG_FIXTURE`；`DriverWDMInterruptTests.cpp` 執行普通／有效 CFG 重定位、傳統十一引數 ABI、Ex 1／2／4、真正 ISR→DPC 完成、AL 低位元組 FALSE、同步／手動鎖、獨立 PDO、重新啟動世代與非法硬體事實。C API／CLI 測試在載入映像前拒絕非法宣告，並執行七要求的 [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json)，檢查 pending IOCTL 位元組及獨立遞送觀測。缺少映像時明確跳過；證據仍僅限 Linux，不代表共用／電位觸發／MSI 中斷或指令級搶占支援。

`DriverDMAScenarioTests.cpp` 驗證明確能力、邏輯域、位元組／數量／時間上限、嚴格方向與分離的設定／觀測。`KernelPhysicalMemoryTests.cpp` 與 `BackendBackingTests.cpp` 檢查共用頁面的配置邊界、固定參考、CPU 權限不變、MMIO／重入排除及整段失敗原子性；`KernelRequestMDLTests.cpp` 檢查唯讀 PFN 與已建立描述元別名是否使用同一實體身分。`KernelDMATests.cpp`、`KernelDMABridgeTests.cpp` 與 `SchedulerDMATests.cpp` 涵蓋真正 RAM 位元組、adapter 繫結表呼叫、直接／排隊 FIFO 所有權、獨立回呼／映射壽命、頁面片段、錯誤方向、釋放預檢、獨立 PDO 域及世代／電源失敗。原創真正 WDK 樣本 `driver_wdm_dma.c` 使用 `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE`；`DriverWDMDMATests.cpp` 與 C API／CLI 測試執行真正 adapter 指標、common／SG 儲存及分開設定的 DMA／中斷事件。共用 [driver-dma-scenario.json](../examples/driver-dma-scenario.json) 要求該樣本協定。缺少產物時明確跳過；執行證據僅限 Linux，不代表真正主機 DMA、PCI 或一般裝置引擎支援。 `pluginsdk/python/tests/test_driver_dma_integration.py` 透過既有擁有回傳 JSON 的繫結，使用 `NEVERD_TEST_LIBNEVERD`／`NEVERD_TEST_WDM_DMA_FIXTURE`／`NEVERD_TEST_WDM_DMA_CFG_FIXTURE` 驗證資料位元組、回呼順序與失敗觀測。

`KernelSEHTests.cpp` 驗證不修改狀態的展開計畫、範圍順序、非揮發性通用暫存器還原、有界堆疊及明確不支援的中繼資料；`KernelExceptionTests.cpp` 驗證精確的 API 參數數目、低 32 位元狀態、具型別例外、IRQL 限制與模型/CPU 狀態不變。以真正 WDK 和 `/GS-` 建置的 `driver_wdm_seh.c` 使用選用的 `NEVERD_WDM_SEH_FIXTURE`／`NEVERD_WDM_SEH_CFG_FIXTURE`；`DriverWDMSEHTests.cpp` 執行一般、啟用 CFG 及重定位映像，涵蓋直接與輔助函式引發例外、巢狀處理常式、處理常式再次引發例外、真正的篩選函式、展開 finally、搜尋順序、穩定例外記錄、支援的 CPU 故障續接及完整 CPU 狀態還原；巢狀篩選函式與衝突 finally 使用關聯邏輯堆疊，其他 CPU 故障仍明確拒絕。C API/CLI 執行 [driver-seh-scenario.json](../examples/driver-seh-scenario.json)，驗證 API 結果為 null 且真正客體處理常式輸出訊息。`pluginsdk/python/tests/test_driver_seh_integration.py` 使用 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_SEH_FIXTURE` 及 `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`。外部映像缺少時明確跳過；證據仍限 Linux，不能據此宣稱使用者緩衝區或一般 SEH 支援。

`KernelDMAChannelTests.cpp`、`KernelDMAChannelBridgeTests.cpp` 與共用 `SchedulerDMATests.cpp` 檢查混合配置 FIFO、回傳寬度、純准入／釋放預檢、register 重用、連續頁面片段、整次操作 flush、CurrentIrp 快照及封包／MDL／裝置壽命。原創真正 WDK 樣本 `driver_wdm_dma_channel.c` 使用選用路徑 `NEVERD_WDM_DMA_CHANNEL_FIXTURE`／`NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`；`DriverWDMDMAChannelTests.cpp` 執行 normal／active-CFG／重定位驅動程式，涵蓋真正 MapTransfer／FlushAdapterBuffers、共用 common／SG／channel 配額、明確裝置交易、IRQ／DPC 完成、連續操作、兩個 PDO 及失敗案例。C API／CLI 執行七筆要求的 [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json)，包含一次跨越兩個映射片段的交易。`pluginsdk/python/tests/test_driver_dma_channel_integration.py` 以 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE`、`NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` 使用相同公開 JSON 介面。缺少產物時明確跳過；Linux 證據不代表系統 DMA 控制器或任意 HAL 映射／flush 模式的支援。

`DriverPowerScenarioTests.cpp` 驗證必填電源事實、JSON／原生一致性、不透明32位元 context、回應 FIFO 限制及獨立子報告。`KernelPowerRequestTests.cpp` 與 `KernelPowerCompletionTests.cpp` 檢查封包配置、路徑旗標、生命週期與裝置通知區別、FIFO 配對、最終回呼所有權、MPR、等待及釋放邊界。真正 WDK 原始範例 `driver_wdm_power.c` 使用選用的 `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE`；`DriverWDMPowerTests.cpp` 涵蓋普通／active-CFG 重新定位、直接及巢狀 Query/Set、獨立延遲完成、S0 先於 D0、跨等待五參數回呼快照、工作項目來源子要求、空回呼、query 拒絕、獨立 PDO 初值／FIFO 和缺失事實錯誤。`DriverScenarioPublicTests.cpp` 新增格式錯誤預檢及 C API／CLI 的六情境要求／三子要求睡眠喚醒序列。缺少真正產物明確略過；執行證據僅來自 Linux，只證明文件中的可分頁無資源電源子集。

`KernelUsbIdleTests.cpp` 驗證協定所有權、精確回呼／D2 身分、借用及首個完成原因；`KernelUsbIdleBridgeTests.cpp`／`KernelUsbIdleReceiptTests.cpp` 涵蓋真正 IRP、非法准入、排隊取消、組合容量及巢狀收到／完成時序，`DriverUsbIdleScenarioTests.cpp` 檢查原生／JSON 一致與報告證據。真正的 `driver_wdm_usb_idle.c` 使用 `NEVERD_WDM_USB_IDLE_FIXTURE`／`NEVERD_WDM_USB_IDLE_CFG_FIXTURE`；`DriverWdmUsbIdleTests.cpp` 涵蓋 idle、取消、D0／D3、真正喚醒、重送重啟、獨立／組合成員及直達 PDO／FDO 轉送和配置方堆疊。`DriverWdmUsbIdlePublicTests.cpp` 經 C API／CLI、一般／active-CFG、慣用／重定位執行 [USB 情境](../examples/driver-wdm-usb-idle-scenario.json)。缺少產物明確略過，證據仍僅限 Linux，不代表 KMDF USB 選擇性暫停已實作。

`KernelFrameworkUsbIdleTests.cpp`、`KernelFrameworkUsbIdleStorageTests.cpp`、`KernelFrameworkUsbIdleBridgeTests.cpp` 驗證策略、真正儲存與型別化排程。真正的 `driver_kmdf_usb_idle.c` 不自行發出 USB idle 封包。`DriverKMDFUsbIdleTests.cpp` 涵蓋無許可、受管 I/O 的延遲 D2／D0、回呼前／中 StopIdle、arm 失敗、明確 Maximum 能力、遠端喚醒及組合成員。`DriverKMDFUsbIdlePublicTests.cpp` 經 C API／CLI、一般／active-CFG、慣用／重定位執行 [KMDF USB 情境](../examples/driver-kmdf-usb-idle-scenario.json)，使用 `NEVERD_KMDF_USB_IDLE_FIXTURE`／`NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`。缺少產物明確略過，執行證據僅限 Linux。 模型測試也驗證：喚醒 arm 回呼成功後配置耗盡，仍執行真實 disarm 回呼、取消 WAIT_WAKE，且不消耗 D2 回應。

`DriverKMDFUsbPoFxTests.cpp` 涵蓋首次 SystemManaged／WithHint 設定、獨立 PoFx／USB 許可、D0 取消、延遲 D2／D0 與真正 worker 延遲 F0 確認、活動及 StopIdle、READ 前喚醒恢復、arm 失敗、移除與重啟。`DriverKMDFUsbPoFxPublicTests.cpp` 以 C API／CLI、兩種服務模式、普通／active-CFG、首選／重定位執行 [USB PoFx 情境](../examples/driver-kmdf-usb-pofx-scenario.json)。`DriverKMDFUsbIdleTests.cpp` 與公開測試保留轉送回歸並驗證直接 READ 於 D0Entry 後投遞。`KernelFrameworkRequestTests.cpp` 檢查對應、caller-context 所有權、手動／停止佇列及 IRQL。配置失敗與獨立門控由模型橋測試證明，真正樣本不耗盡 arena。缺少外部產物明確略過，證據限 Linux。 `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` 另涵蓋 RemovePending 期間真正 D0Entry 失敗：精確 Required 確認與 quiesce 允許失敗 IRP／硬體清理，不觸發 F0／ActiveCondition；不代表一般在場裝置 SET_POWER 失敗或自主意外移除。

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: [原生 WAIT_WAKE 情境](../examples/driver-wdm-wait-wake-scenario.json) 使用真正 WDK `driver_wdm_wait_wake.c`，透過 `NEVERD_WDM_WAIT_WAKE_FIXTURE`／`NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE` 執行。測試涵蓋 START 中提交、回呼參數、喚醒不自動 D0、重送、取消、MPR、DPC 取消後工作執行緒提交 D0、精確事件捕捉和獨立提供方。一般／active-CFG 及慣用／重定位位址的執行證據僅來自 Linux；缺少檔案明確略過。

`KernelPowerCompletionTests.cpp` 涵蓋 APC／DPC 准入、容量耗盡的原子重試、僅提供方同步／延遲有／無回呼、捕捉路徑及 MPR。真正的 `DriverWdmWaitWakeTests.cpp` 檢查 DPC 取消回呼直接要求 D0、獨立 APC／DPC Query/Set、IRQL／CR8 不變、呼叫傳回早於 PASSIVE 派送及完成，以及提升 IRQL 的 WAIT_WAKE 仍被拒絕。[提升 IRQL 情境](../examples/driver-wdm-elevated-power-scenario.json) 經 `DriverWdmWaitWakePublicTests.cpp` 涵蓋 C API／CLI、一般／active-CFG 及慣用／重定位；執行證據仍僅限 Linux。

`DriverGuardTests.cpp` 與四個原創 `driver_guard.c` 變體涵蓋啟用／未啟用的 CFG、重新定位、檢查／分派 ABI 及格式錯誤的目標。`KernelFrameworkTests.cpp`、`KernelFrameworkControlTests.cpp`、`KernelFrameworkQueueTests.cpp` 和 `KernelFrameworkRequestTests.cpp` 涵蓋繫結、失敗時可復原的裝置建立、佇列路由、緩衝區邏輯長度，以及清理順序與 IRP／內容生命週期。原創 `driver_kmdf_lifecycle.c` 與 `driver_kmdf_control.c` 可選用真實 WDK 1.33 標頭編譯，並透過真正的 `FxDriverEntry` 程式庫連結。將 CMake 快取路徑 `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` 指向生命週期映像，將 `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` 指向一般／啟用 CFG 的控制裝置映像。缺少外部產物時會明確略過。`DriverKMDFLifecycleTests.cpp`、`DriverKMDFControlTests.cpp` 及 `DriverScenarioPublicTests.cpp` 中的 C API／CLI 案例涵蓋實際回呼、緩衝／直接 I/O、工作項目完成待處理要求、失敗狀態、卸載及重新定位後的 CFG 執行。驗證證據仍僅限於 Linux，不代表完整 KMDF 或 PnP／電源管理支援。

舊版取消測試將 API 接續保留至取消、巢狀清理與最終銷毀結束；對於已取消的請求，Ex 仍傳回取消狀態而不遞送回呼。`KernelFrameworkRequestAccessorTests.cpp` 與 `KernelRequestMDLTests.cpp` 涵蓋共用的 64 位元 Information、完成時長度驗證、來源佇列／IRP 識別、NULL WDF 檔案控制代碼、保留控制代碼的 getter 結果、緩衝 MDL 快取與首個方向的 ByteCount、直接描述元識別與延後對映、完成時回收，以及拒絕繞過 WDF 完成流程。真實控制裝置 fixture 的 L、M、D、C 模式在一般／啟用 CFG 映像中分別執行舊版取消、緩衝 MDL／資訊、直接 READ／WRITE MDL 和完成後的存取。

取消測試涵蓋僅允許傳輸要求設定的虛擬期限及報告欄位、完成優先與已取消路徑、標記／解除標記結果、排入佇列與已遞送回呼的完成權限、回呼等待及內部參考生命週期。排程器測試獨立驗證 DPC／取消／工作項目順序、容量、識別隔離和暫停／還原。WDM 取消仍明確回報模型錯誤。


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
| `unittests/evm` | `NeverDEVMOpcodeTests`、`NeverDEVMBytecodeTests`、`NeverDEVMLoaderTests`、`NeverDEVMABITests`、`NeverDEVMAnalyzerTests`、`NeverDEVMDecoderPropertyTests`、`NeverDEVMProxyTests`、`NeverDEVMCallTests`、`NeverDEVMSemanticTests`、`NeverDEVMEmitterTests`、`NeverDEVMIntegrationTests` | 硬分叉 metadata、輸入正規化、ABI/signature 歧義、CFG/SSA/復原、窮舉 decoder boundary 與惡意輸入、proxy/call 事實、interpreter 語意、LLVM/C/Solidity 差分執行及公共 API routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFProgramImageTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFVerifierTests`、`NeverDSBFISAConformanceTests`、`NeverDSBFAgaveConformanceTests`、`NeverDSBFSemanticTests`、`NeverDSBFEmitterTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`、`NeverDSBFMalformedCorpusTests`、`NeverDSBFUpstreamConformanceTests`、`NeverDSBFExternalOracleTests`、`NeverDSBFSolanaModelTests`、`NeverDSBFIntegrationTests` | v0-v4 中繼資料與 ELF 配置、嚴格 verifier/loader 行為、23 個固定 ELF 成品、獨立 official oracle、完整 opcode 可用性、惡意輸入、CFG/還原及已執行的 LLVM/C/Rust 差分 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 四 ISA×三物件格式的重寫/混淆等價性 |
| `unittests/semantic` 中的聚焦轉換檔案 | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 從大型語意二進位拆出的快速重新連結探針 |
| `unittests/corpus`（submodule） | `NeverDWindowsEHCorpusTests`、`NeverDRustEHCorpusTests`、`NeverDGoEHCorpusTests`、`NeverDCxxItaniumEHCorpusTests`、`NeverDObjCEHCorpusTests`、`NeverDAdaDEHCorpusTests` | 從 545 個釘住的真實二進位讀出的例外與執行期 metadata，每一個都在 manifest 裡宣告了其復原必須達到的下限 |

註冊的事實來源是
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt) 與
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt)、
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt) 與
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt) 和
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt)。

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
`check-neverd-cxx-itanium-eh-corpus`、`check-neverd-objc-eh-corpus` 與 `check-neverd-ada-d-eh-corpus` 各跑一條。三個
CI 主機都帶著這個開關配置並跑全部六條產線：位元組到處都一樣，但讀位元組的東西不一
樣，在一台主機上跑通不能說明另外兩台。`scripts/audit_ci_test_inventory.py` 會拒絕缺
少六個標籤中任何一個的清單——建置悄悄不再讀 corpus 是一種沒有任何測試能捕捉的迴歸，
因為消失的正是那個測試。

EVM 操作碼稽核每次都會以 `git fetch --depth=1 --force` 強制取得官方預設分支的 remote
`HEAD`：`https://github.com/ethereum/go-ethereum.git`。腳本解析並回報剛取得的精確
SHA，再於 detached 暫存 worktree 中探測該物件。每次執行都使用名稱不可預測的 private
temporary bare repository，在 detached worktree 的整個生命週期持有官方 fetch 的 authority
ref 與精確 SHA，最後一併銷毀 repository 和 worktree。不使用 shared persistent Git
repository 或 cache。本機與
CI 皆不讀取 `local_docs`、既有原始碼 checkout 或 submodule。固定 submodule 反而會在最需
偵測即時漂移時過期：

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

public CLI 唯一接受的 option 是 `--manifest-output`，不提供 remote/ref/toolchain override。輸出
manifest 的封閉契約為 `schema 3`。

每個 Git command 都會先清空全部繼承的 `GIT_*`（包含 `GIT_CONFIG_*`），再只裝入經過
稽核的設定。`GIT_CONFIG_NOSYSTEM` 與 `GIT_CONFIG_GLOBAL` 停用 system/global
configuration；`GIT_ATTR_NOSYSTEM` 與按 command 設定的 `core.attributesFile` 停用
system/global attributes，`core.hooksPath` 停用 hooks。非預期的 private-repository configuration、graft、
`objects/info/alternates` 或 `refs/replace` 都會使驗證失敗；`GIT_NO_REPLACE_OBJECTS` 會停用
replacement lookup。

CI 僅在 `dev` push、pull request、手動啟動和每日排程中執行此線上稽核。Go probe 反射
`params.Rules` 匯出的全部 bool 欄位，對每個對應 fork 呼叫公開的
`LookupInstructionSet(params.Rules)`，並掃描全部 256 個 byte slot。
`EVMUpstreamOpcodePolicy.def` 管理命名 alias 與 typed 的歷史/未排程 EOF 排除項，並驗證
overlap/inactive invariant；正交的 `EVMUpstreamSemanticsPolicy.def` 管理封閉 Rules
清單、fork mapping、base-stack 例外與 EIP-8024 dynamic opcode family 宣告。封閉 manifest 檢查
精確 revision、fork activation、byte/name、`base_min_stack` 和 `net_stack_delta`，拒絕
未知或重複欄位、rule、fork、名稱與 byte。slot allocation 只依
`operation.undefined`；`HasCost` 只用於 cost cross-check，因為 defined zero-cost operation
也會回傳 false。每個 `defined && !HasCost` slot 都必須從宣告的 fork 起與
`EVM_GETH_ACTIVE_WITHOUT_COST` 精確相符。undefined 卻有 cost、未 review 卻 defined，或 marker
消失都會封閉失敗。CI 失敗時會上傳精確 revision、manifest 與
log artifact。parser 與 drift diagnostic 有獨立 Python unit coverage：

`EVMUpstreamSemanticsPolicy.def` 用唯一一筆 `EVM_GETH_RULE_FIELD` 將每個匯出的 boolean
`params.Rules` field 歸入 `MappedForkSelector`、`NoOpcodeAllocation` 或
`ExcludedSelectorExpectedError`。probe 每次只啟用一個 field 並呼叫 `LookupInstructionSet`；前
兩類必須沒有 error，第三類必須報錯，回傳的完整 256-slot opcode/stack fingerprint 均必須等於
`ExpectedFork`。目前 `IsEIP155`、`IsEIP2929`、`IsEIP4762` 與 `IsPetersburg` 是 Frontier
fingerprint 的 no-allocation fields；`IsUBT` 必須報錯並呈現 Cancun fingerprint。

EIP-8024 dynamic opcode family 的 membership 與 activation 由
`EVMUpstreamSemanticsPolicy.def` 宣告；`EVMEIP8024Immediates.def` 仍是 single/pair 各 byte
immediate semantics 的唯一 authority，其 inventory 都明確分類全部 256 個 byte。production 直接
查表；live audit 用 `go -overlay` 向 `core/vm` 虛擬注入 wrapper，取得真正的 private
`operation.execute` handler，並針對每個 active table/family 執行 `DUPN`、`SWAPN` 與
`EXCHANGE` 的 `3x256` candidates 加 `3 missing-operand cases`。測試核對 acceptance、PC
delta、marker 推導的 operand/stack mutation、valid case 的精確 underflow，以及缺 operand 時的
`0x00`；Python 對照同一 `.def`，不重述公式。

`EVM_HARDFORK_LATEST` 只有一個 canonical target；封閉的 `EVMUpstreamForkAliases.def` 將 Prague
映射至 Pectra，將 Osaka 與 BPO1 至 BPO5 映射至 Fusaka，而
Paris/Shanghai/Cancun/Amsterdam/Bogota 映射至自身。未知名稱封閉失敗。單次 audit 記錄的
`audit_unix_time` 同時驅動 `MainnetChainConfig.LatestFork(time)`（必須等於 NeverD latest）與
`LatestFork(max uint64)` 的 alias/已 probe canonical fork 檢查。probe 列舉真實的
`canonical fork jump tables` 與 `mainnet active/scheduled jump tables`，逐 table 完整比較，並
明確記錄 dynamic family 或 fork 的 `inactive` 狀態。僅取得部分 table、family 或 probe 的
`partial` result 不會被接受，而會封閉失敗。manifest 固定
`authority=official-fresh-fetch`、官方 URL、要求的 `HEAD` 與 SHA；public
CLI 沒有 remote/ref/toolchain bypass，probe 使用 `GOTOOLCHAIN=local`。

Go request/response 與 Python controller 會在配置惡意 metadata 前執行
`input/collection/string hard limits`，超限 input、array 或 string 均封閉失敗。兩者還會獨立執行
`bounded diagnostic output`：過長 display 包含 full-content `digest` 與
`explicit truncated marker`。每條 command 都有 bounded child output 與共用 deadline；timeout
或 output-limit 違規會終止整個 `process group` 及其後代 process tree，並排空 pipe。所有
`.def parser` 都會拒絕 unparsed、unknown、duplicate、missing、out-of-range entry 並封閉失敗。

目前 schema-3 live receipt 記錄 `schema_version=3`、`audit_unix_time=1787534659`、
`authority=official-fresh-fetch`、`remote=https://github.com/ethereum/go-ethereum.git`、
`ref=HEAD`、revision `02b73d4ea7181464175e0a6cbecc0a3a2655a562`、本機 `Go 1.24.0`、
`stack_limit=1024` 與 `diagnostics=[]`。它涵蓋 `21 fork tables` 與 `20 Rules probes`，分類為
`15 mapped/4 no-op/1 expected-error`。兩筆 `mainnet active/scheduled` record 都回報
`upstream BPO2`，由封閉 mapping 對應至 `NeverD Fusaka`。EIP-8024 有 `23 table targets`，其中
只有 `Amsterdam/Bogota` 為 active，產生 `1536 candidate executions` 與
`6 missing-operand cases`。`three handler symbols` 在兩個 active target 間一致。Python audit 為
`67/67`，`C++ Opcode 10/10`。macOS 真實執行在 `sandbox-exec` 下成功，最後的 `go run` 保持
offline；Linux workflow 強制 `bubblewrap`。

所有 Go stage——`go env`、`go mod init`、`go mod edit`、`go mod tidy`、
`go mod download` 與 `go run`——都必須通過 `capability-root` filesystem sandbox。其 read
capability 僅包含 private probe、fresh geth、已驗證的 `resolved GOROOT` 與精確所需的 system
runtime root；只有隔離的 environment root 可寫。network 僅授予需要它的 dependency stage，
final run 保持 offline。test 會在 `host HOME/workspace` 放置 sentinel，要求存取遭拒，且任何
output 都不得含有其內容。Linux 驗證同構的 `bubblewrap` policy，且不使用 `/` broad bind。

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

目前 CMake 註冊的 11 個 EVM 測試 target 為：

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` 會在每個改變 decoder 的 fork 上窮舉所有雙位元組輸入，
比較完整 decode 與精確 `JUMPDEST` boundary，並以長度受限的確定性惡意輸入覆蓋所有 fork。

修改 EVM control flow 時，先執行 fixed-point 與 height-domain contract：

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

這些案例涵蓋跨基本塊 internal return、有限 multi-target merge、loop convergence、
deterministic edge ordering、path-sensitive whole-stack lane、correlation preservation、
unknown jump、exact invalid target，以及包含 `MaxAbstractInstructionTransfers` 的
fail-loud budget。strict 僅在已證明 `Reachable` 的 lane 拒絕 unknown 或 fork-inactive
opcode；`MayReachable` 只保留 CFG candidate，不能產生確定語意。接著應執行全部 11 個 EVM 測試
target 與線上 upstream audit；CFG 修改仍可能影響 emitter 與 integration。

修改 MedIR/HighIR dataflow 時，另需執行 constant-phi、selector、typed-operand、
malformed-graph 與 deep-chain contract：

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

這些案例驗證相同與衝突的 cyclic phi、不相鄰及跨基本塊的 selector expression、等式兩種
operand order、exact ABI width check、typed storage/event/calldata operand、只由 root lane
沿 dispatcher mismatch edge 復原 selector/receive/fallback、共享 selector 的 standard
ambiguity、逐標準 `KnownFunctionVariantInfo` 選擇，以及僅在所有已證明可達的成功終態
return shape 一致時輸出 return list。亦涵蓋 malformed MedIR 的 deterministic handling
與深層 iterative producer walk。

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
[Windows 例外重建](windows-exception-reconstruction.md)。

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
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h)。
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

interpreter 在任何 opcode-specific side effect 前執行 typed stack preflight；
`EVMForkSemantics.def` 規定 byte `0x44` 在 Paris 前是 `DIFFICULTY`、自 Paris 起是
`PREVRANDAO`。`REVERT`、fault、step limit 與 resource exhaustion 均回滾 transaction
state；allocation failure 標為 `ExecutionFaultKind::ResourceExhausted`，若連 entry
snapshot 都無法建立，`HasPersistentStateSnapshot` 為 false，結果不可 commit。

### EVM public boundary 與 budget regression

public API test 會分別變造 canonical
`Code`/`Fork`/`Instructions`/`JumpDestinations`，以及每個 LowIR table、range、ID、lane 與
edge reference。`execute` 必須在 instruction lookup 前回傳 `llvm::Error`；
`lowerToMedIR` 必須在建立 index 或按輸入規模配置輸出前，拒絕完整結構非法或超 budget 的
LowIR。`lowerToMedIR` test 亦強制 option validation、resource validation、structure validation
的順序，並要求它們先於逐 field `canonical decode replay` 與
`lowerCanonicalLowToMedIR`。public HighIR recovery 會 replay 驗證外部 LowIR/MedIR；僅
`analyze` 可對自己持有的 canonical IR 使用 `lowerCanonicalLowToMedIR` 與
`recoverCanonicalHighIR`，既避免 recursive/duplicate replay，亦持續強制所有 HighIR
option/resource budget。interpreter 隨後對 `EVMInterpreterLimits.def` 宣告的所有上限進行 exact-boundary 與
+1 test：`MaxSteps` 保持專用 `StepLimit`；`MaxMemoryBytes`、`MaxTraceEntries`、
`MaxLogEntries`、aggregate `MaxLogDataBytes` 與 runtime `MaxPersistentStateEntries` 耗盡
皆回傳 `ResourceExhausted` 並 rollback transaction effect。初始 aggregate
`MaxHostReturnDataBytes` 或 persistent state 過大是 API error。test 亦覆蓋 return-data
`ArrayRef` view 與排序表 `lower_bound` lookup，不需複製 buffer 或建立 PC map。初始
`MaxCalldataBytes`、橫跨 `BlockHashes`/`Balances`/`CodeHashes`/`ExternalCode`/`BlobHashes` 的
aggregate `MaxHostEnvironmentEntries`，以及 aggregate `MaxExternalCodeBytes` 同樣是 API error。
`const execute preflight` 會在複製 environment、snapshot 或 result 前拒絕它們。

獨立的 LowIR boundary test 覆蓋 aggregate diagnostic limit `MaxLowDiagnostics` 與
`MaxLowDiagnosticBytes`，驗證 linear decode/CFG construction 按 exact count/final bytes 預先
計費並拒絕零上限。
HighIR safety test 覆蓋按 lane 排序的 `Any/Exact/Excluded` domain、equality
match/exclusion、raw `XOR(selector, constant)` 的 false-edge match 與 true-edge mismatch、
zero word/calldata size/call value 的逐 edge 精化，以及 unknown condition 的 fail-closed 行為。
測試另含 `EQ` 與 `raw XOR` 兩類 back-jump regression，確保 `arguments`、`mutability`、
`return shape`、`region` 不受另一個 function 污染。
exact-boundary 與 -1 test 覆蓋 `EVMAnalysisLimits.def` 中的
`MaxHighDispatchCandidates`、aggregate
`MaxHighRecoveredArguments`、
`MaxHighDiagnostics`、`MaxHighDiagnosticBytes`、`MaxHighReferenceVisits`、
`MaxHighMemoryTransferCells` 與 `MaxHighMemoryValueVisits`；所有 output diagnostic（包含固定
malformed diagnostic）都必須在配置前計入 count 與最終 bytes。LowIR 與 HighIR diagnostic
budget 會獨立測試；建立 default root CFG region 時必須在 reserve 或複製 block-PC 清單前計入
`MaxHighRegionBlockReferences`。
外部 CALL/CREATE 結果會作為 nondeterministic host outcome 探索兩條精確 CFG edge，因此保留
ERC-1167 fallback 復原；無法讀取的 selector 條件仍是 Unknown，不能製造 fallback 或 function
fact。

control-flow test 從 `EVMLowFaultKinds.def` 取得 `InvalidJumpDestination`，並用於
`end-of-code JUMPI`：target 無效且 condition 確定為 true 時沒有 successful tail，屬於 definite
fault；condition 確定為 false 時成功；condition 未知時保留可能成功的 false path，不把整條 lane
標為 definite fault。

ABI test 在 exact limit 與 +1 驗證 `EVMABIParserLimits.def` 的 grammar boundary，以及
`EVMABITableLimits.def` 的 public table cardinality/text boundary；亦拒絕非法
kind/standard/evidence enum、錯配 metadata、noncanonical signature/return list、錯誤標為
independent 的 shared selector、dangling/duplicate variant，以及非 word width 的 event-topic
`APInt`，再進入 indexed selector 或 sorted topic lookup。

`NeverDEVMOpcodeTests` 亦約束 metadata architecture：每個已配置 opcode 於 byte encoding
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
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
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

sanitizer profile 分開建置於 `build-sbf-asan-ubsan`。按 revision 鎖定的 prebuilt
package 已包含必要的 fork-only header，因此 integration 也在同一個 fail-fast
ASan/UBSan profile 中執行。

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
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

## Android 類別清單效能

在量測 `neverd mobile INPUT --list-classes` 前，以 Release 建置 `NeverDMobileTests` 並執行其標籤。
讀取器測試涵蓋稀疏中繼資料、Unicode、無效參照、不支援的方法本體、校驗和及預算；
封存測試區分完整擷取與選定內容查詢。CLI 測試檢查前綴篩選、JSON 範圍、既有輸出保護及 multidex 失敗原子性。

獨立的樣本／量測工具在接受計時樣本前，會驗證每個程序的完整描述符清單：

```sh
python3 -m unittest scripts.tests.test_benchmark_mobile_inventory -v
python3 scripts/benchmark_mobile_inventory.py \
  --output-dir /tmp/neverd-inventory-benchmark \
  --class-count 6000 --dex-count 3 --code-units 64 \
  --extra-string-bytes 8388608 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

每次執行使用新的輸出目錄。`--generate-only` 只寫入樣本和 manifest，不計時。
`--workload` 為選用的 `--peer-command 'tool {input} {prefix}'` 選擇共用輸入類型；
輸入準備不計入命令時間。報告保留雜湊、命令、所有新程序樣本、暖快取假設，以及在具有 GNU time
的 Linux 上的最大子程序 RSS。該 RSS 不是多程序工具的合計尖峰。合成 APK 是查詢容器，不是可安裝的應用程式。
清單速度不能證明參照搜尋速度或 Java 還原品質。

混合核心 CPU 上，將量測工具及其繼承的子程序綁定至同一個允許的 CPU（例如 Linux 的
`taskset -c 4 python3 ...`），避免混合效能核心與效率核心。報告會記錄繼承的 CPU 親和性。

## Android 程式碼參照效能

參照查詢與還原讀取器共用指令邊界和程式碼驗證。修改此邊界後應執行行動測試套件。
讀取器測試涵蓋運算元池種類、比對模式、方法歸屬與共用程式碼、看似參照的 payload／立即值、
格式錯誤輸入及資源限制。共用除錯資料流須依每個所屬本體的框架、範圍和參數檢查。
儲存上限測試應同時保留大型成員清單及密集分支本體；持久索引和暫時容器擴充的生命週期不同。
另須檢查重新排序和重疊項目、寬度相同但原型不相容的共用程式碼、未對齊輸入儲存，以及跨越搜尋區塊邊界的子字串。
私有解碼資料的效能變更必須保留完整的具所有權還原模型及參照出現多重集合，包括不支援還原中繼資料時的失敗行為。
請區分獨立產生的預期結果與真實輸入上的跨工具一致性。

獨立參照量測工具在產生指令時記錄預期出現位置。每次量測都檢查完整方法身分、以 code unit
計算的 PC、opcode、目標身分、UTF-16 單位和重複次數，並檢查獨立預期的 NeverD 覆蓋計數及 `code_scan_complete`：

```sh
NEVERD_REFERENCE_TEST_BINARY="$PWD/build-release/bin/neverd" \
  python3 -m unittest scripts.tests.test_benchmark_mobile_references -v
python3 scripts/benchmark_mobile_references.py \
  --output-dir /tmp/neverd-reference-benchmark \
  --class-count 500 --methods-per-class 64 --matching-methods 2 \
  --dex-count 3 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

使用 `--kind` 和 `--workload` 選擇案例。`--extra-strings 65536` 測試實際的 32 位元字串索引。
預設啟用 payload 誘餌；`--no-payload-lookalikes` 保留相同配置和真實參照，只替換誘餌 payload 值，
供共同輸入比較使用。正確性和計時結果都要保留。若查詢回傳假的 payload 參照或漏掉真實參照，驗證失敗且計時不被接受。

選用的 `--peer-command` 接受含 `{input}`、`{kind}` 和 `{query}` 的 argv 範本。
若其他工具語義不同，須明確調整查詢語法並比較完整出現多重集合。報告保留其宣告的驗證範圍，不宣稱完整程式碼掃描。
清單基準的全新目錄、CPU 親和性、新程序、暖快取及 RSS 限定同樣適用。
除非 `NEVERD_REFERENCE_TEST_BINARY` 指向已建置的可執行檔，否則選用 CLI 單元測試會略過；請報告該略過情況。

## 行動 SDK 匯出證據

手動工作流程 `Mobile SDK Export Evidence` 針對固定的 Xcode SDK 執行 `collect_mobile_ios_sdk_declarations.py --exports-only`。它原樣保留 iOS 實機與模擬器 SDK 的 Foundation、CoreFoundation、UIKit 連結器映射，並記錄目標、SDK 版本、SDK 設定雜湊、檔案大小及 SHA-256。一般宣告收集器也會保留這些映射。檔案缺失、為空、超出大小限制或位於 SDK 外部時，收集失敗，並保留已完成的證據。連結器映射提供符號匯出證據，不能證明呼叫 ABI 或方法復原成功。

## 行動端 Swift String ABI 證據

手動工作流程 `Mobile Swift String ABI Evidence` 使用 Xcode 26.5，為 arm64 iOS 裝置與模擬器編譯固定的 Swift 相等、排序比較探針及 C `swiftcall` 探針。`collect_mobile_swift_string_abi.py` 保存原始碼、LLVM IR、組合語言、編譯器身分、SDK 設定與 `libswiftCore.tbd` 及其雜湊。兩種語言都必須顯示精確比較匯入採用五個參數並傳回 `i1`；C 必須將該結果明確擴充為一位元組。目標或簽章不符、命令失敗及逾時均保留部分證據並令採集失敗。這些編譯器證據不會安裝執行階段宣告，也不證明方法已還原。可在無 SDK 環境執行 `python3 -m unittest scripts.tests.test_mobile_swift_string_abi` 驗證採集器。

## 模組化 MBA 簡化

`SymReadability.*` 涵蓋減法與補數的表示、結合律運算成本、單位元及寬字面值、共用樹飽和、有預算的候選選擇，以及關閉取樣後的三位元窮舉等價性。`SymMBASample.*` 對照 AP 求值器檢查窄值與任意精度驗證，涵蓋所有運算子、確定性賦值及未使用的寬輸入。跨評分版本比較候選品質時，須用相同指標重算兩側輸出；SDK 的版本化大小計數僅供診斷。

## ARM32 與堆疊框架轉送測試矩陣

```sh
cmake --build build-release --target NeverDSymbolicTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

堆疊框架溢出測試矩陣透過兩套 C 後端涵蓋 x86-32（ELF/COFF/Mach-O）、ARM32（ARM 與 Thumb ELF）及 AArch64（ELF/COFF/Mach-O）。重複的私有框架讀取必須化簡為加減法，並在兩種最佳化等級正確執行位元組配對、跨字邊界配對及確定性亂數字。Clang AST 檢查完整函式中的殘留 MBA 運算，同時區分合法位址表示式。HighFrameStoreForwarding 涵蓋精確存取寬度、區域變數變更、記憶體寫入、前綴別名、部分重疊、有序記憶體、錯誤或循環圖及呈現展開預算；窄位取補回歸於語義化簡後執行。HighCStoreForwarding 在四種架構保留轉送值所依賴的定義存活，包括浮點重新解讀及額外直接使用。SymSimplifyGuard 檢查載入身分與順序、volatile/atomic 狀態及 poison 邊界。ELFARM32ModeTest 驗證 ARM/Thumb 選擇、位址正規化、純對映符號物件、混合模式中繼資料保留及同一位址矛盾證據的拒絕。ELFARM32ModeCAPITest 在同一 SDK 工作階段以混合中繼資料取代 Thumb 映像，確認反組譯、HighC、LLVMC 的明確錯誤，再重新載入 Thumb 驗證解碼器復原。InstructionMode 涵蓋對應的解碼、程式碼指標、直接分支與程式碼產生邊界。缺少跨目標 Clang 應標記為略過，不能當作格式通過的證據。

`NeverDHighControlFlowTests` 中的 `HighBoundPrivateFrameCopies.*` 檢查呼叫 ABI 綁定後經非逃逸私有框架槽傳播的副本，涵蓋分支、堆疊槽重用及不同條件脈絡的一致性。x64 與 AArch64 產生的 C 在 `-O0`、`-O2` 下啟用未定義行為陷阱，並與獨立算術結果比較。反例要求在框架位址逸出、呼叫 ABI 未知、框架別名缺失或不一致、入口參數被重新指派、存取重疊、有序或原子記憶體、格式錯誤的陳述式、循環及預算耗盡時保留原函式。一般值轉換不能標記為 PHI 複製。

原始碼投影也會在清理後重新驗證變參物件清單：允許空的指令位址錨點，但拒絕隱藏效果或控制轉移。同步清理允許同一已儲存接收者的單層 `int64_t` 或 `uint64_t` 檢視；窄化、浮點轉換、位址運算和重新賦值仍會被拒絕。Foundation 物件集合及正常、例外解鎖軌跡均在 `-O0` 與 `-O2` 下執行驗證。

## x64 原生同步例外

checked x64 的 `DIV`/`IDIV` 使用處理器結果與 `#DE`。KVM 透過私有 supervisor IDT/IST 接收例外，WHP 使用明確的例外攔截位圖；原始上下文與可用錯誤碼和傳輸錯誤分開保留。OS 模型先消費可恢復事件，再安裝明確的繼續執行上下文。Windows 驅動將零除與商溢位映射為 `STATUS_INTEGER_DIVIDE_BY_ZERO`，執行實際 SEH filter、`__finally` 與重試。`NeverDX64ExceptionTests` 可停用 Unicorn 建置，`DriverWDMCPUException` 驗證原始 WDK 用例；缺少 ARM64 主機時明確跳過。

## 分階段提交 RAM 效果

`RAMTransaction` 在物理執行租約內，只保存一條指令明確宣告之寫入範圍的物理聯集。結果觀察器執行前恢復原始 RAM；取消、後端傳輸錯誤和觀察器例外不會發布部分 RAM 或暫存器。CPU 例外在 RAM 回復後保留架構例外狀態。ARM64 的單次與成對寫入共用此記憶體權威層。x64 支援 8/16/32/64 位元 `XCHG`、`XADD`、`CMPXCHG`，LOCK 或隱式鎖定形式要求自然對齊。`NeverDRAMTransactionTests` 將結果與獨立宿主 CPU 對照，並驗證回復、別名和權限；不可用的平台明確跳過。裝置交易與平行 SMP 仍不在此契約內；CPU 快照不會撤銷已提交的 RAM。

## 完整 x87 狀態

`NeverDEmulationArch` 獨立負責 ISA、頁表及 FP 狀態佈局，原生與 Unicorn 傳輸共用此層。x64 上下文保存 x87 控制、狀態、TOP、實體標籤、操作碼、指令／資料指標及八個 80 位元暫存器。`FP0`–`FP7` 使用 `RegisterValue`，純量存取拒絕截斷；`FPTag` 是實體非空位圖。`NeverDX64FPTests` 涵蓋全部 TOP、精確運算的主機 FXSAVE/FXRSTOR 對照與上下文還原。這不新增 checked x87 指令，也不證明全部捨入語義；缺少原生主機時明確略過。

`driver-strict` 支援匹配 Linux x64 主機的 KVM 與 Windows x64 主機的 WHP；`auto` 選取對應原生傳輸，跨 ISA 執行選取 Unicorn。明確指定 Unicorn 及原有 V1 API 保留可移植軟體設定。原生執行在進入 CPU 前檢查規範位址和指令效果；硬體不可用時明確失敗且不回退。不支援的指令與 OS 行為仍明確報錯。Windows x64 原生 CI 在停用 Unicorn 的設定下通過全部 359 項必測檢查：131 項 CPU 檢查、26 個內建映像與 46 個 WDK 映像及 40 個情境組合在首選和重定位位址產生的 224 項驅動程式結果，以及 4 項 SEH 邊界檢查 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). 原生 ARM64 實機證據仍待補充，這不表示相容任意驅動程式或 Android/Darwin 環境。

上述原生驗證涵蓋已宣告的驅動程式進入點及已發佈情境。下文的逐功能回歸以及 C API／CLI／Python 檢查，除非明確記錄 Windows 執行結果，其證據範圍仍限於 Linux；原生樣例集通過不代表每一種測試變體均已在 Windows 驗證。

使用 `executionCapabilities(Contract, ISA, Backend)` 查詢所選後端的能力設定。`NativeLegacyX64` 描述原生 x64 驅動程式執行；`NeverDNativeDriverTests` 驗證原有驅動程式集，也可在停用 Unicorn 的組建中執行。

現有 CI 工作流程在通用測試設定前執行完整模擬測試目錄，並在 `emulation-focused` 儲存探索清單、JUnit 結果和 CTest 日誌。其他模組的失敗不會阻止這組測試執行。硬體不可用及選用驅動程式範例缺失仍明確記錄為略過；軟體執行或編譯通過不能作為原生執行證據。

在 Linux 上，`NeverDUnicornDeadlineTests` 透過受控 pthread 排程，讓實際計時執行緒在客體入口前完成。測試涵蓋 x64、ARM32 和 ARM64，要求入口前取消不產生客體效果，並驗證下一次執行使用獨立預算。測試呼叫公開引擎 API，不修改引擎私有狀態。

`NeverDX64ExceptionTests` 中的 `X64StateTransition` 在原生 CPU 上執行獨立 RAM 讀取和 CR8 讀取，交替改變 TLS 基址與權限級，在重複除法例外後恢復，並在取消進入後更改 TLS。修改原生狀態傳輸時，應執行所屬 CTest 標籤，同時涵蓋別名重新映射、CPU 上下文、FP 狀態和原始驅動結果對比。KVM/WHP 不可用仍明確跳過。


`NeverDKvmRunTests` 無需 `/dev/kvm` 即可驗證 `KvmRunControl` 借用的傳輸回呼。`StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` 檢查準備、讀取和被攔截的主機進入使用同一執行緒，且中斷重試期間只準備一次。其他案例涵蓋準備失敗而不進入、讀取失敗、準備期間停止及活動進入被取消；隨後重新執行，確認不會重用舊回呼。驗證仍應包含真實取消、RAM 回滾、例外和原有驅動程式測試。 `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` 驗證同一期限內的多次進入重用該執行緒，每輪傳輸只執行一次，並保持先前狀態封包不變。

KVM x64/ARM64 透過 `KvmRunControl` 在同一專用 vCPU 工作執行緒準備狀態、進入 `KVM_RUN` 並讀取狀態。`EINTR` 重試僅準備一次；取消進入或讀取失敗不能發布。`KvmAArch64Machine.cpp` 在該執行緒執行位址轉換維護與完整純量、向量傳遞，共用一次單步期限。呼叫執行緒僅在確認完成後提交；ISA 解碼、RAM 交易、OS 策略和觀察器仍屬於呼叫執行緒。ARM64 原生執行仍缺少實機證據。

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` 在主機修改通用暫存器、首尾 XMM 暫存器、MXCSR 和 x87 控制字後，驗證連續執行及真實 CPU 寫入。停止進入後的實際 `FXSAVE64` 位元組驗證全部實體 80 位元暫存器、TOP、標籤、操作碼和指標；重複除法例外也會使重用失效。這些機器邊界測試不向 checked 設定開放額外的 x87 指令。

`NeverDKvmStateTransferTests` 在真實 KVM 執行後注入暫存器或 XSAVE 讀取失敗，再以未變更的輸入重試。獨立的整數和封裝位元組結果證明失敗的讀取不會重用已前進的原生狀態。只有該測試程式包裝 `ioctl`；原生主機不可用時明確略過。

Checked ARM64 使用統一的完整狀態提交邊界。`Registers.def` 定義 39 個純量欄位及 32 個 128 位元向量暫存器；`captureAArch64State` 暫存所有讀取、套用宣告位寬與 NZCV 正規化，最後一次提交。Unicorn、KVM 和 WHP 傳遞相同清單，包括 TPIDR_EL0、TPIDRRO_EL0、TPIDR_EL1、FPCR 和 FPSR。原生介面透過 CPACR_EL1 啟用 FP/SIMD。任何純量或向量讀取失敗、進入取消，皆保留完整呼叫方狀態。

ARM64 KVM/WHP 初始化執行私有 `AArch64MachineProbe.def` 程式：NOP、向正無窮捨入的 FP32 加法及雙通道 SIMD 加法。每步比較全部 39 個純量欄位與 32 個向量，包括 TLS、NZCV、目的暫存器高位清零及保留和累積的 FPCR/FPSR 狀態。自檢只使用特權級監控儲存，共享一個總截止時間。成功僅驗證這段有界初始化程式；仍需獨立的 ARM64 原生工作負載驗證。

x64 KVM/WHP 原生初始化在私有 supervisor 頁面執行 `X64MachineProbe.def`。單一時限涵蓋 NOP、朝正無窮捨入的 FP32 加法、雙通道 SIMD 加法、FS/GS 載入及 CS/SS/CR8 讀取；每一步比較完整的純量、XMM、實體 x87 和控制狀態。x64 與 ARM64 自檢都必須取得實體記憶體的獨占執行租約。`MemoryProjection` 統一保存快取身分（ISA、位址空間、映射世代、權限及監控變體）和各 ISA 已提交的頁表根歷史。建構器在改寫私有位元組前使快取失效；失敗的重建不能重用部分寫入的頁表，呼叫者也不能傳入過期頁表根。這些自檢僅證明有界初始化；ARM64 原生工作負載仍缺少獨立驗證。

共用 XSAVE 解碼器區分標準格式與壓縮格式的 SSE 初始狀態。XSTATE_BV[1] 清零時，兩種格式都初始化 XMM 暫存器；標準格式仍讀取並驗證 MXCSR，壓縮格式才初始化 MXCSR。`X64XsaveCases.def` 提供獨立的資料配置和原創主機 XRSTOR 程式。`X64XsaveTests.cpp` 檢查拒絕狀態的原子性，並以真實主機執行對照兩種格式，同時保留呼叫端 FP/SSE 狀態。主機架構或所需指令功能不可用時，對照測試明確略過。

`X64FPState.def` 宣告壓縮 AVX、AVX-512、CET_U/CET_S 和 AMX 傳輸配置，包括元件的 64 位元組對齊。存在的擴充資料必須符合架構的全零初始狀態；缺席元件資料與對齊填補不定義狀態。偏移由配置位元決定，未知配置、非初始資料或錯誤長度會在發布前失敗。`CompactedOffsetsFollowLayoutRatherThanPresentBits`、`WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`、`InitialCETComponentsDoNotHideFPState` 和 `InitialWideComponentsDoNotHideFPState` 涵蓋 872 及 10752 位元組 WHP 封包。這項傳輸支援不准入上述擴充指令。

`WhpXsaveRegisters.def` 使用具名 x87/SSE 控制暫存器補充完整 XSAVE 資料封包。最後操作碼及指令/資料位址會明確寫入並從主機讀回；可補齊封包中的零值欄位，但非零中繼資料衝突或共用控制欄位不一致時，會在發布狀態前失敗。`NamedMetadataRestoresOmittedPacketFields` 驗證欄位缺失情境，並保留完整 FP 資料。

原生 `FOP/FIP/FDP` 遵循主機 x87 儲存、還原規則。沒有未遮罩的待處理例外時，AMD 可能清零這些欄位；快照保留實際觀測值。`X64MachineProbe.def` 與精確 NOP/上下文測試使用一致的待處理例外狀態，確保每個欄位有效並逐項比對，不遮蔽差異。主機行程 FXRSTOR64/FXSAVE64 參考程式涵蓋兩種狀態；後端不會以輸入中繼資料取代主機結果。

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` 比對客體實際執行 FXSAVE64 寫入 RAM 的完整 FP/SSE 狀態與主機 XSAVE 讀回。兩種 API 都測試直接安裝與客體內 FXRSTOR64，分別使用預設指標儲存特性及明確選擇的主機支援設定，以區分進入、客體執行和擷取邊界。測試不修補傳回值；不一致仍然失敗。 邊界矩陣亦涵蓋未遮罩的待處理 x87 例外，並記錄主機行程直接執行 FXRSTOR64/FXSAVE64 的參考結果和處理器廠商，以區分條件式指標儲存語義與 WHP 狀態傳輸行為。

共用的 `encodeX64XsaveState` / `decodeX64XsaveState` 編解碼層擁有標準及壓縮 FP/SSE 封包、實體 TOP 輪轉、缺失元件的初始狀態和原子驗證。WHP 使用完整 XSAVE API，優先選擇 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`，舊 XSAVE API 作為相容路徑。個別的舊 x87 暫存器介面不能取代完整封包。非初始擴充元件、格式錯誤的標頭、非法控制位元和截斷擷取明確失敗。WHP 映射錯誤保留 HRESULT、GPA 和大小供診斷。

`CheckedX64Instructions.def` 透過既有 CPU 後端准入 8/16/32/64 位元無號 `MUL` 和 `CBW/CWDE/CDQE/CWD/CDQ/CQO`。`NeverDX64IntegerTests` 使用獨立的 `X64IntegerCases.def` 編碼和預期值，在兩種特權級驗證部分暫存器保留、32 位元零擴展、乘積高低兩部分、已定義的 CF/OF 結果及符號擴展不改變旗標。一般 RAM 乘法保留完整存取範圍的權限檢查和讀取觀察回呼；故障或觀察回呼停止會保留隱式輸出暫存器及 PC。裝置運算元仍不支援。這些案例也在 checked Unicorn 上執行；不可用的原生後端明確略過。

`X64BitInstructions.def` 支援 16/32/64 位元暫存器及一般 RAM 的 `BT/BTS/BTR/BTC`。暫存器位元索引依運算元寬度解讀為有號數並選取完整資料字；立即數索引限制於基底位址的資料字內。位址寬度截斷先於 FS/GS 基底位址相加。CF 與寫入值由處理器提供；`RAMTransaction` 在觀察回呼接受前保留私有執行結果。完整範圍權限檢查涵蓋獨立頁面配置與別名。停止、回呼失敗或頁面權限不足均保留原始 CPU 與 RAM。LOCK 僅支援自然對齊的記憶體修改形式；MMIO 與硬體平行 SMP 仍不支援。`X64BitStringTests.cpp` 使用獨立編碼與 x64 本機實際執行對照，檢查負索引、寬度截斷、跨頁存取、取消及非法 LOCK 形式。參見 [Intel 指令參考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。

`X64StringInstructions.def` 統一管理一般 RAM 上 8/16/32/64 位元的 `MOVS/STOS/LODS`；`CLD/STD` 只改變方向旗標。每個 REP 元素在觀察回呼前驗證整個運算元，並於一個可恢復邊界提交。後續錯誤保留先前完成的元素；取消或回呼例外不改變目前元素。FS/GS 僅作用於來源位址，且在位址寬度截斷之後相加。AL/AX 載入保留高位元，EAX 載入零擴展。32 位元位址模式的零次 REP 要求計數高位元為零，MOVS/STOS 還要求參與的位址暫存器高位元為零，否則不同真實 CPU 實作會產生不同結果。MOVS/STOS/LODS 的 REPNE 形式與 STOS/LODS 裝置運算元仍不支援。`X64StringTransferTests.cpp` 用獨立的主機指令對照寬度、方向、重疊和零次數，並分別檢查權限、別名、回繞、錯誤與恢復。原創 WDK 資源驅動程式透過 `driver_resource_strings.def` 執行四種寬度的 STOS/LODS。

`X64StringInstructions.def` 也統一管理一般 RAM 上 8/16/32/64 位元的 `CMPS/SCAS` 與 `REPE/REPNE`。每個元素在觀察回呼前驗證全部讀取運算元，更新六個算術旗標，並於首次符合終止條件時退出。資料錯誤會恢復本次連續 REP 執行開始時的旗標，同時保留已完成的指標與計數更新；公開介面恢復執行時，以已發布的 CPU 狀態重新開始。停止與觀察回呼例外不改變目前元素，提前終止也不會讀取下一個元素。FS/GS 僅影響 CMPS 來源位址；SCAS 保留累加器與未使用的來源暫存器。裝置運算元及有歧義的 32 位元零次數高位元狀態仍不支援。`X64StringComparisonTests.cpp` 以獨立主機指令對照旗標、方向、別名、回繞、權限與恢復，並透過 Linux x64 訊號測試讀取實際錯誤時的暫存器。原創 WDK 資源驅動程式透過 `driver_resource_strings.def` 執行四種寬度的兩類條件重複形式。參見 [Intel 指令參考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。

`WhpResourceCache.h` 將邏輯 CPU 狀態與 WHP 分割區分離。執行階段保留一個作用中的原生分割區：同一 CPU 連續單步會重用它；切換 CPU 時先銷毀舊分割區，再重建映射、虛擬處理器並還原完整狀態。邏輯 CPU 保留獨立的 `MemoryProjection` 檢視和權威 RAM。取得租約遵守取消訊號和目前截止時間；銷毀非作用中 CPU 不會銷毀其他 CPU 的分割區。x64 保留主機預設 XSAVE 特性組合，並透過 `WHvGetPartitionProperty` 驗證實際分割區，不透過清除相依特性強制縮減遮罩。CPU 協作式切換不提供平行硬體 SMP。

`NeverDX64FPTests` 檢查全部 79 個啟動狀態損壞位置，並在原生傳輸上執行獨立組譯的 `X64ProbeCases.def` 指令，驗證單一時限及客體 RAM 不變。`NeverDProjectionCacheTests` 涵蓋呼叫者切換、ISA 順序、頁表根歷史、權限/監控變體、映射世代、位址空間身分及失敗重建。`NeverDRunControlTests` 的 `WhpXsaveTests.cpp` 檢查新舊 API 封包、所有 TOP、大小邊界及失敗時狀態不變；記憶體協定測試不能取代 WHP 原生證據。不可用的原生傳輸明確略過。

XSAVE 驗證診斷區分長度查詢、本機資料準備和擷取資料解碼，並保留 API 名稱、傳回位元組數、容量及有限的標頭/控制欄位；獨立預期位於 `WhpHostFailureCases.def`，不輸出客體暫存器內容。`InvalidInputReportsPreparationWithoutHostMutation` 也驗證無效輸入不會呼叫主機或修改其資料。共用 ISA 編解碼器仍是唯一驗證入口。

WHP 在能力查詢、分割區/虛擬 CPU 初始化、暫存器/XSAVE 傳輸及執行中的主機呼叫失敗，均保留 HRESULT 和 `WhpProtocol.def` 中宣告的 API 名稱；能力查詢失敗仍傳回具型別的不可用結果。 `WhpHostFailureCases.def` 提供獨立錯誤預期，涵蓋與取消同時發生的主機失敗，以及新版/舊版 XSAVE 查詢、安裝和擷取失敗。 Windows 專項 CI 要求 166 項原生案例通過：16 項映射、2 項啟動、10 項 FP/上下文、7 項共用 CPU、8 項整數案例，以及 `NativeInstallRetainsFPStateBeforeAnyGuestExecution` 的兩種 API 變體。後兩項在執行客體程式碼前比對完整 FP/SSE 與獨立讀取的中繼資料。 缺少註冊、略過、停用或未執行都會使原生證據稽核失敗。 新增的 26 項檢查涵蓋 `X64BitStringTests.cpp` 在兩種特權層級下的全部案例。 Windows PE64 要求 33 項 WHP 程序案例與兩項獨立原生 Windows 對照案例。

`NeverDMemoryLifecycleTests` 獨立於 Unicorn 建置，也涵蓋僅啟用原生後端的組態。停用 Unicorn 時，專用的軟體投影/裝置案例明確略過；符合主機的共用 CPU 案例仍會註冊。`WhpMemoryTests.cpp` 使用 `WhpMemoryCases.def` 中的 16 個案例隔離原生記憶體 API：單頁/投影大小的後備記憶體、共用/獨立配置、未觸頁/已駐留位元組，以及存在/不存在第一個虛擬處理器。每個案例保留兩個存活的邏輯擁有者，反覆切換其映射分割區，銷毀非作用中擁有者，並驗證剩餘映射無需重建即可繼續使用。實際映射錯誤保留 HRESULT 並使測試失敗；這是記憶體 API 證據，不是指令執行證明。

`X64MachineProbe.def` 的啟動診斷列出失敗指令，以及所有不一致的純量、TLS、特權級、x87 控制欄位、實體 FP 通道和 XMM 字，並保留預期值及觀測值。`DiagnosticIdentifiesStepFieldAndBothValues` 使用獨立的預期訊息驗證。狀態比較仍要求完全一致；診斷用來區分傳輸遺失與指令執行問題，失敗的原生探針仍判為失敗。

`WhpResourceTests.cpp` 涵蓋快取重用、替換前銷毀、失敗復原及截止時間/停止競爭。`LogicalCPUSwitchingRestoresPhysicalFPAndTLS` 在兩種權限模式下交替執行兩個存活機器，檢查獨立的實體 x87/XMM 和 FS/GS 狀態，並在銷毀同伴後恢復剩餘機器。Windows CI 要求兩種權限的 WHP 案例都執行通過。

`NEVERD_ENABLE_SEMANTIC_TESTS` 預設為 `ON`，控制 `unittests/semantic` 中的測試組及其彙總執行目標。建置不依賴 Unicorn 的原生 CPU 測試時，保留 `BUILD_TESTING=ON`，同時設定 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` 和 `NEVERD_EMULATION_BACKEND_UNICORN=OFF`。原生 KVM/WHP 測試仍可建置，包括具備對應 SDK 標頭的 Windows ARM64/MSVC 組態。在 Windows ARM64 上啟用 Unicorn 仍需 ARM64 LLVM-MinGW 工具鏈。這項建置解耦不等於 ARM64 原生執行驗證。

僅原生 CPU 的 CI 檢出初始化固定版本的 Capstone 原始碼，並使用經驗證的預建 LLVM 套件。設定 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` 且停用 Unicorn 後端後，CPU 測試目標的設定、建置和連結均不需要 Unicorn 原始碼，也不依賴簽章庫及外部語料。預設 CI 仍啟用完整語意測試組。

現有 `ci.yml` 在 Windows x64 runner 上提供明確選擇的 `native_cpu_only` 手動模式。`NativeCPUTests.def` 選擇十個測試目標；`run_native_cpu_ci.py` 先建置它們，再執行篩選後的 CTest，並儲存清單、JUnit、日誌和摘要。共用 CI 解析器區分通過、失敗、略過、停用和未執行結果。每個宣告的 WHP 原生映射案例都必須被探索並執行；缺少或略過原生證據會使聚焦工作失敗。預設的 LLVM 原始碼建置 CI 保持原樣。協定測試和編譯不能取代 WHP 或 ARM64 原生工作負載驗證。

在 `native_cpu_only=true` 時，設定 `native_driver_tests=true` 可啟用不依賴 Unicorn 的 `NeverDNativeDriverTests`。設定前，`build_wdk_driver_fixtures.py` 驗證微軟官方 WDK/SDK 10.0.26100.6584 套件的完整 SHA-256，並從原始程式碼重建 46 個一般、CFG 或 DBG 驅動程式映像。`WDKDriverFixtures.def` 統一定義套件身分、編譯與連結參數及範例繫結。未修改的微軟檔案與授權保留在本機建置或快取目錄；CI 僅上傳建置中繼資料與記錄。清單記錄工具版本、命令、原始碼與標頭摘要及輸出映像摘要。

`NativeDriverTests.def` 要求 `DriverBuiltinImages.def` 與 `DriverBackendParityCases.def` 中全部 112 個工作負載產生 224 個 WHP 結果：26 個內建映像、46 個 WDK 映像及 40 個要求情境，均涵蓋原始與重定位位址。加上 166 項 CPU 檢查及 4 項共用 SEH 續接回歸，共有 394 項必測結果。固定位址映像保留預期的重定位拒絕。遺失或略過 WDK 映像與情境會使這項選用 CI 工作失敗；一般本機建置仍可不提供外部範例。`run_native_cpu_ci.py --with-drivers` 記錄已設定的測試目標與完整清單及 JUnit 證據。建置成功不代表 Windows 或 ARM64 原生執行已驗證。本機可用下列命令重現，也可將產生的快取載入現有模擬建置。 `166 CPU + 224 WHP + 4 SEH = 394`.

C SEH 範圍仍使用左閉右開區間。合法的 `__C_specific_handler` 落點可能位於其保護區間內：[LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) 將 `EndLabel + 1` 寫為區間末端。Windows OS 模型保留原始端點，並獨立驗證目標可執行性、所屬函式及續接身分，重定位後亦然。`KernelSEHContinuationCases.def` 保留原始範例布局；`ScopeEndLabelMayOverlapTheHandlerLandingPad` 涵蓋常數處理常式與篩選函式。配套測試驗證末端排除，以及非法目標遭拒後派發狀態仍可重試。這些純模型檢查納入 `NeverDNativeDriverTests`，停用 Unicorn 時仍會執行。

目標展開同樣使用原始範圍末端：若處理常式目標仍在某個 `finally` 的保護區間內，便不會離開該範圍。`FinallyRespectsRawScopeEndAtHandlerTarget` 檢查邊界兩側，並在 Windows x64 上直接對照 `ntdll.dll!__C_specific_handler`。NeverD 不修補編譯器產生的區間。Clang 20/21 建置的原始範例在 `T`、`J` 模式下傳回客體失敗，因為偏移後的末端包含選定目標；Clang 23 建置會執行兩級清理。[LLVM 修改 #144745](https://github.com/llvm/llvm-project/pull/144745) 移除了舊的 `+1` 偏移。這類編譯器相關結果與後端故障分開記錄。

建置清單涵蓋全部原創 WDM/KMDF C 範例及可選 WDK CMake 路徑。每個公開的 `driver-*-scenario.json` 都在 `DriverBackendParityCases.def` 中具有一般與 CFG 案例；缺少原始碼、建置或情境繫結會使清單測試失敗。`Original` 清除情境中的載入位址覆寫並核對映像慣用基底，`Rebased` 核對宣告的重定位基底。驅動程式自有 IRP 情境會主動取消一個子要求，因此 `DriverNativeOutcomes.def` 在正常清理後仍保留其預期的整體失敗結果。

```bash
python3 scripts/build_wdk_driver_fixtures.py \
  --output build-driver-fixtures --cache build-driver-packages
cmake -S . -B build-native -G Ninja \
  -C build-driver-fixtures/fixtures.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
```

`NeverDAArch64StateTests` 驗證每個純量欄位、每個向量的兩個字、特權級改變、浮點指令未執行及傳輸錯誤診斷的保留。`NeverDAArch64FPTests` 在兩種特權級的真實傳輸上執行 `OriginalProgramChecksCompleteStateAndOneDeadline`，使用獨立組譯的 `AArch64ProbeCases.def` 原始指令。測試把這些與 PC 無關的指令遷到客體程式，保持監控頁僅供特權級存取。Unicorn 執行和原生後端的明確略過不能取代 ARM64 原生啟動證據。

`CheckedAArch64Instructions.def` 和 `AArch64InstructionEffects` 在 EL0/EL1 接納有界的基礎 FP32/FP64 算術、比較、移動與定寬 SIMD 運算。FPCR 支援四種捨入模式、FZ 和 DN；FPSR 保留累積狀態與 QC。不支援的控制位元及狀態位元在修改前拒絕。FP16 算術、SVE/SME、未遮罩例外、選用擴充及未列出的形式明確失敗。這些 CPU 能力不代表已支援 Windows ARM64 驅動程式載入或新增 OS 環境。

`AArch64InstructionEffects` 負責純量及 FP/SIMD 單次、成對 RAM 存取範圍，單一運算元最大 128 位元。共用位址空間在進入 CPU 前驗證每頁；`RAMTransaction` 僅提交完整宣告的實體寫入。128 位元寫入觀察器在生效前依序收到兩個 64 位元字。停止與故障保留 RAM、向量及位址寫回。Xn/Vn 編號重疊合法；位址回繞的成對存取被拒絕。`NeverDAArch64MemoryTests` 使用獨立的 `AArch64CrossPageCases.def` 與 `AArch64VectorMemoryCases.def` 編碼。

`NeverDAArch64StateTests` 在兩種特權級驗證完整狀態的全部 71 個讀取位置，包括位寬正規化、缺失讀取器與重試。`NeverDAArch64FPTests` 執行 `AArch64FPCases.def` 原始指令，驗證所有向量通道、打包運算、純量及向量浮點結果、四種捨入模式、FZ/DN、累積 FPSR、上下文狀態及擴充和控制位元拒絕。`NeverDAArch64MemoryTests` 涵蓋每個跨頁偏移、觀察器順序與停止、權限拒絕、別名及恢復後的向量輸入。`NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`) 在真實執行後注入每個純量與向量讀取失敗。不可用的原生傳輸明確略過；測試與交叉編譯無法取代 ARM64 KVM/WHP 實機證據。

`NeverDAArch64MemoryTests` 在兩種權限層級下涵蓋 Unicorn、KVM、WHP 的 18 種純量及成對指令，檢查所有跨頁偏移、符號延伸與寬度結果、觀察器順序、第二頁權限不足或缺失、明確消費故障後重試、重複實體別名，以及別名替換後的上下文還原。修改前已重現合法跨頁載入遭拒的情況。無法使用的後端明確跳過；Unicorn 驗證及交叉編譯不能取代 ARM64 KVM/WHP 實機證據。

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) 檢查零旗標的未啟用 CFG 中繼資料、兩種載入位址下不變的回退指標、無效指標槽/目標及缺失重定位。執行案例明確選擇 Unicorn/KVM/WHP，並使用 `driver-strict` 和 `checked-x64-v1`；不可用的後端分別略過。`DriverPublicCLICases.def` 為 CLI 與相容的 v1 C API 比較選擇 `--backend unicorn`。原生後端與 `auto` 選擇保留獨立的公開介面涵蓋範圍，主機 API 不可用時不會靜默回退。

checked Unicorn 使用 `MachineRunControl`：ARM64 維護、客體執行與完整狀態回讀共用一次單步額度。`UC_HOOK_CODE` 在指令入口檢查借用的停止權杖和期限；同步引擎呼叫返回前解除 hook 借用，機器單步則保留控制直到發佈狀態。Unicorn 與 WHP 暫存完整 CPU 狀態，並在成功步驟發佈前檢查同一控制條件。WHP 在準備前只建立一次額度。已確認的 x64 CPU 例外優先於回讀期間到來的停止要求。回讀取消時，checked RAM 交易捨棄推測寫入；非受限軟體契約不變。 `MachineInterruptedError` 區分已確認取消與主機或回讀失敗。共用 checked CPU 返回 `Stopped` 或 `Deadline`，保留 CPU/RAM 並允許重試；真實故障即使伴隨停止要求也仍是 `BackendFailure`。

狀態回讀回歸： `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests` 在真實 x64 和 ARM64 引擎的兩種權限級執行 `UnicornMachineControlCases.def` 中的原始儲存指令。`RejectedEntryPreservesStateAndRAMAndAllowsRetry` 檢查單步前取消、實際客體入口處停止或期限到期、全部輸入狀態與 RAM 保持不變，以及隨後成功執行一次儲存。測試專用入口包裝不需要虛擬機監控器，也不構成 ARM64/WHP 原生執行證據。

`RunDeadline::invoke` 在 WHP 入口已停止或逾期時拒絕呼叫主機，取消期間保留真實主機結果，並在釋放借用的停止標記前確認中斷回呼結束。KVM 和 WHP 在持有執行租約的呼叫執行緒上驗證完整擷取的私有狀態，然後分類同時到達的停止或逾時。真實主機錯誤、擷取失敗以及經過認證的 x64 CPU 例外保持較高優先順序。一般成功狀態在取消檢查結束前保持私有；已確認的中斷捨棄推測性的 CPU/RAM 效果並允許重試。準備、原生執行和擷取共用一次單步寬限。這些控制提供協作式取消，不保證硬性實際時間上限。

`NeverDRunControlTests` 包含可攜式 `NativeEntryTests.cpp` 和 Windows 啟用 WHP 時的 `WhpEntryControlTests.cpp`。記憶體主機回呼驗證拒絕入口、重試、晚到取消、真實錯誤保留、完成結果優先順序和已確認的回呼生命週期，無需 Hyper-V。`NeverDKvmRunTests` 檢查呼叫執行緒完成、錯誤優先順序及重入拒絕。真實 `NeverDKvmStateTransferTests` 執行 `KvmStateTransferCases.def` 原始指令；`ActualCPUExceptionOutranksStopDuringCapture` 和 `PublicCPUExceptionOutranksStopDuringCapture` 在真實暫存器/XSAVE 讀取後停止，並保留除零例外、原始上下文、RAM 和明確恢復。Wine 上採用 Windows ABI 執行的可攜式測試僅提供執行緒及控制協定證據，不證明原生 WHP 執行。不可用的原生後端仍明確略過。

`windows-pe64-v1` 支援有界 Windows x64/ARM64 主控台程序，包括 PEB/TEB、EXE TLS、具名 Win32 API 與明確無環啟動 DLL 圖。客體 DLL 支援名稱／序號程式碼及資料匯入、DIR64 重定位及真實載入器串列身分。DLL 進入點／TLS、動態載入、轉送匯出、CRT／GUI、使用者 SEH 與執行緒仍待完成；原生 ARM64 KVM/WHP 證據仍缺。

輸入總位元組與映像總範圍各受 `memory_limit` 限制，執行期映射也計入映像預算。準備階段共用 65,536 筆紀錄、64 MiB 中繼資料讀取、名稱長度及整體截止時間；阻塞主機 I/O 無硬即時保證。原創 EXE→DLL→DLL 樣例驗證重定位指標、序號呼叫、共享資料、API 指標身分、`MEM_IMAGE`、載入器串列及 EXE TLS 掛接／分離。`NeverDWindowsProcessTests` 包含直接原生 Windows 對照，`NeverDPEProgramExportsTests` 驗證畸形資料與預算，`NeverDProcessPublicTests` 驗證 C ABI/CLI 目錄一致性。不可用後端明確略過。

Windows 虛擬記憶體新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及目前行程的 `FlushInstructionCache`。OS 層管理保留區域，`AddressSpace` 統一管理已認可頁面、權限和實體儲存。測試涵蓋動態程式碼改寫、存取錯誤和記憶體額度回收。

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

外部寫入分離回歸涵蓋精確及不足預算、部分字、大小端、未觸及記憶體的身分、完整仿射槽邊界、晚到前驅覆寫與預設失效。公開 C API／CLI 檢查 v6 版面相容性和無效域。HighC、LLVMC 輸出均於 O0／O2 檢查回傳值、記憶體、堆疊與保留狀態；這些測試不等於原生等價證書。
