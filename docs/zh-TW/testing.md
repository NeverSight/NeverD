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

新增回歸在兩種記憶體模式和位元組順序下保留純位元運算 intrinsic，並檢查普通呼叫、operand bundle、convergent、生命週期、記憶體副作用及陷阱仍構成邊界。原始及改寫程式在 O0/O2 啟用未定義行為陷阱，對照獨立基準檢查傳回值中的數值位址及緩衝區每個位元組。

位元組活躍性回歸涵蓋不重疊讀取、部分被觀察的寫入，以及多個後續寫入組成的覆蓋。條件式位址測試涵蓋 AND/OR/XOR、32/64 位元、兩種位元組序、錯誤根、不完整遮罩、繞過檢查的合流及每個位址預算邊界。O0/O2 獨立 oracle 檢查兩個分支結果、全部十六種位址餘數及輸出緩衝區的每個位元組。

位址關係回歸涵蓋整數與指標 PHI/select、雙端序與指標寬度、穩定及變動的回邊、未定義/freeze 輸入邊、無入口錨點的環、相鄰位址/輸出預算及僅改寫位址時的分析失效。O0/O2 迴圈以逐次獨立 oracle 比較回傳值與整個緩衝區，包括不能視為常數位址的移動位址反例。

數值位址測試涵蓋單一寫入者的完整與子字轉發、不新增快照的 undef/poison、雙端序、32/64 位元表示、模運算負位移、部分重疊、不同根與 alloca 的別名、可擲出例外的呼叫、迴圈、相鄰工作量與用途預算，以及僅刪除寫入時的分析失效。O0/O2 執行將原始與改寫 IR 對照獨立基準，同時檢查傳回值及別名緩衝區的每個位元組。

`NeverDMedMutableSourceTests` 和 `NeverDLLVMCValueTests` 在 O0/O2 下執行獨立撰寫的迴圈、區塊重排、入口回邊、執行期堆疊運算、較早讀取、分支匯合、部分別名、布林真值和包含零輸入的位元計數。反例要求在發射前拒絕畸形輸入、截斷目標、歧義承載和預算耗盡。超過 SSA 限制的 CLI 案例要求 LLVMC 輸出可執行，並要求 HighC 明確拒絕。 連續更新和跨區塊儲存運算式鏈也會檢查產生 C 的大小與執行結果。

新增迴歸限制 LLVM 提升前的私有讀寫數量和 C 輸出大小，執行長混合運算鏈、重新排序的 SSA 區塊、重疊的客戶記憶體寫入及零回傳值的 O0/O2 檢查；關鍵案例還經過實際 LLVM 最佳化管線，並驗證模組產生遭拒後可以安全重用發射器。

複合條件回歸在 O0/O2 下執行包含非零常數相等、無符號比較、兩種運算元順序的有符號比較、擴寬布林輸入及全部布林否定組合的合取和析取。C 發射必須保留完整真值表，且不得解引用不存在的零比較運算元。整數位址儲存涵蓋對齊與未對齊的 32/64/128 位元承載；位元組儲存陣列保留明確對齊及精確的首位址與部分存取，不得產生純量對陣列賦值或不相容型別的別名存取。

`NeverDLowIRRefinementTests` 涵蓋實際恢復的殘餘圖、不同結構的有限迴圈、零次迭代、獨立動態產生者、條件見證、重疊輸入視圖、複製與溢出關聯、兩邊不可變讀取證據、強制系統旗標及返回槽保留。錯誤候選、額外寫入、不完整或無限路徑、過期證據、暫存區衝突與共享預算耗盡必須拒絕證書；既有獨立性測試仍拒絕可觀察的任意值。

`NeverDLowIRRefinementTests` 中的 `CompleteModel`、`CompletedTargetFacts`、`ConditionalImplication` 和 `PartitionedCoverage` 案例使用小輸入域的獨立窮舉判定，並檢查格式錯誤的輸入、過期快取、不完整列舉及剛好/不足預算。實際 LowIR 與二進位精化案例在固定邏輯閘數限制下檢查條件乘積及原始/恢復圖的所有終止分支，拒絕被修改的最終觀察、不相關輸入域與缺少的目標。`FiniteValues` 測試區分編碼失敗與搜尋、值數量及全域預算造成的拒絕。

同一目標中的 `LowIRLoopRefinement.*` 和 `BinaryLowIRLoopRefinement.*` 涵蓋任意 64 位元計數、巢狀字典序排名、真實原生殘餘程式碼、入口前綴範本、重疊視圖及相關溢出。負例拒絕錯誤迴圈本體、縮小入口域、不下降的排名、無號回繞、遺忘先前寫入、遺漏切點、畸形範本和共用預算耗盡。成功的有限分支不能授權不完整的歸納證明。

`LowIRLoopInference.*` 和 `BinaryLowIRLoopInference.*` 使用獨立編寫的計數器、堆疊儲存、提前返回、原生呼叫和封裝旗標案例，涵蓋窄位元算術拓寬，以及運算式不同但語意相等的旗標狀態。格式錯誤的圖、缺失或偽造的來源、不終止／回繞迴圈，以及推導或證明預算耗盡均不得產生憑證。

零前綴回歸檢查串接分組、非標準位寬及窮舉位元組對，並保留未知與非零位元。獨立編寫的框架迴圈涵蓋兩種位元組序中分別寫入低位值與高位零、窄位寬、錯誤算術與填補、恰好及不足的推斷預算，以及獨立的完整證明查詢預算。

共用入口與共用回跳區塊的迴歸涵蓋零擴展的 32 位元及完整的 64 位元計數器、非單位步長純量排名、錯誤結果、不進展與回繞路徑，以及純量和組合排名搜尋之間恰好足夠或耗盡的累計預算。`LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`。 另有遞增與重設迴歸，要求不必每輪只展開一個計數器位元即可收斂，並拒絕缺乏進展和無符號回繞。 排程迴歸涵蓋迴圈攜帶的非單位步長累加器、有效非單位步長純量排名旁可能回繞的單位計數器，以及有效組合排在早期視窗之後的三個計數器。恰好足夠和少一次的排名預算檢查確定性的繼續搜尋，並防止重複候選。

同一目標中的 `LowIRLoopPlanPairing.*` 檢查暫存器重新命名、不同算術主體、雙方獨立前綴快照、述詞保留、共用框架輸入、巢狀切點覆蓋和獨立證明預算。缺少關係、錯誤寫入、無效暫存值繫結、不完整配對或中繼資料預算耗盡均不得產生憑證。

`LowIRLoopAlignment.*` 使用獨立編寫的一般與旋轉框架計數迴圈：兩個預設自關係計畫各自證明成功，首次配對失敗，另一個候選切點則證明關係成立。迴歸涵蓋多切點排列、錯誤結果與框架寫入、缺失或過期的原始記錄、明確的未定義值 witness、不遞減或回繞的計數器、格式錯誤的圖、失敗嘗試的累計查詢、恰好足夠的總預算及搜尋限額耗盡。任何拒絕結果都不得包含憑證。 新增案例涵蓋分離的重設與進展階段、移動退出判斷後需要跨族配對的等價迴圈、不重複推斷的共用快取，以及快取中繼資料合計超限。後接獨立迴圈的案例以明確的 16384 次查詢上限驗證完整週期覆蓋。空候選族與重複候選族不消耗符號查詢，切點不足必須拒絕。同時檢查恰好足夠與少一次的全域預算、錯誤結果、缺失進展、原始證據及未定義值 witness。 過濾候選族回歸涵蓋保持結果不變的算術菱形、局部匯合與僅在邊界匯合，以及能繞過可達匯合點退出或返回迴圈邊界的路徑。測試原始過濾族重複時仍嘗試候選過濾族、先成功的過濾計畫由後續完整分支嘗試重用、恰好足夠／少一次／零 `MaxCutSelectionWork`、失敗後累計的 `CutSelectionWork`，以及全域圖工作預算耗盡後不啟動符號推斷。兩種分支候選族都驗證完整週期覆蓋；菱形關係使用明確的推斷與證明查詢上限。

局部計數器回歸涵蓋框架和暫存器、遞增和遞減、低位／中間位／高位、非標準寬度、兩種位元組順序及三位元組框架字。測試檢查延遲發現新區間、竄改保留位元、不進展和無保護回繞、無效新增入口、恰好足夠／不足的推導預算，以及既有單切點搜尋。

前置階段回歸涵蓋兩個及三個順序迴圈重用同一倒數計數字、與原有巢狀迴圈階段組合、恰好足夠與少一次的排名／查詢預算、不進展迴圈，以及重設回前一階段。同寬但錯誤的階段常數、錯誤結果及框架寫入必須由完整檢查器拒絕且不產生憑證；缺失原始證據仍回報不支援。

`NeverDLowIRRefinementTests` 中的 `InterpreterMachineStateModel.*` 使用獨立撰寫的 LowIR 案例，檢查原始入口旗標、狀態碼與客體 RAX 的區分、全部 17 個狀態字、部分暫存器分片、封裝旗標、動態拒絕狀態的持續保留、客體堆疊框架寫入、兩個分支以及循環推斷後的全新證明。錯誤輸出、遺失狀態、記憶體變更、過期指令記錄、非法輸入和產生預算耗盡必須失敗。既有機器狀態原始碼測試也涵蓋兩條 C 路徑的 O0/O2；模型測試本身不證明編譯後的 C。

`NeverDLLVMInterpreterModelTests` 將獨立編寫的 LLVM 與完整狀態 LowIR 參考實作比較，涵蓋位寬、平行 PHI、switch、客體記憶體、獨立狀態碼、poison 檢查、內建函式值域、被拒絕的契約及四種建模預算。測試完成任意字長倒數迴圈的完整證明，並拒絕遭改寫的狀態碼。獨立 C 用例經 O1/O2 編譯後必須滿足相同觀察契約。這些測試驗證受支援的模型；自動不變量發現與編譯器正確性仍是獨立義務。 變數位移用例涵蓋全部四種位寬、經遮罩或分支限制的位移量、邊界及越界位移量、無回繞與精確旗標、嚴格 poison 拒絕，以及 O1/O2 編譯後的 C。

`LLVMGuestAlignment.*` 將載入和儲存與獨立位元組記憶體參考實作比較，涵蓋正確和錯誤的對齊域、自由高位址位元、解析後的預設對齊、部分寬度、未使用或被覆寫的存取、不可達分支及恰好/少一單位的建構預算。`InterpreterLLVMRefinement.GuestAlignmentRequiresBothFreshPremises` 透過兩次新鮮關係檢查驗證原生堆疊儲存、相符的入口同餘條件和遭改寫的原始碼效果。

`LLVMByteSwap*`、`LLVMScalarByteSwap.*` 和 `InterpreterLLVMRefinement.ByteSwapRequiresBothFreshPremises` 以獨立位元組複製及位移／遮罩參照檢查高位元組保留、跨區塊值、poison 保留、嚴格呼叫契約與獨立計數的精確／少一預算。Clang O1/O2 測試必須包含實際交換 intrinsic；小型原生位元組交換／BSWAP 測試檢查雙方的新證明，並拒絕錯誤值或遺漏高半字清零。

`NeverDLLVMScalarEquivalenceTests` 涵蓋完整迴圈域、零次迴圈、PHI 同時交換、switch、高位輸入、最後分區反例、產生 poison 的額外更新、回傳範圍、不支援的契約，以及精確、少一單位及零預算。獨立雙寬與溢位參考實作涵蓋各支援字寬的漏斗位移端點及帶溢位約束的乘法；獨立巢狀迴圈 C 於 O1/O2 檢查編譯器輸入形態。狀態模型測試也檢查漏斗位移端點。`SymExpr.ConstantWindowSharesActualWorkWithoutRelaxingQueryCeilings` 檢查累計查詢計費與不變的局部上限。

`LLVMScalarDecision.*` 涵蓋深層精確位移/擴展約束、常數分支決策、兩條迴圈回邊、保留高資料位元、末端未定義操作、不終止、已檢查函式的後續修改，以及精確/少一/局部預算。`LLVMScalarDecisionCompiled.DeepOneAndTwoBackedgeOracles` 將獨立撰寫的單回邊和雙回邊遞推與無號 C oracle 在 O0/O2 下比較，共 32,768 次呼叫。這些是純量模型檢查，不代表原生 ABI 或完整二進位還原涵蓋。

`LLVMScalarDemand.*` 在固定工作預算內證明不同的非線性迴圈函式本體，保留全部16個控制分區及自由高位輸入，拒絕最後分區的輸出或定義性失敗，並檢查精確/不足的工作預算及節點上限。`LLVMScalarDemandCompiled.*` 在 O0/O2 對照獨立無號算術 oracle，共執行262,144次呼叫。測試驗證查詢時機，不刪除來源操作或縮小輸入域。

`SymKnownBitsTests` 以全部位元組輸入對及任意精度邊界值檢查事實，涵蓋擴展恆等關係、不回繞加法、不同來源及全定義位移語意。亦檢查精確/少一預算、快取命中計費、儲存上限、獨立上下文、深度、運算元數量與不支援的位寬。`SymExprExtensionTests` 檢查常數高位元與完整位移計數。純量等價回歸在證明值域約束時保留符號資料，拒絕最後分區差異及已執行的 poison，並以精確/少一預算核驗完整證明計費。 `SymMBAExtensionTests` 在關閉取樣驗證時要求推導證據，檢查全部位元組輸入對，並保留有號、窄進位、位元反相及工作量耗盡的邊界。

`NeverDLLVMScalarLoopRecoveryTests` 涵蓋前綴與前驅攜帶值重構、零次迴圈分支、自回邊迴圈判斷前移、仿射狀態、回繞時的等式退路、額外更新的 poison、高位元資料差異及不支援的輸入契約。精確／少一累計預算檢查原子拒絕；獨立算術 oracle 在 O0/O2 執行原始與恢復 LLVM，涵蓋全部位元組控制輸入。這些測試不代表原生 ABI 恢復或預設短 C 輸出已完成。

迴圈述詞回歸涵蓋 8/16/32/64 位元 PHI、交換比較運算元與分支極性、有號/無號擴展、不同外部初值、衝突入口觀察、多回邊、隱藏符號資料反例、截斷拒絕、原始 poison/不終止及構造/證明/候選/轉換限制的原子拒絕。獨立無號 oracle 在 O0/O2 對照原始與恢復後的 LLVM，共 458,752 次呼叫並啟用未定義行為陷阱；來源函式及父模組保持不變。

退出邊界回歸涵蓋8/16/32/64位元模運算條件、遞減步長、兩種極性、零次迴圈、共享出口、僅零資料一致的錯誤相鄰邊界、步長無法到達的邊界、超限切片、poison、缺少同寬葉節點及原子預算拒絕。有效前綴初值前的40個未使用參數不得擠占有界搜尋位置。獨立無號 oracle 在 O0/O2 執行原始與恢復後的 LLVM，共458,752次呼叫並啟用未定義行為陷阱。

同一測試目標也涵蓋 `recoverLLVMScalarSource`：搜尋前準備、僅清理而無迴圈變換、全位寬未使用狀態、死碼中的溢位/精確位移/除法及 assume 義務、不支援的副作用、精確/少一累計預算與有界繼續。獨立算術 oracle 在 O0/O2 執行原始與準備後的 LLVM，涵蓋所有位元組控制值及確定性全位寬狀態。成功或拒絕都必須保持來源函式及其父模組不變。

來源準備守衛測試涵蓋帶註解的模恆等式、不同前驅中的等價運算式、不同分支值保留、原始溢位/exact 位移/截斷/擴展拒絕及精確/不足累計預算。獨立無號 oracle 在 O0/O2 對照原始與準備後的函式本體，共131,072次呼叫並啟用未定義行為陷阱。既有語義 pass 測試繼續涵蓋獨立述詞的拒絕規則。

遮罩回歸涵蓋交換運算元、零欄位、保留輸入高位元、完整資料證明失敗後的替代值、所有回邊、回繞／溢位拒絕、超過32項的批次，以及精確／少一預算下的原子拒絕。獨立 LLVM 與產生 C 的算術預期在 O0/O2 下檢查其與位寬恢復的組合。自等價回歸保留完整控制域、poison/undef 與不支援約束的拒絕、非終止、局部上限和精確／少一工作計費；修改同一函式後，舊結果不再有效。這些仍是純量 LLVM 覆蓋，不是原生 ABI 認證。

共用遮罩包含關係測試窮舉全部位元組輸入對，涵蓋非連續遮罩與最高128位元字，保留未知／高位元，並限制超出項數和位寬上限時的節點增長。帶兩條回邊的符號迴圈必須相對獨立閉式運算式證明遮罩 XOR 遞推。控制相依測試按正規化後的位移量儲存計費，並保留精確／少一預算及寬值保守退路。

位寬回歸涵蓋非零常數、不變簽章、更寬輸入和intrinsic、可觀察高位元、有號次序、新增溢位及精確／不足預算。同一純量候選在x86-64、AArch64、大端AArch64及ARM32目標三元組下測試，屬於LLVM層涵蓋範圍，不代表原生ABI認證。原始／還原LLVM與產生C分別通過O0/O2獨立算術oracle，C啟用未定義行為trap。

種子回歸涵蓋全部外部入口一致性、多回邊、隱藏高位資料、poison、平行交換、失敗批次分拆、超過 32 個攜帶變數及原子預算。純量證明測試區分已完成的未知查詢與全域工作量耗盡，並在不列舉資料位元時證明安全位移。符號測試窮舉位元組值、遮罩及計數，保留可觀察高位、來源身分與大計數語義。跨位寬位移視圖必須共用完整數值計數。這些檢查不認證原生 ABI 恢復。

新增迴圈迴歸涵蓋窄位寬末索引回繞、分離的 body/latch、兩種入口條件方向、等式運算元交換、單位步長變數換序與遞減、零次路徑的高位資料差異、新增執行的 poison 和錯誤邊界候選。精確／少一建構及證明預算和候選耗盡保持整體拒絕。原始 LLVM 與產生 C 在 O0/O2 下對照獨立算術 oracle 執行。

`NeverDLLVMCScalarLoopRecoveryTests` 檢查預設整模組與單函式輸出、回傳路徑清理後的持續恢復、函式身分和屬性、呼叫繫結、既有及新增內建函式與符號衝突、共用預算、副作用呼叫、缺少輸入定義性、中繼資料、映像投影、外部區塊位址和外來函式選擇。獨立算術與旋轉 oracle 在 O0/O2 下執行產生的 C，並啟用未定義行為陷阱；算術也與獨立編譯的原始 LLVM 對照，涵蓋所有位元組控制輸入、邊界字值及確定性的完整位寬資料。

`SymSimplifyPredicates.*` 也對照獨立階段與完整 pass 的策略、回報工作量、恰好與少一單位預算、停用階段及帶混淆標記的函式。純量原始碼回歸涵蓋 8/32/64 位元算術編碼迴圈終止條件、有號溢位拒絕，以及跨輪次與函式共用的建構限制。發布失敗時保留原 IR；產生的 C 在 O0/O2 下與獨立編譯的原始 LLVM 及算術 oracle 對照執行。

`SymKnownBits.*` 以窮舉位元組配對算術檢查無損遮罩與有號位移往返，涵蓋負值、不同來源、捨棄的未知位元、不符的因子或位移量及寬位移過移。直到 128 位元的檢查保留精確與少一單位的查詢預算，且不新增 DAG 節點。純量決策測試加入不同回邊的正負數更新、來源修改後舊事實及溢位拒絕、保持符號化的高資料位元，以及對獨立無號 oracle 的 16,384 次 O0/O2 呼叫。

`SymKnownBits.*` 也窮舉位元組配對檢查倍數排序與跨位寬乘積，拒絕回繞、錯誤係數或因子重複次數，以及將窄位寬溢位移至更寬的字。直到 128 位元的查詢保留精確與少一單位工作預算，且不新增 DAG 節點。純量測試不列舉資料位元即可證明重複加法及乘法，保留所有迴圈溢位義務，並對獨立 oracle 執行 16,384 次 O0/O2 呼叫。

`LLVMScalarAssume*` 檢查完整迴圈域、最後分區失敗、不可達與已到達的假條件、全部位元組輸入上的累積定義性、精確/少一預算、IR 修改及不支援的呼叫契約。四個目標 triple 驗證共享建模；8,192 次 O0/O2 呼叫對照獨立無號 oracle。狀態模型測試另行檢查相同義務及運算元 bundle 拒絕。

`LLVMScalarProjection.*` 涵蓋巢狀欄位、位元視窗、未使用參數保留、多個回傳、回邊、未選取運算的溢位/位移/assume 義務、最後分區失敗、不終止、未知契約、輸入修改及精確/少一預算。四種目標三元組驗證共用語意。`LLVMScalarProjectionCompiled.*` 透過 LLVM 陣列橋接原始聚合，與投影視窗及獨立無號算術在 O0/O2 下對照。`SymExpr.RightShift*` 窮舉位元組對，檢查符號延伸、進位、保留高位元、完整計數與有界探索。

`LLVMScalarInputs.*` 檢查有序混合位寬映射、零參數與無名介面、死算術及 assume 的輸入需求、輸出突變、未知契約、包裝拒絕，以及精確/不足的累計預算。證明測試恢復完整原始簽名，不固定省略輸入。`LLVMScalarInputsCompiled.*` 在 O0/O2 執行原始及精簡循環介面，對照獨立無號 oracle，並改變所有省略參數。

`NeverDLLVMScalarStateProjectionTests` 涵蓋重疊與非對齊視窗、8/16/32/64 位元單元、迴圈、入口遮罩、來源修改、狀態範圍、保留 poison、外部記憶體拒絕及精確/少一預算。原記憶體函式與 LLVM 聚合橋在 O0/O2 下對獨立位元組/算術 oracle 執行 172,032 次比較；純量證明另行檢查。`SymKnownBits.*` 窮舉位元組對並檢查 128 位元、不同因子、放寬遮罩、回繞加總及預算邊界。`LLVMCIntrinsicSemantics.AssumeEvaluatesItsConditionAndRefusesBundles` 驗證 O0/O2 條件單次求值與 bundle 拒絕。

迴圈中繼資料回歸測試在全部控制分區及精確/少一預算下，比較計數迴圈與獨立公式。過大或為零的剝離歷史計數不能掩蓋錯誤結果、不終止或 poison。透過 API 建立的畸形中繼資料單獨驗證匯入器的拒絕行為，避免與 LLVM 組合語言解析混淆；機器狀態測試保留狀態副作用及輸入限制。

初始化契約回歸涵蓋部分及分離的位元組範圍、固定別名、兩個分支、每個返回點、迴圈首輪讀取與迴圈內先寫後讀。先讀後寫、漏寫、客體寫入、未知別名、特殊記憶體存取、物件外範圍，以及輸入／工作預算耗盡，都必須失敗。獨立的僅輸出狀態字 C 案例在 O1/O2 編譯後保留精確 LLVM 屬性，並通過全新的原生到 LLVM 組合證明。

受條件保護的倒數測試涵蓋拒絕本體模板後的重試、任意字長輸入的完整迴圈頭證明、切點與查詢預算的累計計費，以及真實入口契約違規時立即拒絕。

`NeverDInterpreterLLVMRefinementTests` 檢查全新的原生到 LLVM 組合證明、精確文字／函式綁定、獨立預算、完整觀察項及刻意擴大的原始碼域。修改位元組、殘餘程式、結果、旗標、狀態碼、框架寫入、poison 或錯誤／過期迴圈方案，都必須拒絕組合憑據。任意字長倒數要求兩段歸納前提；獨立 C 案例在 O1/O2 編譯後驗證實際序列化 LLVM 輸入。狀態模型回歸拒絕隱藏入口回邊，對入口集合計費且不複製附屬來源資訊。

`InterpreterLLVMRefinement.Preservation*` 涵蓋局部／重疊範圍、非法請求、獨立計算的準備開銷、兩端相同的最終破壞、跨迴圈的入口保存／還原、新鮮不透明狀態證據及後期拒絕。`NeverDPEFixedImageTests` 也是 API 使用端，需要重新建置。未提供請求時的結果、計數與摘要另行對照基準。

`InterpreterLLVMRefinement.Collection*` 檢查有限與歸納證明所需的保留／延遲策略、可達壞分支、後期原始碼拒絕、入口保存及全部四種原生策略身分。編譯組合層遺漏傳遞的故障，確認每項必要選項確實到達原生檢查器。

```sh
cmake --build build-release --target NeverDLLVMCScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMCScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarEquivalenceTests --parallel 4
build-release/bin/NeverDLLVMScalarEquivalenceTests
cmake --build build-release --target NeverDLLVMScalarResultProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarResultProjectionTests
cmake --build build-release --target NeverDLLVMScalarStateProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarStateProjectionTests
cmake --build build-release --target NeverDLLVMScalarInputProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarInputProjectionTests
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

實體返回回歸涵蓋直接及間接呼叫跳過無效行內位元組、可達的錯誤返回位置、完整目標列舉及恰好和不足的預算。完整狀態關係拒絕被改動的結果；獨立 C 經 O1/O2 編譯後也必須保留完整框架寫入，返回值相同不能掩蓋返回槽位元組變化。

顯式原生交疊測試涵蓋真實 x64 跳入立即數的分支、兩個可行分支的結果，以及位於先前指令內部的間接返回入口。合成提供器測試涵蓋兩種收集順序的包含式交疊、未執行直接分支上的衝突位元組、兩種順序的程式碼／讀取一致性及候選讀取。精確和不足的位元組預算按間接轉移累計計入重複交疊位元組。分支結果改變、靜態或循環介面使用及證據矛盾均必須拒絕憑證；啟用選項或改變額度會改變摘要。

拒絕邊界的明確啟用測試涵蓋不可達的 RCL、LOCK 記憶體 XADD 和 REP MOVS、符號路徑矛盾、任意值控制的分支，以及入口、間接跳躍、CALL 和 RET 抵達時的精確拒絕。測試也檢查可達後綴的獨立入口、候選與原生位址重合、格式錯誤或不完整的證據、資源耗盡、靜態/迴圈介面拒絕，以及精化證明的三層摘要繫結。修改不可達指令，或在無保留邊界時切換選項，都會改變證書摘要。這些測試驗證宣告的有限證明範圍，不證明未審計指令的語義。

封裝旗標測試涵蓋全部純量入口旗標組合、特權遮罩、兩次執行的 TF/AC 條件、不同未定義產生點、相關副本、原生呼叫、兄弟路徑狀態、強制最終系統狀態觀察、畸形證據及資源計費。有限迴圈必須結束每條可行輸入路徑；安全分支不能掩蓋無限或截斷路徑。RDSSPD/RDSSPQ 檢查涵蓋 16 個通用暫存器和兩種寬度、高位元保持、保留 `Missing` 證據及偽造投影拒絕。機器狀態測試在兩個 C 後端的 O0/O2 下啟用未定義行為陷阱，與獨立使用者模式旗標預言機比較，並檢查環境失敗狀態不會被後續操作清除。 INCSSPD/INCSSPQ 測試涵蓋兩種寬度和全部通用暫存器、不可達邊界保留、安全兄弟路徑完成後的可行陷阱、零運算元及偽造陷阱證據。

`NeverDX86DecodeDetailTests` 涵蓋三種解碼入口、兩種 x64 位址寬度、有號位移邊界、必要前綴、真正的 i386 disp16、moffs、截斷輸入及無詳細資訊的重複使用。只有精確的重定位欄位可繫結，錯誤寬度、偏移或數值均不能繫結。原生獨立性及關係證明測試也保留完整框架寫入，拒絕可觀察的任意旗標及被修改的移位候選。

`NeverDLowUndefinedDigestTests` 檢查獨立 SHA-256 向量、每個已儲存欄位、有號序號位元、順序、排除填補位元組及輸入不變性，涵蓋內嵌緩衝增長、199/200 操作邊界與更長增量範圍。`LowIRRefinement.StaleUnusedInputRefusesAcrossDigestStorageBoundaries` 在兩條路徑檢查真實過期證據的拒絕與重新繫結。保留 `NeverDLiftTests` 的 `InputDigest.*` 覆蓋；修改獨立實作時重新編譯受影響呼叫端。記錄實際 sanitizer、可攜路徑及主機覆蓋；摘要微基準無法單獨證明原生等價。

```bash
cmake --build build-release --target NeverDLowUndefinedDigestTests --parallel 4
build-release/bin/NeverDLowUndefinedDigestTests
```

`NeverDX86UndefinedEffectsTests` 檢查未定義位元中繼資料、已定義／保留旗標及過期憑證拒絕。`NeverDX86CarryArithmeticFlagTests` 以算術參考實作檢查暫存器和記憶體形式 ADC/SBB 的輔助進位。`NeverDX86LogicIdentityTests` 檢查相同運算元的 AND 在 64 位元模式下寫入 32 位元目的暫存器時，仍清零其所屬 64 位元暫存器的位元 63:32，同時保留窄位寬寫入未涵蓋的位元。

`X86RotateUndefinedEffects.*` 以純量算術基準涵蓋全部原始計數、運算元寬度、CL 重疊、高位元組別名及記憶體目的運算元。`X86BitTestUndefinedEffects.*` 涵蓋暫存器/立即數索引、來源與目的重疊、擴充暫存器、已定義旗標及暫存器高位寫入。中繼資料反例拒絕遭修改的運算元、編碼及不支援的形式。原生證明區分同一任意位的關聯讀取與不同任意位，檢查恰好及不足的產生者預算，並拒絕可觀察的未定義溢位。完整狀態細化檢查接受選定見證，拒絕零位見證或遭竄改的候選。

`X86XaddAudit.*` 透過無號算術基準檢查全部 65,536 對位元組輸入、較寬位元寬度的旗標邊界、暫存器/高位元組重疊、兩次寫回、REX 位元組寬度限制及完整暫存器保留。原生檢查要求不產生新的任意位元，同時保留先前的相依性；兩種見證皆接受未修改的 XADD，竄改總和、交換來源值或已定義旗標則遭拒。`/6` 別名使用完整移位計數矩陣，並透過修改群組編號/解碼 ID 的反例拒絕語義錯配。 記憶體測試也窮舉位元組輸入對，涵蓋位址覆寫前綴、擴充、IP 相對及 i386 16 位元定址、有號位移、相鄰位元組及過期解碼細節的拒絕。完整框架原生檢查保留先前任意值相依性及精確／不足預算檢查；竄改儲存位址會違反返回槽契約。

`X86DoubleShiftUndefinedEffects.*` 以獨立逐位元轉移基準檢查 16/32/64 位元運算的全部原始位元組計數，涵蓋來源／目的／CL 重疊及精確生成守衛。原生檢查先捨棄 RAX 再隔離旗標，區分計數 16 與 17，保留先前相依性並執行精確／不足預算檢查。完整狀態關係拒絕被修改的有定義部分及全零位元見證；低字未定義不允許清除有定義的高位元。畸形形式不發布部分證據。

`*Deferred*` 案例涵蓋不可達跳轉側與落空側、缺失或畸形程式碼、符號矛盾守衛、任意控制、選值見證及完整狀態變異。合成提供者驗證不會擷取不可達後繼，而可達的畸形中繼資料仍被拒絕。精確／不足指令、操作、造訪及查詢預算、可達壞分支和無限迴圈均不能認證前綴。策略改變會改變憑證摘要，靜態與迴圈 API 拒絕此選項。

`NeverDPEFixedImageTests` 使用獨立建構的 PE 檔案，檢查含重定位的指令與不可變資料、匯入寫入範圍、畸形標頭／表格、別名及來源資訊竄改。原生到 LowIR 與精確 LLVM 證明接受相符候選，拒絕結果、狀態或原始位元組遭修改的候選。準備預算耗盡維持獨立分類，允許明確提高限額後重試；一般載入也接受含 40000 筆有效重定位記錄、超過預設分析預算的檔案。

`FrameOffsets.*`、`NativeStackSpecialization.*` 與 `OriginalBinaryUndefinedIndependence.*` 檢查 2/4/8/16/32 位元組對齊的全部餘數、自由高位、跨呼叫儲存、倒數迴圈、別名破壞、錯誤分派、無關大遮罩、必要分區升級及恰好／少一次預算。獨立原生控制檢查帶分支約束的對齊、內部無符號返回清理、錯誤清理量與帶前綴返回。這些測試不表示分區迴圈已具備自動原生至 LLVM 的完整證明。

原生框架快取回歸涵蓋以一次載入的查詢預算執行 64 次重複對齊載入、查詢預算少一、位址越界，以及一條路徑返回後另一謂詞下的相同位址。仍執行完整狀態竄改和快取鍵/容量測試。

重複可行性回歸保留全部 130 條原始指令，同時使用與兩條直線指令相同的查詢預算；查詢或指令預算少一仍拒絕，分支及入口域變更後的可達陷阱、求解閘預算耗盡和候選狀態被修改仍拒絕。

框架偏移回歸涵蓋 558 個切片寬度／對齊／餘數／偏移組合，在不足以展開整個根相減的閘預算下證明；並檢查來源或偏移不匹配的切片、未約束的稀疏遮罩、模運算進位與回繞、巢狀遮罩及節點／查詢耗盡。原生測試驗證部分對齊指標的精確儲存，拒絕缺失對齊和框架外存取，並在完整狀態精化中拒絕被修改的儲存值。

入口同餘證明測試涵蓋對齊1/2/4/16的全部餘數、兩個不同根暫存器、自由高位元、非法定義域、矛盾入口常數、不回繞範圍、排除區間空隙、保存義務和精確/少一查詢預算。迴圈歸納模板保留原入口根條件。重新執行的原生獨立性/關係證明與原生至LLVM檢查拒絕不匹配的定義域及改變的狀態碼，憑證摘要繫結兩側定義域。案例獨立撰寫，不從ABI或單次執行推斷對齊條件。 另將獨立撰寫的帶入口檢查C函式原樣編譯為O1/O2，在兩個餘數下證明與實際原生指令序列一致；同一編譯產物在不同餘數下必須拒絕。

暫存器分區回歸涵蓋大小端、高位框架根、覆寫與重疊欄位、後續邊、擴大的前驅、原生 CALL/RET，以及剛好足夠或少一步的預算。兩種 C 路徑在 O0/O2 執行全部四種記憶體情況。原生細化檢查分別綁定兩個選擇器值，並非無約束輸入證明。獨立 LLVM 案例驗證：將假分支移至共享匯合點之前，不可使它在真分支後繼續執行，並檢查 PHI 複製與儲存。

入口對齊回歸涵蓋更細分區、全部允許餘數、不同高位根位址、最後案例失敗，以及精確與少一預算。兩個 C 後端均在 O0/O2 下以不可存取的被拒客體位址及無效旗標驗證：狀態 2 必須保留全部狀態位元組。模型細化檢查相同拒絕語義；C/Python 測試涵蓋 v5 配置、所有權及舊版與未來尾部。原生證明控制明確拒絕未綁定的對齊域。

`StringTransfer.*` 與重複搬移迴歸檢查重疊、零次數、暫存變數隔離、容量／預算限制及指標失效。`MachineStringSourceTests.cpp` 針對四種寬度與兩個方向，將原生執行及兩條 C 路徑的 O0/O2 結果與獨立的全部暫存器、旗標及堆疊參考結果比較。

`ControlDiscovery.*` 與 `NativeStackSpecialization.*` 涵蓋根位址低位元條件、高位元和完整根相依、不完整遍歷、恰好足夠及不足的遍歷預算，以及有限不可變位址證據的保留。

未解析目標的守衛測試涵蓋不可達的非法階段、入口或回邊之後確實可達的未知目標、精確細化次數上限，以及相鄰的成功與耗盡探索預算。拒絕時不得發布殘餘程式碼、來源記錄或讀取見證。完整恢復之後的可選探索開銷不是必要預算的下界。

依需求投影框架關係的迴歸涵蓋自動及既有暫存器載體、高位根位址與模回繞、只約束低位元組的條件、兄弟邊事實衝突或缺失、相鄰查詢預算結果及求解器 unknown。窄位需求不能把部分指標證明當作全寬證明；拒絕恢復時不發布殘留程式碼或見證。

有限子欄位回歸涵蓋高半部與低半部選擇值、可觀察的任意負載、暫存器與框架槽、兩種位元組順序、窄載體、後續取值域擴大、不相交遮罩及缺失事實。缺失的可達目標、無界選擇值、共用查詢預算耗盡及求解器 unknown 均不得發布殘餘程式碼或證明憑據。 測試也涵蓋整字需求下的自動探索、未使用的手動提示、根衍生酬載和前段常數視窗。

公開工作預算測試涵蓋預設值和 32 位元最大值、求值與相依性探索各自耗盡、兩種原始碼 ABI 和 C 後端，以及無效 CLI 參數。C/Python 配置檢查涵蓋 v7、繼承欄位驗證和 v1–v6 對未來尾部的忽略。預算拒絕時不得發布原始碼和復原見證；節點求值計數達到最大值時不得回繞。

可選的重複目的地切鏈測試在同一固定操作預算下比較一般迴圈與巢狀迴圈，執行殘餘迴圈，區分解碼模式，並確認零鏈長行為不變。關聯反例必須在預設策略下成功、啟用選項後拒絕發布。重複原生呼叫／返回槽、相依重播及後到前驅以兩種設定執行。公開旗標測試涵蓋兩種原始碼 ABI／後端、預設與未知旗標、舊 API 忽略擴充及報告布林值。

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

`NeverDParallelExecutionTests` 強制處理器呼叫重疊與寫入等待期間取消，並驗證獨立 CPU 狀態、實體別名原子競爭和私有傳輸暫存。`NeverDRunControlTests` 檢查獨立 WHP binding 可同時持有兩份資源租約，而共享 binding 序列化存取。`NeverDMMIOAtomicTests` 比較原始 x64 原子／更新指令與全部 ARM64 LSE 案例的裝置及 RAM 結果，涵蓋寬寫入兩次觀察、過期預覽、提供者失敗與提交／停止競爭。`KernelMMIOFailure` 涵蓋別名、同值寫入、重複提交、電源變化、解除映射與擁有者銷毀。不可用平台明確跳過；包裝層會合只證明處理器呼叫能並行，不證明硬體同時退休指令。ARM64 KVM/WHP 原生驗證仍需對應主機。

```bash
cmake --build build-cpu --target NeverDParallelExecutionTests NeverDMMIOAtomicTests NeverDRunControlTests --parallel 4
ctest --test-dir build-cpu/unittests/emulation -L '^NeverD(ParallelExecution|MMIOAtomic|RunControl)Tests$' --output-on-failure
```

## Linux 程序設定檔測試

獨立[程序測試套件](process-emulation.md#驗證)會編譯真實 x64/AArch64 ELF fixture。`NeverDLinuxProcessTests` 檢查啟動、program-header 政策、服務續接、二進位輸出、客體錯誤與資源停止。`NeverDProcessPublicTests` 經由 C API/CLI 測試且不變更分析映像。`NeverDExecutionSessionTests` 檢查兩個 CPU 共用記憶體／預算，以及要求／錯誤恰好消耗一次。`NeverDX64MemoryUpdateTests` 檢查記憶體算術、SETcc、BT、XMM/MXCSR、寫入 observer、REP 邊界及預備裝置讀取。`DriverBackendParityTests.cpp` 執行原始與重定位 WDK fixture，並將完整可觀察報告與 Unicorn 比較；缺少映像／後端會明確略過。

checked x64 亦支援帶遮罩的傳統 `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN`、`MAX` 的 `SS`、`SD`、`PS`、`PD` 形式。`X64SSEInstructions.def` 統一定義運算元寬度、對齊與准入規則。`MaskedSSEArithmeticMatchesIndependentHostExecution` 以獨立本機 CPU 參照驗證暫存器與 RAM 形式，涵蓋四種捨入模式、FTZ、有符號零、次正规輸入及 NaN；`SSEMemoryObserverStopsBeforeResultAndStatusChanges` 驗證停止請求先於效果提交。未遮罩例外、x87、AVX 仍未開放。

`X64PackedIntegerTests.cpp` 使用 `X64PackedIntegerCases.def` 中的原始編碼和 180 組固定向量結果，並與原生 x64 編譯器 intrinsic 獨立核對。暫存器與頁尾別名 RAM 案例保留其他 XMM、整數哨兵值、FLAGS、MXCSR 和來源位元組。觀察器停止／失敗與可恢復讀取故障保留狀態，修復後可重試一次。未對齊觸發 `#GP(0)`；MMX、LOCK 與 MMIO 仍在回呼前拒絕。原生 CI 強制執行 WHP 的兩個特權級。

`X64PackedShiftTests.cpp` 與原始 `X64PackedShiftCases.def` 使用 16 個立即數計數和 21 個變數計數，將十種移位與獨立純量計算、宿主 SSE2 intrinsic 對照。涵蓋計數／目標別名、高位忽略、對齊、觀察器、可恢復故障和裝置拒絕。`X64VectorTestSupport.h` 與打包算術測試共用暫存器與 RAM 斷言。原生 CI 強制執行 WHP 的兩個特權級。

`X64VectorMaskTests.cpp` 將獨立原始編碼與純量位元擷取及原生 SSE intrinsic 比較，涵蓋每個來源位元、全部 16 個 GPR × 16 個 XMM 組合及兩種 REX.W 值。完整公開暫存器快照、RAM 與資料觀察器驗證零擴展及狀態保留；指令停止、回呼失敗與不支援的形式不得發布副作用。KVM/WHP 原生驗收要求執行兩個特權層級。

`X64ShuffleTests.cpp` 使用 `X64ShuffleCases.def` 中的獨立編碼，並以原生 intrinsic 驗證純量通道選取結果。涵蓋全部 256 個控制值、暫存器與自身來源、頁尾記憶體別名、全部 XMM 暫存器配對、完整公開 CPU 狀態和 RAM、觀察回呼停止與例外、權限、對齊錯誤及重試。MMX、VEX/EVEX、LOCK 與裝置運算元必須無副作用地拒絕。原生 KVM/WHP 驗收要求兩個特權層級的這些案例全數通過；無法使用的主機/ISA 組合仍明確跳過。 `UNPCKLPS`、`UNPCKHPS`、`UNPCKLPD` 與 `UNPCKHPD` 沿用相同狀態/錯誤矩陣及獨立純量、原生對照，並檢查記憶體來源搭配每個 XMM 目的暫存器。

`X64PartialMoveTests.cpp` 與 `X64PartialMoveCases.def` 對照獨立的純量/原生載入儲存參考結果，涵蓋全部 16 個 XMM 暫存器及 NaN/次正規數的原始位元。非對齊存取、別名、跨頁錯誤、權限修復、觀察者停止/失敗和重試均檢查完整 CPU 狀態及兩頁 RAM。頁尾運算元只需八位元組，儲存無需讀取權限。暫存器別名、拒絕形式和裝置回呼另行驗證；原生 KVM/WHP 在兩個權限層級均為必測。

`X64IntegerFloatTests.cpp` 使用獨立的 `X64IntegerFloatCases.def` 編碼、`APFloat` 預期值，以及保存/還原浮點狀態後執行的原生指令。涵蓋兩種整數寬度、四種捨入模式、精度黏滯狀態、FTZ、所有 GPR/XMM 組合及完整 CPU/RAM 保留。非對齊、跨頁、頁尾來源、權限修復、觀察者停止/失敗和重試均檢查精確範圍。原生 KVM/WHP 在兩個權限層級均為必測。

`X64FloatIntegerTests.cpp` 使用獨立的 `X64FloatIntegerCases.def` 編碼、`APFloat` 和原生暫存器/記憶體指令，檢查捨入及截斷轉換。兩種整數寬度均涵蓋帶正負號邊界、中點值、NaN、無窮、次正規數、全部捨入模式、黏滯狀態和 FTZ。檢查所有 GPR/XMM 組合、完整 CPU/RAM 狀態、精確頁尾讀取、可復原跨頁錯誤及觀察者取消/重試。兩個權限層級的原生 KVM/WHP 結果均為必測。

`X64SSEComparisonTests.cpp` 使用獨立的 `X64SSEComparisonCases.def` 編碼、`APFloat` 排序及保存/還原宿主 FLAGS 和浮點狀態的原生指令。21 種原始輸入的全部配對涵蓋 NaN/次正規優先級、零、無窮及相鄰值。檢查所有 XMM 配對與別名、MXCSR 黏滯位元、捨入無關性、DF 保留、完整 CPU/RAM 狀態、精確頁尾/跨頁讀取及觀察者取消/重試。兩個權限層級的原生 KVM/WHP 結果均為必測。

`X64SSEPredicateTests.cpp` 使用 `X64SSEPredicateCases.def` 的獨立條件、`X64SSEComparisonCases.def` 的共用原始輸入、`APFloat` 排序及原生指令對照。測試涵蓋全部輸入配對、混合通道例外優先級、純量高位保留、XMM 別名、完整 CPU/RAM 狀態、頁尾/跨頁讀取、對齊優先級及觀察者/故障重試。直接 Capstone 檢查涵蓋全部控制位元組、兩種語法、兩種解碼 API 與 32/64 位元模式。兩個權限層級的原生 KVM/WHP 結果均為必測；保留控制值、VEX/EVEX 與裝置運算元仍排除。 數值矩陣按指令及比較條件拆分，捨入與控制矩陣按指令拆分。`NativeCPUTests.def` 仍要求所有原始組合；客體時限及 15 秒 CTest 時限維持不變。編譯期檢查要求每個指令/條件組合恰好出現一次。

`X64SSEPrecisionTests.cpp` 結合獨立的 `X64SSEPrecisionCases.def` 編碼、`APFloat` 精度捨入及原生指令對照。範圍檢查使用無界指數下的捨入，涵蓋定向捨入至有限值的溢位及捨入至正規數的微小結果。測試涵蓋正負號、NaN 載荷、全部捨入/FTZ/黏滯狀態、打包通道聚合、XMM 別名、完整 CPU/RAM 狀態、精確來源寬度、對齊優先級、頁面錯誤及觀察者取消/重試。兩個權限層級的 KVM/WHP 案例均為必測。

`X64PackedFloatTests.cpp` 使用獨立的 `X64PackedFloatCases.def` 編碼、有號 `APFloat` 預期值及原生指令對照。測試涵蓋全部輸入對、整數邊界、精度中點、捨入模式、黏滯狀態與 FTZ，並在兩個權限層級檢查暫存器別名、所有 XMM 組合、完整 CPU/RAM、每種 m64 跨頁位置、精確頁尾、對齊優先級及觀察者/錯誤重試。全部 KVM/WHP 案例均為必測。

`X64PackedFloatIntegerTests.cpp` 結合獨立的 `X64PackedFloatIntegerCases.def` 編碼、共用 `X64FloatIntegerCases.def` 輸入、`APFloat`/`APSInt` 及原生指令。43 個原始輸入的全部組合涵蓋 signed32 邊界、相鄰捨入中點、NaN、無窮及次正規數；獨立通道矩陣分別變化精確、非精確、無效及次正規輸入。測試涵蓋全部捨入/FTZ/黏滯設定、XMM 別名、完整 CPU/RAM、對齊頁尾、對齊優先級與觀察者/錯誤重試。兩個權限層級的 KVM/WHP 均為必測。

`DAZBackends` 為比較、謂詞、純量整數/浮點、封裝整數/浮點及精度轉換矩陣增加啟用 DAZ 的涵蓋。`X64DAZTestSupport.h` 使用獨立的 `APFloat` 輸入正規化，並在執行原生對照指令前檢查宿主的 `MXCSR_MASK`。測試涵蓋帶符號零、次正規數、NaN、混合通道、全部捨入模式、FTZ 與黏滯狀態，同時檢查來源位元組、無關暫存器、FLAGS 及完整 RAM 保持不變。暫存器、別名及頁面邊界運算元保留原有的存取觀察檢查。KVM/WHP 要求兩個特權層級的全部 DAZ 案例及 17 個原始宿主指令對照案例均通過；可攜式執行中不支援的宿主會明確略過。原有停用 DAZ 的案例與逾時限制保持不變。

`X64AlignmentTests.cpp` 驗證已准入 aligned SSE 指令的未對齊運算元在資料觀察器、權限檢查或裝置回呼之前回報可恢復或終止性的 `#GP(0)`。故障保留完整公開 x64 暫存器內容、PC 與 RAM；位址寬度回繞先於 FS/GS 基底相加，修復位址後重試原指令。直接 KVM/WHP 機器測試獨立驗證硬體邊界。Windows ring3 已派送明確分類的 `operand_alignment` 故障；其他原因的 `#GP` 仍不支援。

`X64SIMDExceptionTests.cpp` 繞過 checked 准入層，在兩個特權層級驗證 KVM/WHP 的原生 `#XM` 傳遞。`X64SIMDExceptionCases.def` 中八個原始案例涵蓋六類例外，包括精確的次正規結果及無界指數下精度精確的溢位。暫存器與 RAM 形式發生例外時，除規定的 MXCSR 狀態外，完整 GPR、XMM、x87、FLAGS、FS/GS 與客體記憶體皆保持不變。屏蔽例外後重試原指令；保留黏滯狀態並修復運算元，驗證舊旗標不會再次觸發例外。 公共 checked 與驅動契約也執行這些原始故障及重試案例。`WindowsSIMDExecutionTests.cpp` 執行真實原生例外、客體 VEH/VCH 指令，以及跳過、遮罩後重試或修復運算元後繼續，分別檢查處理器入口控制狀態與保存的上下文。啟動反例會拒絕缺失例外、錯誤向量及目的暫存器遭修改的結果，不發布能力。

`check_windows_simd.py` 根據 `WindowsSIMDCases.def` 與純量案例清單建置獨立的原創 Windows x64 程式。6,144 組觀察涵蓋暫存器/RAM 運算元、全部例外遮罩組合、清零或全部設置的黏滯狀態，以及跳過、遮罩後重試、修復運算元且保持遮罩位元不變後重試三條路徑。組合語言入口分別記錄 VEH/VCH 的即時 MXCSR、x87 控制狀態與保存的 `CONTEXT`。測試在還原主機狀態前檢查精確故障 PC、狀態保持、修復後的上下文與重試結果；CI 保留原始記錄與原始碼雜湊。`--build-only` 僅證明編譯成功。這些觀察不啟用 checked 模式下未遮罩的 SIMD，也不代表 ARM64 原生執行。

`WindowsSIMDStatusCases.def` 固定了原生 Windows 上觀察到的全部 63 種非空有效狀態組合。`WindowsSIMDMappingTests.cpp` 檢查精確代碼與參數、拒絕不一致的故障及無效控制值，並透過注入故障邊界檢查例外記錄、CONTEXT 中的兩份控制狀態與遮罩例外後的繼續執行。Windows CI 中的原創程式獨立驗證這些固定結果。注入測試不證明 Unicorn 能產生該例外，也不啟用 checked 模式下未遮罩的 SIMD。

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

`DriverThreadPriorityTests.cpp` 使用原創編譯驅動程式 `driver_thread_priority.c`，在明確指定的 Unicorn／KVM／WHP driver 與 checked 契約下驗證排隊及阻塞執行緒的優先順序修改、時間片內事件／計時器喚醒、同級輪轉、DISPATCH_LEVEL 屏蔽與低優先順序飢餓時的計時器推進。成對計數迴圈證明搶佔後保留準確的剩餘時間片。模型測試涵蓋帶符號 ABI、失敗不改變狀態、已結束物件參照、巢狀身分及獨立回呼堆疊重用。原生案例納入 `NativeDriverTests.def` 強制清單；本機不可用的後端明確略過。

`DriverMutexThreadTests.cpp` 執行 `driver_seh_mutex.def` 中四種原創 WDK 模式：在 SEH 篩選器中遞迴取得、篩選器或異常 finally 取得後保留所有權，以及篩選器阻塞並在另一系統執行緒釋放 mutex 後恢復。Unicorn／KVM／WHP 的 driver 與 checked 契約涵蓋一般／有效 CFG 映像、偏好／重定位位址及協作式／1／17 指令時間片。模型測試亦驗證巢狀堆疊退役後的 APC 停用、錯誤執行緒釋放及最外層返回檢查；KVM／WHP 案例納入 `NativeDriverTests.def` 強制清單。

`KernelWaitSetTests.cpp` 在不依賴 Unicorn 時執行十六項模型測試：部分 `WaitAll`、首個就緒 `WaitAny`、索引快照、逾時清理、後續非法物件／儲存、64 物件邊界、IRQL、保留已退出執行緒及兩個同步計時器。`DriverMultipleWaitTests.cpp` 執行 `driver_wdm_multiple_wait.c` 與 `DriverMultipleWaitCases.def` 的七種原創 WDK 模式，涵蓋 Unicorn/KVM/WHP、兩種驅動契約、一般／CFG 映像、重定位及協作式／1／17 指令時間片。30 項模型／原生結果納入 `NativeDriverTests.def` 強制驗收。 回歸也涵蓋成功或逾時後的重複完成、擷取狀態被更動，以及延遲等待的重複完成。

`KernelMultipleWait.DispatcherScalarParametersIgnoreUpperRegisterBits` 檢查高位元污染、有號邊界、溢位及物件狀態不變性。`DriverMultipleWaitCases.def` 的裸尾呼叫包裝器讓 `driver_wdm_multiple_wait.c` 經由真正的 WDK 匯入執行相同的合法 ABI 呼叫，並涵蓋啟用 CFG、重定位與按指令搶佔。

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
| `unittests/loader` | `NeverDRawISATests` | 二進位檔案：從位元組識別指令集（資料、測試程式自身程式碼、偏移兩位元組的程式碼、各家族 32 位元與 64 位元編碼、以零開頭的檔案）以及 Cortex-M 向量表。`scripts/validate_isa_model.py --engine build/bin/libneverd.so` 依雜湊下載模型從未見過的 180 個真實程式與函式庫進行檢查；它需要網路，不屬於 CTest |
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

`SymSimplifyFinite.*` 涵蓋 8 至 512 位元的完整兩值域片段、共用用途下的收益計算、所有支援且可能產生 poison 的註記、獨立 volatile 讀取與 freeze、明確的 undef/poison、深層迭代走訪、預算及混淆標記。原始與簡化後的 IR 在 O0/O2 下執行，針對所有位元組輸入和隨機全寬輸入與獨立判定程式比較。轉譯物件測試要求不同有限值預算具有不同快取識別。

合併值域測試涵蓋 8–512 位元巢狀選擇、菱形 PHI 與複製循環；衝突回邊、未定義條件、無來源分量、獨立 PHI/freeze 觀測及帶標記產生節點的保留；以及精確的節點、邊與工作量邊界。O0/O2 執行 oracle 窮盡所有位元組輸入對，並改變完整位寬的運算元，驗證選擇、匯合及有界狀態循環。

兩值測試亦涵蓋 8–512 位元巢狀合取遮罩、運算元交換、OR/undef 拒絕、有界深層發現、獨立工作計費與混淆標記策略，以及首次改寫的精確預算邊界。執行 oracle 窮盡所有位元組輸入對，並改變無關的 64 位元資料，在 O0/O2 對照原始與化簡 IR。

`SymSimplifyPredicates.*` 窮舉四位元偏移、符號和輸入，檢查布林區間組合與不連續集合，並在 O0/O2 執行獨立的位元組及全寬 oracle。涵蓋 poison 註記、合流點隱藏的未定義輸入、獨立讀取/freeze、保留的迴圈 PHI、共享用途收益、累計預算、高扇出、遞迴上限和混淆標記。轉譯物件測試要求兩種快取鍵都區分條件分析預算。

`SymExpr.*` 以四位元輸入與遮罩的窮舉檢查非低位常數窗口，並涵蓋寬承載、巢狀結構操作、已知與未知位元組混合重組及算術進位反例。預算回歸將寬節點放在遞迴邊界，並拒絕複製超出預算的寬常數。未知窗口必須維持符號形式，且不得擴展運算式 DAG。 `SymState.*` 也在兩種位元組序下區分純量推導常數與區域字面常數，確保不增加 DAG 節點、不改變完整儲存值。

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

`NeverDHighControlFlowTests` 中的 `HighIntegerSignedness.*` 檢查一個後期 pass：它依每個暫存器或暫時區域變數多數用途的需要，把它宣告為有號或無號。回繞算術、邏輯移位與無號比較傾向無號；有號比較、有號除法、算術移位與符號延伸傾向有號；任何一處非整數用途都會讓該變數保持原型別。產生的 C 在 `-O0` 與 `-O2` 下搭配未定義行為陷阱執行，並與獨立的參考算術比對，其中包括對已變為無號的區域變數做有號比較的情形。

`NeverDHighControlFlowTests` 中的 `HighValueForward.*` 檢查 HighC 寫出器何時可以把只用一次的值折疊進它的使用處。迴圈條件會保留一個其變數在迴圈中被賦值的值，因為同一個名字可能代表多個 SSA 值；重新讀取的堆疊槽在對該槽的寫入之後仍保持原值，而在對其他槽的寫入之後可以折疊。來源變數在使用前被重新賦值時，副本保持原值。每種情形都在 `-O0` 與 `-O2` 下搭配未定義行為陷阱執行。

`NeverDHighControlFlowTests` 中的 `HighCIntegerConversion.*` 檢查 HighC 寫出器交給 C 完成的整數轉換。運算元內部的轉換若保留了外層轉換所保留的位元組，就不再單獨輸出強制轉型；對已宣告整數區域變數的賦值與 return 採用隱式轉換，字面值寫成轉換後的值，而指標保留顯式轉換。記憶體寫入與賦值一樣轉換，傳給有型別的更寬參數的零擴充引數保留其擴充。每種情形都在 `-O0` 與 `-O2` 下搭配未定義行為陷阱執行，並與參考運算比較。

原始碼投影也會在清理後重新驗證變參物件清單：允許空的指令位址錨點，但拒絕隱藏效果或控制轉移。同步清理允許同一已儲存接收者的單層 `int64_t` 或 `uint64_t` 檢視；窄化、浮點轉換、位址運算和重新賦值仍會被拒絕。Foundation 物件集合及正常、例外解鎖軌跡均在 `-O0` 與 `-O2` 下執行驗證。

## x64 原生同步例外

checked x64 的 `DIV`/`IDIV` 使用處理器結果與 `#DE`。KVM 透過私有 supervisor IDT/IST 接收例外，WHP 使用明確的例外攔截位圖；原始上下文與可用錯誤碼和傳輸錯誤分開保留。OS 模型先消費可恢復事件，再安裝明確的繼續執行上下文。Windows 驅動將零除與商溢位映射為 `STATUS_INTEGER_DIVIDE_BY_ZERO`，執行實際 SEH filter、`__finally` 與重試。`NeverDX64ExceptionTests` 可停用 Unicorn 建置，`DriverWDMCPUException` 驗證原始 WDK 用例；缺少 ARM64 主機時明確跳過。

## 分階段提交 RAM 效果

`RAMTransaction` 在物理執行租約內，只保存一條指令明確宣告之寫入範圍的物理聯集。結果觀察器執行前恢復原始 RAM；取消、後端傳輸錯誤和觀察器例外不會發布部分 RAM 或暫存器。CPU 例外在 RAM 回復後保留架構例外狀態。ARM64 的單次與成對寫入共用此記憶體權威層。x64 支援 8/16/32/64 位元 `XCHG`、`XADD`、`CMPXCHG`，LOCK 或隱式鎖定形式要求自然對齊。`NeverDRAMTransactionTests` 將結果與獨立宿主 CPU 對照，並驗證回復、別名和權限；不可用的平台明確跳過。裝置交易與平行 SMP 仍不在此契約內；CPU 快照不會撤銷已提交的 RAM。

`CMPXCHG8B` 與 `CMPXCHG16B` 在 KVM、WHP 和 checked Unicorn 的驅動及使用者模式中執行原始指令。比較成功或失敗都需要讀寫權限，故障按寫入存取分類。`CMPXCHG16B` 在存取記憶體前檢查 16 位元組對齊，不滿足時回報 `#GP(0)`。兩次結果觀察屬於同一 RAM 交易；任一次停止或擲出例外，都不會發布暫存器或記憶體變更。未加鎖的 `CMPXCHG8B` 可以跨頁，加鎖操作仍要求自然對齊。`X64WideAtomicTests.cpp` 對照主機原始執行結果和直接原生故障，並檢查別名、前綴、定址、修復重試及取消。原創 Windows 驅動及 ring3 PE 範例涵蓋兩種寬度，WDK 範例也執行 `_InterlockedCompareExchange128`。CPU 模型必須支援 `CMPXCHG16B`。

## 完整 x87 狀態

`NeverDEmulationArch` 獨立負責 ISA、頁表及 FP 狀態佈局，原生與 Unicorn 傳輸共用此層。x64 上下文保存 x87 控制、狀態、TOP、實體標籤、操作碼、指令／資料指標及八個 80 位元暫存器。`FP0`–`FP7` 使用 `RegisterValue`，純量存取拒絕截斷；`FPTag` 是實體非空位圖。`NeverDX64FPTests` 涵蓋全部 TOP、精確運算的主機 FXSAVE/FXRSTOR 對照與上下文還原。這不新增 checked x87 指令，也不證明全部捨入語義；缺少原生主機時明確略過。

`driver-strict` 支援匹配 Linux x64 主機的 KVM 與 Windows x64 主機的 WHP；`auto` 選取對應原生傳輸，跨 ISA 執行選取 Unicorn。明確指定 Unicorn 及原有 V1 API 保留可移植軟體設定。原生執行在進入 CPU 前檢查規範位址和指令效果；硬體不可用時明確失敗且不回退。不支援的指令與 OS 行為仍明確報錯。Windows x64 原生 CI 在停用 Unicorn 的設定下通過全部 359 項必測檢查：131 項 CPU 檢查、26 個內建映像與 46 個 WDK 映像及 40 個情境組合在首選和重定位位址產生的 224 項驅動程式結果，以及 4 項 SEH 邊界檢查 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). 原生 ARM64 實機證據仍待補充，這不表示相容任意驅動程式或 Android/Darwin 環境。

上述原生驗證涵蓋已宣告的驅動程式進入點及已發佈情境。下文的逐功能回歸以及 C API／CLI／Python 檢查，除非明確記錄 Windows 執行結果，其證據範圍仍限於 Linux；原生樣例集通過不代表每一種測試變體均已在 Windows 驗證。

使用 `executionCapabilities(Contract, ISA, Backend)` 查詢所選後端的能力設定。`NativeLegacyX64` 描述原生 x64 驅動程式執行；`NeverDNativeDriverTests` 驗證原有驅動程式集，也可在停用 Unicorn 的組建中執行。

現有 CI 工作流程在通用測試設定前執行完整模擬測試目錄，並在 `emulation-focused` 儲存探索清單、JUnit 結果和 CTest 日誌。其他模組的失敗不會阻止這組測試執行。硬體不可用及選用驅動程式範例缺失仍明確記錄為略過；軟體執行或編譯通過不能作為原生執行證據。

在 Linux 上，`NeverDUnicornDeadlineTests` 透過受控 pthread 排程，讓實際計時執行緒在客體入口前完成。測試涵蓋 x64、ARM32 和 ARM64，要求入口前取消不產生客體效果，並驗證下一次執行使用獨立預算。測試呼叫公開引擎 API，不修改引擎私有狀態。

`NeverDX64ExceptionTests` 中的 `X64StateTransition` 在原生 CPU 上執行獨立 RAM 讀取和 CR8 讀取，交替改變 TLS 基址與權限級，在重複除法例外後恢復，並在取消進入後更改 TLS。修改原生狀態傳輸時，應執行所屬 CTest 標籤，同時涵蓋別名重新映射、CPU 上下文、FP 狀態和原始驅動結果對比。KVM/WHP 不可用仍明確跳過。


`NeverDKvmRunTests` 無需 `/dev/kvm` 即可驗證 `KvmRunControl` 借用的傳輸回呼。`StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` 檢查準備、讀取和被攔截的主機進入使用同一執行緒，且中斷重試期間只準備一次。其他案例涵蓋準備失敗而不進入、讀取失敗、準備期間停止及活動進入被取消；隨後重新執行，確認不會重用舊回呼。驗證仍應包含真實取消、RAM 回滾、例外和原有驅動程式測試。 `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` 驗證同一期限內的多次進入重用該執行緒，每輪傳輸只執行一次，並保持先前狀態封包不變。

`KvmHandoffPolicy` 將每次輪詢等待限制為 8 μs，連續兩次未命中後改用阻塞等待，並在 256 次交接後重試。呼叫執行緒和工作執行緒分別自適應；呼叫執行緒同時遵守原始期限與停止標記。原子就緒標記只提示排程：資料包、回呼生命週期和取消確認仍由互斥鎖管理。`NeverDKvmRunTests` 檢查無效輪詢的上限、恢復、對端延遲變化及資料包重用前的取消確認。

KVM x64/ARM64 透過 `KvmRunControl` 在同一專用 vCPU 工作執行緒準備狀態、進入 `KVM_RUN` 並讀取狀態。`EINTR` 重試僅準備一次；取消進入或讀取失敗不能發布。`KvmAArch64Machine.cpp` 在該執行緒執行位址轉換維護與完整純量、向量傳遞，共用一次單步期限。呼叫執行緒僅在確認完成後提交；ISA 解碼、RAM 交易、OS 策略和觀察器仍屬於呼叫執行緒。ARM64 原生執行仍缺少實機證據。

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` 在主機修改通用暫存器、首尾 XMM 暫存器、MXCSR 和 x87 控制字後，驗證連續執行及真實 CPU 寫入。停止進入後的實際 `FXSAVE64` 位元組驗證全部實體 80 位元暫存器、TOP、標籤、操作碼和指標；重複除法例外也會使重用失效。這些機器邊界測試不向 checked 設定開放額外的 x87 指令。

`NeverDKvmStateTransferTests` 在真實 KVM 執行後注入暫存器或 XSAVE 讀取失敗，再以未變更的輸入重試。獨立的整數和封裝位元組結果證明失敗的讀取不會重用已前進的原生狀態。只有該測試程式包裝 `ioctl`；原生主機不可用時明確略過。

`NeverDKvmStateTransferTests` 也在實際 KVM 上涵蓋 `KVM_CAP_SYNC_REGS` 缺失、個別支援、組合支援及能力查詢失敗。`SynchronizedCapturesRemoveOnlySupportedReadIoctls` 統計實際讀取呼叫並核對連續單步後的完整 CPU 狀態；`CancelledWarmEntryRequiresFreshSpecialStateOnRetry` 要求取消後重新讀取特殊暫存器。擷取失敗、整數/SIMD 重試、推測 RAM 回復與例外優先順序使用相同能力矩陣；原生覆蓋不可用時明確略過。

Checked ARM64 使用統一的完整狀態提交邊界。`Registers.def` 定義 39 個純量欄位及 32 個 128 位元向量暫存器；`captureAArch64State` 暫存所有讀取、套用宣告位寬與 NZCV 正規化，最後一次提交。Unicorn、KVM、WHP 和 HVF 傳遞相同清單，包括 TPIDR_EL0、TPIDRRO_EL0、TPIDR_EL1、FPCR 和 FPSR。原生介面透過 CPACR_EL1 啟用 FP/SIMD。任何純量或向量讀取失敗、進入取消，皆保留完整呼叫方狀態。

ARM64 KVM/WHP/HVF 初始化執行私有 `AArch64MachineProbe.def` 程式：NOP、向正無窮捨入的 FP32 加法及雙通道 SIMD 加法。每步比較全部 39 個純量欄位與 32 個向量，包括 TLS、NZCV、目的暫存器高位清零及保留和累積的 FPCR/FPSR 狀態。自檢只使用特權級監控儲存，共享一個總截止時間。自檢僅證明有界初始化。Linux ARM64 KVM 與 Windows ARM64 WHP 的工作負載驗證仍待完成；macOS 原生結果記錄於 [HVF 指南](macos-hvf.md)。 程式亦包含金鑰停用時的 A/B 返回位址簽署與驗證，以及非防護頁面的四種 BTI 指令。

自檢還執行兩次 `MRS CTR_EL0`，以及 `DC CVAU`、`DSB ISH`、`IC IVAU` 和 `ISB`，核對快取幾何資訊穩定及完整狀態。Checked EL0/EL1 接納原始指令、所有具名基線 DSB 選項及 ISB SY。CTR 來自選定虛擬 CPU，不同傳輸可以不同。快取目標必須在目前權限下指向可讀普通 RAM，允許非對齊位址和別名；其他目標明確報未支援。維護操作不產生資料讀寫觀察事件。投影保證指令執行的一致性，不模擬私有快取內容或平行硬體 SMP。`NeverDAArch64CacheTests` 檢查完整狀態、唯讀頁尾、拒絕項、停止、上下文、預算，以及透過跨頁 RW/RX 別名更新 guest 程式碼；不可用的 KVM/WHP 主機明確跳過。

x64 KVM/WHP/HVF 原生初始化在私有 supervisor 頁面執行 `X64MachineProbe.def`。單一時限涵蓋 NOP、朝正無窮捨入的 FP32 加法、雙通道 SIMD 加法、FS/GS 載入及 CS/SS/CR8 讀取；每一步比較完整的純量、XMM、實體 x87 和控制狀態。x64 與 ARM64 自檢都必須取得實體記憶體的獨占執行租約。`MemoryProjection` 統一保存快取身分（ISA、位址空間、映射世代、權限及監控變體）和各 ISA 已提交的頁表根歷史。建構器在改寫私有位元組前使快取失效；失敗的重建不能重用部分寫入的頁表，呼叫者也不能傳入過期頁表根。自檢僅證明有界初始化。Linux ARM64 KVM 與 Windows ARM64 WHP 的工作負載驗證仍待完成；macOS 原生結果記錄於 [HVF 指南](macos-hvf.md)。

共用 XSAVE 解碼器區分標準格式與壓縮格式的 SSE 初始狀態。XSTATE_BV[1] 清零時，兩種格式都初始化 XMM 暫存器；標準格式仍讀取並驗證 MXCSR，壓縮格式才初始化 MXCSR。`X64XsaveCases.def` 提供獨立的資料配置和原創主機 XRSTOR 程式。`X64XsaveTests.cpp` 檢查拒絕狀態的原子性，並以真實主機執行對照兩種格式，同時保留呼叫端 FP/SSE 狀態。主機架構或所需指令功能不可用時，對照測試明確略過。

`X64FPState.def` 宣告壓縮 AVX、AVX-512、CET_U/CET_S 和 AMX 傳輸配置，包括元件的 64 位元組對齊。存在的擴充資料必須符合架構的全零初始狀態；缺席元件資料與對齊填補不定義狀態。偏移由配置位元決定，未知配置、非初始資料或錯誤長度會在發布前失敗。`CompactedOffsetsFollowLayoutRatherThanPresentBits`、`WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`、`InitialCETComponentsDoNotHideFPState` 和 `InitialWideComponentsDoNotHideFPState` 涵蓋 872 及 10752 位元組 WHP 封包。這項傳輸支援不准入上述擴充指令。

`WhpXsaveRegisters.def` 使用具名 x87/SSE 控制暫存器補充完整 XSAVE 資料封包。最後操作碼及指令/資料位址會明確寫入並從主機讀回；可補齊封包中的零值欄位，但非零中繼資料衝突或共用控制欄位不一致時，會在發布狀態前失敗。`NamedMetadataRestoresOmittedPacketFields` 驗證欄位缺失情境，並保留完整 FP 資料。

原生 `FOP/FIP/FDP` 遵循主機 x87 儲存、還原規則。沒有未遮罩的待處理例外時，AMD 可能清零這些欄位；快照保留實際觀測值。`X64MachineProbe.def` 與精確 NOP/上下文測試使用一致的待處理例外狀態，確保每個欄位有效並逐項比對，不遮蔽差異。主機行程 FXRSTOR64/FXSAVE64 參考程式涵蓋兩種狀態；後端不會以輸入中繼資料取代主機結果。

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` 比對客體實際執行 FXSAVE64 寫入 RAM 的完整 FP/SSE 狀態與主機 XSAVE 讀回。兩種 API 都測試直接安裝與客體內 FXRSTOR64，分別使用預設指標儲存特性及明確選擇的主機支援設定，以區分進入、客體執行和擷取邊界。測試不修補傳回值；不一致仍然失敗。 邊界矩陣亦涵蓋未遮罩的待處理 x87 例外，並記錄主機行程直接執行 FXRSTOR64/FXSAVE64 的參考結果和處理器廠商，以區分條件式指標儲存語義與 WHP 狀態傳輸行為。

共用的 `encodeX64XsaveState` / `decodeX64XsaveState` 編解碼層擁有標準及壓縮 FP/SSE 封包、實體 TOP 輪轉、缺失元件的初始狀態和原子驗證。WHP 使用完整 XSAVE API，優先選擇 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`，舊 XSAVE API 作為相容路徑。個別的舊 x87 暫存器介面不能取代完整封包。非初始擴充元件、格式錯誤的標頭、非法控制位元和截斷擷取明確失敗。WHP 映射錯誤保留 HRESULT、GPA 和大小供診斷。

`CheckedX64Instructions.def` 透過既有 CPU 後端准入 8/16/32/64 位元無號 `MUL` 和 `CBW/CWDE/CDQE/CWD/CDQ/CQO`。`NeverDX64IntegerTests` 使用獨立的 `X64IntegerCases.def` 編碼和預期值，在兩種特權級驗證部分暫存器保留、32 位元零擴展、乘積高低兩部分、已定義的 CF/OF 結果及符號擴展不改變旗標。一般 RAM 乘法保留完整存取範圍的權限檢查和讀取觀察回呼；故障或觀察回呼停止會保留隱式輸出暫存器及 PC。裝置運算元仍不支援。這些案例也在 checked Unicorn 上執行；不可用的原生後端明確略過。

`X64BitInstructions.def` 支援 16/32/64 位元暫存器及一般 RAM 的 `BT/BTS/BTR/BTC`。暫存器位元索引依運算元寬度解讀為有號數並選取完整資料字；立即數索引限制於基底位址的資料字內。位址寬度截斷先於 FS/GS 基底位址相加。CF 與寫入值由處理器提供；`RAMTransaction` 在觀察回呼接受前保留私有執行結果。完整範圍權限檢查涵蓋獨立頁面配置與別名。停止、回呼失敗或頁面權限不足均保留原始 CPU 與 RAM。LOCK 僅支援自然對齊的記憶體修改形式；MMIO 與硬體平行 SMP 仍不支援。`X64BitStringTests.cpp` 使用獨立編碼與 x64 本機實際執行對照，檢查負索引、寬度截斷、跨頁存取、取消及非法 LOCK 形式。參見 [Intel 指令參考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。

`X64StringInstructions.def` 統一管理一般 RAM 上 8/16/32/64 位元的 `MOVS/STOS/LODS`；`CLD/STD` 只改變方向旗標。每個 REP 元素在觀察回呼前驗證整個運算元，並於一個可恢復邊界提交。後續錯誤保留先前完成的元素；取消或回呼例外不改變目前元素。FS/GS 僅作用於來源位址，且在位址寬度截斷之後相加。AL/AX 載入保留高位元，EAX 載入零擴展。32 位元位址模式的零次 REP 要求計數高位元為零，MOVS/STOS 還要求參與的位址暫存器高位元為零，否則不同真實 CPU 實作會產生不同結果。MOVS/STOS/LODS 的 REPNE 形式與 STOS/LODS 裝置運算元仍不支援。`X64StringTransferTests.cpp` 用獨立的主機指令對照寬度、方向、重疊和零次數，並分別檢查權限、別名、回繞、錯誤與恢復。原創 WDK 資源驅動程式透過 `driver_resource_strings.def` 執行四種寬度的 STOS/LODS。

`X64StringInstructions.def` 也統一管理一般 RAM 上 8/16/32/64 位元的 `CMPS/SCAS` 與 `REPE/REPNE`。每個元素在觀察回呼前驗證全部讀取運算元，更新六個算術旗標，並於首次符合終止條件時退出。資料錯誤會恢復本次連續 REP 執行開始時的旗標，同時保留已完成的指標與計數更新；公開介面恢復執行時，以已發布的 CPU 狀態重新開始。停止與觀察回呼例外不改變目前元素，提前終止也不會讀取下一個元素。FS/GS 僅影響 CMPS 來源位址；SCAS 保留累加器與未使用的來源暫存器。裝置運算元及有歧義的 32 位元零次數高位元狀態仍不支援。`X64StringComparisonTests.cpp` 以獨立主機指令對照旗標、方向、別名、回繞、權限與恢復，並透過 Linux x64 訊號測試讀取實際錯誤時的暫存器。原創 WDK 資源驅動程式透過 `driver_resource_strings.def` 執行四種寬度的兩類條件重複形式。參見 [Intel 指令參考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。 Linux 原生驗證涵蓋首元素執行前後發生的故障，並區分 Intel 還原入口 flags 與 Hyper-V 下 AMD EPYC 7763 保留最後一次比較 flags 的行為（[原生觀測](https://github.com/NeverSight/NeverD/actions/runs/37202522130)）；未知 CPU 廠商會明確失敗。所有後端的 checked 來賓仍統一還原入口 flags。

存活的 WHP CPU 共用一個原生分割區；最終關閉與重新建立由同一登錄表鎖序列化。`WhpResourceCache.h` 重用協作式 VP 0；切換邏輯 CPU 前先銷毀該 VP 並撤銷其映射。平行 CPU 保留獨立 VP 與私有 GPA 區間。暫存器、XSAVE 與取消請求均指向各自的 VP。x64 保留主機預設 XSAVE 功能集，並透過 `WHvGetPartitionProperty` 驗證實際分割區設定。預設排程仍為協作式。

`NeverDX64FPTests` 檢查全部 79 個啟動狀態損壞位置，並在原生傳輸上執行獨立組譯的 `X64ProbeCases.def` 指令，驗證單一時限及客體 RAM 不變。`NeverDProjectionCacheTests` 涵蓋呼叫者切換、ISA 順序、頁表根歷史、權限/監控變體、映射世代、位址空間身分及失敗重建。`NeverDRunControlTests` 的 `WhpXsaveTests.cpp` 檢查新舊 API 封包、所有 TOP、大小邊界及失敗時狀態不變；記憶體協定測試不能取代 WHP 原生證據。不可用的原生傳輸明確略過。

XSAVE 驗證診斷區分長度查詢、本機資料準備和擷取資料解碼，並保留 API 名稱、傳回位元組數、容量及有限的標頭/控制欄位；獨立預期位於 `WhpHostFailureCases.def`，不輸出客體暫存器內容。`InvalidInputReportsPreparationWithoutHostMutation` 也驗證無效輸入不會呼叫主機或修改其資料。共用 ISA 編解碼器仍是唯一驗證入口。

WHP 在能力查詢、分割區/虛擬 CPU 初始化、暫存器/XSAVE 傳輸及執行中的主機呼叫失敗，均保留 HRESULT 和 `WhpProtocol.def` 中宣告的 API 名稱；能力查詢失敗仍傳回具型別的不可用結果。 `WhpHostFailureCases.def` 提供獨立錯誤預期，涵蓋與取消同時發生的主機失敗，以及新版/舊版 XSAVE 查詢、安裝和擷取失敗。 Windows 專項 CI 要求 210 項原生案例通過：16 項映射、2 項啟動、10 項 FP/上下文、7 項共用 CPU、8 項整數案例，以及 `NativeInstallRetainsFPStateBeforeAnyGuestExecution` 的兩種 API 變體。後兩項在執行客體程式碼前比對完整 FP/SSE 與獨立讀取的中繼資料。 缺少註冊、略過、停用或未執行都會使原生證據稽核失敗。 新增的 26 項檢查涵蓋 `X64BitStringTests.cpp` 在兩種特權層級下的全部案例。 Windows PE64 要求 67 項 WHP 程序案例與十一項獨立原生 Windows 對照案例。

`NeverDMemoryLifecycleTests` 獨立於 Unicorn 建置，也涵蓋僅啟用原生後端的組態。停用 Unicorn 時，專用的軟體投影/裝置案例明確略過；符合主機的共用 CPU 案例仍會註冊。`WhpMemoryTests.cpp` 使用 `WhpMemoryCases.def` 中的 16 個案例隔離原生記憶體 API：單頁/投影大小的後備記憶體、共用/獨立配置、未觸頁/已駐留位元組，以及存在/不存在第一個虛擬處理器。每個案例保留兩個存活的邏輯擁有者，反覆切換其映射分割區，銷毀非作用中擁有者，並驗證剩餘映射無需重建即可繼續使用。實際映射錯誤保留 HRESULT 並使測試失敗；這是記憶體 API 證據，不是指令執行證明。

`X64MachineProbe.def` 的啟動診斷列出失敗指令，以及所有不一致的純量、TLS、特權級、x87 控制欄位、實體 FP 通道和 XMM 字，並保留預期值及觀測值。`DiagnosticIdentifiesStepFieldAndBothValues` 使用獨立的預期訊息驗證。狀態比較仍要求完全一致；診斷用來區分傳輸遺失與指令執行問題，失敗的原生探針仍判為失敗。

`WhpResourceTests.cpp` 涵蓋快取重用、替換前銷毀、失敗復原及截止時間/停止競爭。`LogicalCPUSwitchingRestoresPhysicalFPAndTLS` 在兩種權限模式下交替執行兩個存活機器，檢查獨立的實體 x87/XMM 和 FS/GS 狀態，並在銷毀同伴後恢復剩餘機器。Windows CI 要求兩種權限的 WHP 案例都執行通過。

`NEVERD_ENABLE_SEMANTIC_TESTS` 預設為 `ON`，控制 `unittests/semantic` 中的測試組及其彙總執行目標。建置不依賴 Unicorn 的原生 CPU 測試時，保留 `BUILD_TESTING=ON`，同時設定 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` 和 `NEVERD_EMULATION_BACKEND_UNICORN=OFF`。原生 KVM/WHP 測試仍可建置，包括具備對應 SDK 標頭的 Windows ARM64/MSVC 組態。在 Windows ARM64 上啟用 Unicorn 仍需 ARM64 LLVM-MinGW 工具鏈。這項建置解耦不等於 ARM64 原生執行驗證。

僅原生 CPU 的 CI 檢出初始化固定版本的 Capstone 原始碼，並使用經驗證的預建 LLVM 套件。設定 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` 且停用 Unicorn 後端後，CPU 測試目標的設定、建置和連結均不需要 Unicorn 原始碼，也不依賴簽章庫及外部語料。預設 CI 仍啟用完整語意測試組。

`ci.yml` 的手動模式 `native_cpu_only` 透過 `native_cpu_backend=whp` 選擇 Windows x64（預設），或透過 `native_cpu_backend=kvm` 選擇 Ubuntu x64。`NativeCPUTests.def` 共用 CPU／行程驗收要求，分別宣告後端專屬目標和案例。`run_native_cpu_ci.py --require-whp` 或 `--require-kvm` 驗證宿主，先建置全部目標再執行 CTest，並保留清單、JUnit、日誌和結果分類。即使 CTest 成功結束，缺少或略過必測案例仍會失敗。CI 停用 Unicorn；`--with-drivers` 要求所選後端執行相同的原址／重定位驅動程式範例。編譯和建立探測不能證明來賓執行或 ARM64 驗收。 Ubuntu 設定使用上游簽署的 Clang/LLD 21 套件；Clang 18/19 的 CR8 宣告與固定版本的 WDK 標頭衝突。 Linux 原生驗收使用 CMake 4.2.3。`NeverDNativeDriverTests` 明確設定 `NO_PRETTY_VALUES`，讓 CTest 保留已宣告的案例名稱，不依賴參數診斷輸出。

原生 CI 在建置前探測 `sccache --zero-stats`。探測失敗時會清空 C 與 C++ 編譯器啟動器，保留原有編譯器設定和全部必測清單。設定或編譯失敗仍會使工作失敗。

KVM 驗收要求真實且不主動退出的 vCPU 取消，以及 `KvmStateTransferCases.def` 中 48 項狀態傳輸結果，包括 ioctl 擷取和選用能力查詢失敗。其他同步暫存器模式在宿主支援時執行，否則明確略過。穩定的參數名稱不依賴 ioctl 數值或元組格式。協定測試補充原生執行證據，不能取代它。

`native-host-probe.yml` 在 Linux 與 Windows x64/ARM64 託管 runner 上執行獨立的 `probe_native_host.py`。`NativeHostProbe.def` 宣告能力查詢、VM/vCPU 建立及清理證據的順序。報告保留原始碼/二進位雜湊、原生宿主 ISA 及每一步宿主狀態碼。`setup_ready` 只證明初始化成功，不執行客體指令。缺少 API/裝置能力記為 `unavailable`；建置、初始化、清理、逾時或證據格式錯誤會使工作失敗。ARM64 託管環境的可用性須逐次觀察，此探測不構成 ARM64 工作負載驗收。 兩個 Linux 工作流程均透過 `prepare_kvm_ci.py`，只授予目前託管 runner 帳戶既有 KVM 字元裝置的存取權限，並記錄裝置身分及權限；腳本拒絕本機與自管機器，不會建立缺少的裝置。

`windows-alignment-oracle.yml` 透過 `check_windows_alignment.py` 和 `WindowsAlignmentCases.def` 收集 72 項原創 x64 Windows 例外觀測：九種對齊 SSE 形式分別涵蓋七種未對齊位址/權限情境，以及一個已對齊但頁面無法存取的對照。它保留例外代碼、參數、故障 PC、儲存的上下文、原始輸出及原始碼/二進位雜湊，並驗證輸入與 RAM 未改變。這些觀測僅建立 OS 行為依據，不代表 KVM/WHP 執行驗收，也不新增 SEH 支援。

在 `native_cpu_only=true` 時，設定 `native_driver_tests=true` 可啟用不依賴 Unicorn 的 `NeverDNativeDriverTests`。設定前，`build_wdk_driver_fixtures.py` 驗證微軟官方 WDK/SDK 10.0.26100.6584 套件的完整 SHA-256，並從原始程式碼重建 48 個一般、CFG 或 DBG 驅動程式映像。`WDKDriverFixtures.def` 統一定義套件身分、編譯與連結參數及範例繫結。未修改的微軟檔案與授權保留在本機建置或快取目錄；CI 僅上傳建置中繼資料與記錄。清單記錄工具版本、命令、原始碼與標頭摘要及輸出映像摘要。

`NativeDriverTests.def` 要求 `DriverBuiltinImages.def` 與 `DriverBackendParityCases.def` 中全部 115 個負載產生 230 項 WHP 結果：27 個內建映像、48 個 WDK 映像及 40 個要求情境，各涵蓋原始與重定位位址。完整必測清單為 `5068 CPU + 230 WHP + 25 SEH + 77 scheduling + 30 wait sets + 12 driver UNPACK + 6 clock reads + 4 image memory checks = 5452`。30 項等待集合檢查包含十六項可攜模型測試及十四項原創原生驅動測試。`run_native_cpu_ci.py --with-drivers` 在停用 Unicorn 時保留精確清單與 JUnit 證據；必要範例遺失或略過會使此選用驗收失敗，一般建置仍可不提供外部範例。固定位址映像保留預期的重定位拒絕。ARM64 原生客體執行仍未驗證。

`InterruptionRetainsPhaseCauseDeadlineAndLease` 在兩條不同啟動指令前注入逾時、停止及兩者同時發生的中斷，檢查精確階段診斷、訊息自行持有的生命週期、錯誤類型和原因位元、步驟間不變的統一截止時間及記憶體占用釋放。既有真實傳輸失敗與狀態不符仍分別處理。原生 x64 啟動驗證預算為 `5 s`；一般客體截止時間及單步寬限不變。

`WhpResourcePolicy.def` 為 WHP x64 和 ARM64 的資源建立設定獨立的 `30 s` 期限，再執行 ISA 自我檢查。同步主機設定完成後，發布資源前仍檢查該期限。指令自我檢查和一般客體執行保留各自的限制。`WhpResourceTests.cpp` 檢查初始化中斷的類型與原因診斷、取消後資源清理、主機錯誤優先順序及一般執行期限不變。

`X64PopFlagsTests.cpp` 檢查兩種權限及 `driver-strict`：全部 256 種允許的旗標輸入與兩種初態、九種編碼、全部 64 個輸入位元、唯讀和可執行別名、跨頁錯誤與修復、觀察器中止及失敗、裝置堆疊拒絕和後續原生指令邊界。`X64PopFlagsOracle` 在 x64 主機獨立執行原始指令，核驗 CPL3/IOPL0 和精確堆疊消耗。`driver_resource_flags.def` 使原 WDK 資源驅動透過兩種運算元寬度設定、清除並還原旗標。測試保留完整整數、控制、x87、SSE 狀態，不代表支援客體 TF/NT/AC/ID 或已有 ARM64 原生執行證據。

`X64StatusFlagsTests.cpp` 檢查 `CLC/STC/CMC`、`LAHF/SAHF`、全部 256 個 AH 輸入及已接納的旗標組合、所有 REX 前綴、完整 CPU 狀態、記憶體不變性、觀察器停止或錯誤、儲存上下文和原生 ADC/儲存續行。無效 LOCK 編碼無副作用拒絕。獨立主機指令判據在驗證 CPUID 支援後檢查 24 組前綴。原有 WDK 資源驅動涵蓋全部五條指令，新增 22 項原生必測結果；不可用的主機或 ISA 組合明確跳過。 可攜式 Unicorn 設定執行相同的七項案例；相依項的直接測試涵蓋 16/32/64 位元 AH 與 LOCK 行為、明確指定的 REX 暫存器，以及長模式缺少特性時的拒絕。

`X64DoubleShiftTests.cpp` 涵蓋全部已接納的 imm8/CL 計數、重疊與擴充暫存器、已定義旗標、精確的跨頁 RAM 觀察、取消、權限/未映射/裝置故障、LOCK 與未定義計數拒絕、上下文及原生 ADC 續行。獨立主機判據檢查 5,184 次原始執行，12 個原有 WDK 驅動探針涵蓋暫存器與 RAM 形式。原生檢查新增 145 項必測結果。

`X64ScalarShiftTests.cpp` 檢查全部位元組計數、兩種進位輸入、零/全一及帶符號運算元、隱含單次形式、AH/SPL 與計數暫存器別名、完整 CPU 狀態、精確 RAM 範圍、觀察回呼回滾、故障及上下文續執行。獨立原生對照檢查 65,536 次執行。WDK 資源驅動加入 72 個原創探針。KVM/WHP 驗證關卡要求此指令族的 769 項結果通過。 只有遮罩後計數為零才保證保留全部旗標。非零計數的 `RCL/RCR` 完整進位環繞會保留運算元和 CF，但 OF 未定義；對照僅排除這個未定義位元。

`X64LoopTests.cpp` 涵蓋 21 種原創編碼、計數器環繞、4 GiB 以上的有號相對目標、完整 CPU/RAM 狀態保留、觀察回呼取消、跨獨立映射取指、目標取指錯誤與情境還原。指令位元組不完整時在執行前拒絕；目標取指失敗則保留已完成分支的計數器和 PC。WDK 資源驅動新增 12 個原創探針。KVM/WHP 在核心、使用者和驅動契約下必須通過本指令族的 379 項結果。 原始主機指令對照最多執行 1,008 個案例並回報次數。AMD 上採用 `66H` 的已跳躍分支會存取主機系統保留的低位址，因此這些形式在明確映射低位址的客體矩陣中執行。Intel、AMD 的目標寬度及 REX.W 優先級分別驗證。

`X64BranchTests.cpp` 檢查全部 16 種 Jcc 條件、相對 JMP、九組前綴、短/近形式、4 GiB 以上的有號相對目標、完整 CPU/RAM 狀態、觀察器停止與錯誤、跨頁解碼、目標取指錯誤和情境還原。獨立 Intel 宿主對照執行 9,792 條原始指令；AMD 低位址目標形式留在客體測試中。兩種解碼模型均測試完整和截斷位元組，原生探針也檢查失敗時不發布結果。四個原創 WDK 資源探針涵蓋驅動策略。KVM/WHP 驗收各增加 274 項必要結果。AMD 軟體模型涵蓋範圍不代表原生 AMD 或 ARM64 執行證據。

`X64StackTests.cpp` 以九類測試涵蓋 42 種編碼，檢查寬度、定址、完整狀態、觀察器順序、取消、權限、跨頁、實體別名、故障修復、裝置拒絕及情境還原。獨立宿主對照執行原始指令，六個 WDK 資源探針涵蓋驅動路徑。KVM/WHP 原生驗收各增加 1135 項必要結果。無法使用的後端在其原生強制驗收之外仍明確回報略過。

`X64FrameExitTests.cpp` 涵蓋 14 種 `LEAVE` 編碼、有效前綴順序、完整 RBP 定址、完整暫存器狀態、唯讀別名、跨頁框架錯誤與修復、權限、觀察器、無效位址、裝置拒絕及上下文重播。獨立主機對照執行 42 條原始指令，五個 WDK 資源探針驗證驅動程式執行。兩個原生驗收各增加 379 項必要結果。

`X64FrameEntryTests.cpp` 涵蓋 14 種編碼、巢狀與重疊、實體別名、僅寫入權限探測、跨頁故障與修復、觀察器取消、權限、前綴拒絕及上下文重播。獨立宿主對照執行 882 組成功指令，並在 Linux x64 上執行 84 組故障，逐位元組檢查堆疊與暫存器。兩項注入後端測試區分取消及失敗回復與架構故障提交。六個 WDK 資源探針執行原始驅動指令。原生驗收新增 508 項 KVM 與 507 項 WHP 必測結果。

`DriverSIMDSEHTests.cpp` 以四種受支援的處置及一次 x87 修改拒絕、兩種原生執行契約、一般/CFG WDK 映像及兩個載入位址執行八類原始 SSE 故障。十項後端專屬結果與三項純核心 SSE 記錄檢查均為必測。`driver_seh_simd.def` 統一定義樣例與模式；非同步展開表涵蓋故障輔助函式。微軟核心 10.0.26100.9549 提供獨立分類與還原依據：隔離執行了 107,744 組指令路徑分類及 8,192 組還原。這不代表已在完整 Windows 核心中執行驅動程式；ARM64 原生 KVM/WHP 仍未驗證。

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

`NeverDInstructionFetchTests` 透過 Unicorn、KVM 與 WHP，在核心態與使用者態執行 x64/ARM64 checked 程式。`InstructionFetchCases.def` 涵蓋運算元形式切換、相對分支、客體與主機經程式碼別名寫入、上下文還原、權限撤銷、獨立頁面儲存、頁尾預讀、無效或截斷編碼及遞迴執行拒絕。Windows 原生 CI 要求所有 x64 WHP 案例通過。無法使用的主機/ISA 組合明確略過；可攜式 ARM64 執行不構成原生 ARM64 支援證據。

`WhpStateTransferTests.cpp` 對兩代 XSAVE API 注入暫存器傳輸，檢查精確變更組、完整擷取、填補忽略、部分失敗、取消、例外優先順序與分區重建。`ContinuedStepsReuseCapturedRegistersAndFP` 統計省略的安裝；`PartialTransferFailuresPreserveStateAndForceFullRetry` 要求完整恢復。這些是協定檢查，而非原生執行證據；既有原生 FP、狀態轉換、驅動及 ring3 測試仍屬必要驗證。

`CancelledDirectRunPublishesACompleteBoundary` 驗證 direct 執行確認取消後發佈的完整狀態。`FailedDirectCapturePreservesStateAndForcesFullRetry` 要求取消期間暫存器、XSAVE 或中繼資料擷取失敗時保留呼叫方狀態，隨後完整重試；兩代 API 都不得發佈部分暫存器前綴。

`WhpStateTransferCases.def` 也涵蓋合併後 32 個暫存器讀取中每個部分前綴失敗，以及全部七個中繼資料欄位衝突。兩代 XSAVE API 都必須保留呼叫端狀態並在重試時完整還原。同一套測試核對每步只有一次暫存器讀取，並從該讀取補齊 XSAVE 省略的中繼資料。

`windows-pe64-v1` 支援有界 Windows x64/ARM64 主控台程序，包括 PEB/TEB、靜態與動態 TLS、`DllMain`、具名 Win32 API 和明確的無環 DLL 圖。客體模組支援依名稱／序號匯入程式碼與資料、DIR64 重定位、轉送匯出及真實載入器串列身分。`LoadLibraryA`／`LoadLibraryW`、`FreeLibrary` 和 `GetProcAddress` 使用設定的模組目錄。CRT／GUI、ARM64 以堆疊框架為基礎的使用者態 SEH、執行緒及通用 Windows 應用程式相容性仍待完成；原生 ARM64 KVM/WHP 證據仍缺失。

輸入總位元組與映像總範圍各受 `memory_limit` 限制，執行期映射也計入映像預算。準備階段共用 65,536 筆紀錄、64 MiB 中繼資料讀取、名稱長度及整體截止時間；阻塞主機 I/O 無硬即時保證。原創 EXE→DLL→DLL 樣例驗證重定位指標、序號呼叫、共享資料、API 指標身分、`MEM_IMAGE`、載入器串列及 EXE TLS 掛接／分離。`NeverDWindowsProcessTests` 包含直接原生 Windows 對照，`NeverDPEProgramExportsTests` 驗證畸形資料與預算，`NeverDProcessPublicTests` 驗證 C ABI/CLI 目錄一致性。不可用後端明確略過。

`WindowsProcess.ClockServicesUseConsistentUnitsAndPreserveLastError` 執行原始 x64/ARM64 PE 中的 `QueryPerformanceFrequency`、單調計數器、FILETIME、回繞 tick 計數和相對延遲呼叫。`WindowsProcess.UnmodeledDelaysStopWithoutClaimingCompletion` 驗證警覺、正數絕對時間及 INT64_MIN 間隔在完成前遭拒絕。`NativeWindowsOracleRunsTheSameExecutable` 也在 Windows 上直接執行成功的時鐘情境；兩項客體回歸均納入停用 Unicorn 的 KVM/WHP 必測清單。 原始樣例明確污染 `BOOLEAN` 參數暫存器未使用的高位元，並以明確的 64 位元型別定義常數，防止 Windows ABI 截斷紀元值與 INT64_MIN。

`ARM64 native backend build` 使用固定版本的 LLVM 原始碼，在 `ubuntu-24.04-arm`（KVM）和 `windows-11-arm`（WHP）上停用 Unicorn 並編譯 `NeverDEmulationNative`。`audit_native_backend_build.py` 核對每個宣告的原生原始檔、啟用的後端定義、編譯命令、ARM64 ELF/COFF 物件及雜湊。`probe_native_host.py` 記錄主機初始化能力與資源清理；能力不可用會明確記錄，初始化錯誤會使工作失敗。這些工作驗證編譯與主機初始化，尚不構成客體執行證據。 `NeverDCapstoneCompilerOptions.inc` 將限定詞診斷選項限定於 Clang 的 C 編譯；GCC 與 MSVC 保留各自的警告規則。 `native_arm64_only=true` 可單獨執行這些 ARM64 元件建構與初始化探測，不啟動完整 x64 CPU 驗收。 建構審計會先正規化原始碼與建構目錄，再比對路徑，包括 Windows 8.3 別名。 ARM64 元件工作會在建置稽核前明確啟用選定的 KVM 或 WHP 後端。

`WindowsTestExecution.def` 將 Unicorn ARM64 的 `WindowsExclusive` 對照設為 `RUN_SERIAL`。CTest 策略避免它與其他客體負載爭用資源，保留原有 60 s 客體截止時間及全部結果、暫存器、權限和原生摘要檢查。

`run_native_cpu_methods.py` 依據 `NativeMethodExecution.def` 驗證布林屬性 `RUN_SERIAL`，並在方法分組及跨次執行契約中保留該約束。各方法依序執行；子程序尚未結束時不會啟動下一項。未知屬性及遭修改的分片契約仍會使驗證失敗。

`WindowsProcessLifetime` 在相同 CPU 與執行預算下，依相依順序執行 DLL TLS 回呼及 `DllMain`，再執行 EXE TLS 與進入點。各模組具有獨立 TLS 索引與對齊區塊，從完成重定位和匯入繫結的映像複製，共用 64 KiB 空間。TLS 保留參數為零，啟動／程序結束的 `DllMain` 接收不透明非空值。明確程序結束依載入器串列的反向順序分離已完成初始化的 DLL，再執行 EXE TLS 結束回呼，即使 EXE 初始化尚未執行。啟動 `DllMain(FALSE)` 以 `0xc0000142` 結束，不發送分離通知。故障和預算耗盡不捏造清理。含客體 DLL 的 PE 進入點返回需要尚未支援的執行緒終止，因此明確停止。非零 `SizeOfZeroFill` 仍不支援；實際 TLS 範本中的零初始化位元組受支援。 無進入點 DLL 接收 TLS 掛接通知，但不接收程序分離通知。

`WindowsProcessExports` 為靜態匯入和 `GetProcAddress` 共用名稱／序號解析，涵蓋程式碼、資料、別名與鏈式轉送。 只有實際引用的啟動轉送會引入目錄模組及初始化相依；未使用的轉送不載入檔案。 匯出名稱區分大小寫；名稱缺失回傳 NULL／錯誤 127，直接查詢缺失序號（含空洞）回傳 NULL／錯誤 182，查詢參數為空指標回傳錯誤 87，成功保留 LastError。 未知模組控制代碼仍不支援。 有界 API 清單依精確提供者／名稱一次保留呼叫入口。 解析檢查每個映像的即時 PE 標頭與匯出中繼資料，拒絕修改或不可讀位元組，轉送鏈最多 64 項，並共用準備階段剩餘中繼資料額度及執行期限。 轉送到空洞時回傳目標映像基址並保留 LastError；轉送到零序號回傳錯誤 87。 回傳基址是資料位址，不授予映像標頭執行權限。 執行期轉送可載入設定目錄中的模組，並在回傳查詢結果前完成初始化。仍不支援即時改寫匯出表。

`WindowsProcessLoader` 從 `windows.modules` 載入 ASCII DLL 基底名稱，統一管理明確參考、共用相依與啟動模組保留。重複查詢轉送匯出不會增加額外參考。模組目錄槽位在重新載入時使用新的駐留世代。TLS 與 `DllMain` 在同一 CPU 上、暫停 API 的堆疊框架下方執行；還原暫存器保留客體記憶體寫入，並使用即時返回位址。動態附加／分離的保留指標為零。顯式載入期間的附加失敗在清理後回傳錯誤 1114，並保留已成功的獨立巢狀載入。卸載釋放映像映射與 TLS，重新載入恢復原始映像內容。模型之外對載入器串列或 TLS 指標的修改會明確失敗。失敗與重新載入皆不會重設檔案、映像及中繼資料工作額度。系統提供者以已映射 PE 的基址作為模組控制代碼。檔案系統搜尋、非 ASCII 路徑、`LoadLibraryEx` 旗標、循環匯入及正在初始化或卸載之同一模組的重入轉換仍不支援。

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 共用 PEB 程序參數中的即時客體環境區塊。名稱限 ASCII 且忽略大小寫，值為 UTF-16。修改前驗證輸入、容量及可寫記憶體。快照不受後續修改影響，釋放時回收客體記憶體。模型的環境區塊上限為 64 KiB；字串與展開操作有明確邊界並檢查工作負載期限。未知指標歸屬、格式錯誤的環境區塊、ANSI 字碼頁及展開緩衝區重疊仍不支援。`WindowsEnvironmentTests.cpp` 在可用後端比較原創 x64/ARM64 範例，CI 必須執行獨立的原生 Windows 對照。

`WindowsProcessHeap` 統一管理程序堆積的配置、`HeapReAlloc`、釋放和大小查詢。調整大小保留原有有效資料；`HEAP_ZERO_MEMORY` 清零新增位元組，`HEAP_REALLOC_IN_PLACE_ONLY` 禁止搬移。重新配置失敗時保留舊區塊，傳回 NULL 並設定 `ERROR_NOT_ENOUGH_MEMORY`（8），與原生觀測一致。獨立頁記憶體使縮減和釋放能歸還容量，分階段擴充及有界複製檢查工作負載期限。自訂堆積、例外產生旗標、未知歸屬及無法存取的複製或清零範圍均明確停止。`WindowsHeapTests.cpp` 涵蓋兩種 ISA、強制搬移、預算重用及失敗原子性；CI 也在原生 Windows 上執行同一原創 EXE。 PE 批次案例採用測試框架共用的 120 秒 CTest 外層時限，每個客體工作負載仍有獨立的有限預算。堆積範例為每個程序保留 20 秒，讓 WHP 完成全部資料驗證。

`WindowsSystemModules` 為兩種 ISA 建立有界的 `ntdll.dll`、`kernelbase.dll` 與 `kernel32.dll` PE64 模型映像。ASCII `GetModuleHandleA` / `GetModuleHandleW`、`LoadLibraryA` / `LoadLibraryW` 和 `GetProcAddress` 共用映射基址；PEB/LDR 與 `MEM_IMAGE` 描述相同映像。靜態匯入、名稱查詢與客體 DLL 轉送使用相同 API 跳板及匯出解析器。提供者固定駐留，不執行客體初始化回呼，普通客體 DLL 全部卸載後不會阻止進入點傳回。標頭或匯出中繼資料改變會停止查詢。未知系統匯出名稱與非零系統序號查詢明確停止；已建模名稱的大小寫不符及空名稱傳回錯誤 127，空指標查詢傳回 87。產生的位元組與位址屬於模型策略，不重建特定 Windows DLL 配置、原生序號或跨提供者別名。`WindowsSystemTests.cpp` 對照原始 x64/ARM64 EXE 與原生 Windows，並獨立觀察八次初始執行緒傳回。

`WindowsSectionFixture.inc` 檢查兩個獨立映像檢視、控制代碼重用、關閉後的讀取、寫入後的檢視隔離、解除映射及原駐留模組的保留。負例涵蓋命名空間、存取權限、固定位址與位移。`WindowsThreadFixture.inc` 分別檢查親和性與隱藏狀態、精確長度及含雜值的參數高位元。原始樣本亦納入 Windows 原生對照；缺少 `KnownDlls` 命名空間的 Wine 無法驗證 section 情境。

`WindowsProcessExceptions` 在同一 CPU 與程序預算內實作 `AddVectoredExceptionHandler`、`RemoveVectoredExceptionHandler` 和 `RaiseException`。有序處理器可註冊或移除處理器、觸發巢狀例外、呼叫已建模 API、載入 DLL 及結束程序。x64/ARM64 資料存取例外與 x64 整數除法例外可在驗證客體對 `CONTEXT` 的修改後恢復；一般暫存器、SIMD 與受支援的浮點狀態會保留。軟體例外經模型提供者中的實際返回指令繼續執行。模型最多保留 128 個註冊項、巢狀 16 層。非法處置值、遭修改的例外指標、不支援的內容欄位及超限皆明確失敗。ARM64 以堆疊框架為基礎的 SEH／展開、偵錯器派送及執行／防護頁例外仍不支援。`WindowsExceptionTests.cpp` 將原創 EXE／DLL 情境與原生 Windows 比較；原生 ARM64 KVM/WHP 證據仍待補齊。 軟體例外記錄帶有 `EXCEPTION_SOFTWARE_ORIGINATE`（`0x80`），與呼叫者傳入的不可繼續旗標分別處理；原始 Windows 執行檔精確核對軟體例外和硬體例外的旗標值。

`WindowsProcessContext` 保留每個派送框架的來源。已支援的 x64 資料存取和除法故障在 `CONTEXT.EFlags` 中呈現 RF（`0x10000`）；`RaiseException`（包括軟體拋出的存取違規碼）保留目前上下文。來源資訊貫穿 VEH/VCH 和 SEH 搜尋／展開。合法繼續執行時還原不含 RF 的邏輯 CPU 旗標；客戶修改 RF 會在發布狀態前被拒絕。此受限設定不模擬指令中斷點或客戶控制的 RF。`WindowsExceptionTests.cpp` 檢查儲存記錄、還原，以及拒絕時 CPU／RAM 不變。

Windows ring3 依獨立原生觀測，將 checked x64 的 `operand_alignment` 故障映射為 `STATUS_ACCESS_VIOLATION`，參數為 `[read, UINT64_MAX]`，儲存指令亦相同。原因由 CPU 層提供，Windows 不憑向量 13 猜測或重新解碼指令。`WindowsAlignmentProcessTests.cpp` 執行原始 PE 指令，涵蓋 72 種故障情境及 9 次位址修復重試（`72 + 9`），檢查 PC、RF、XMM 與 RAM。未分類或欄位不一致的故障仍會拒絕。程序與驅動程式故障報告保留可空的 `cause` 及十六進位 `error_code`，區分缺失與零。此派送適用於 checked x64 使用者態執行契約。 每次故障或修復重試後，範例都會匯出完整的 4096 位元組頁面；宿主核對全部 81 份快照及實際完成計數，來賓時限保持不變。 原生程序及初始執行緒觀測使用 `CREATE_DEFAULT_ERROR_MODE`：GoogleTest 會啟用可繼承的 `SEM_NOALIGNMENTFAULTEXCEPT` 旗標，使 Windows 自動修復正在測量的故障。因此原生驗證使用系統預設行為，避免測試框架策略干擾結果。

`AddVectoredContinueHandler` 與 `RemoveVectoredContinueHandler` 管理獨立的有序串列，與例外處理器共用最多保留 128 個註冊項的限制。向量例外處理器接受繼續執行後，繼續處理器讀取同一份可修改的例外記錄與 `CONTEXT`；最終內容驗證在這些回呼完成後進行，包含巢狀例外與 DLL 通知。兩類處理器的控制代碼不可交叉移除。`WindowsContinuationTests.cpp` 將順序、提早結束派送、增刪、內容修復、巢狀派送、載入器回呼及程序結束的原創 EXE 案例與原生 Windows 比較。已測 Windows x64 向量處理路徑允許在設定 `EXCEPTION_NONCONTINUABLE` 時繼續執行；這不代表以堆疊框架為基礎的 SEH 行為。原生 ARM64 執行仍未驗證。

`RtlCaptureContext` 已透過 `kernel32.dll` 和 `ntdll.dll` 支援 x64、ARM64。共用的 `WindowsProcessContext` 與 `IntegerABI` 保存呼叫者 PC/SP，不修改 CPU 狀態或 LastError。原生 Windows 觀察確認 x64 旗標為 `0x10000f`，未涉及的 home／除錯／向量儲存保持原樣，x87 位址欄位保留傳統的低 32 位元；ARM64 從 LR 保存 PC，並清零記錄中的 X0/LR。暫存器、SIMD 與浮點控制來自客體；x64 選擇子和 MXCSR 能力遮罩遵循設定的客體 CPU。無效、未對齊或部分無法存取的目標記錄在寫入前明確失敗。`WindowsContextTests.cpp` 涵蓋靜態匯入、提供者查詢、VEH 回呼、跨頁輸出及失敗原子性。`scripts/check_windows_context.py` 在原生 Windows x64、ARM64 上執行原創程式，並另外驗證非空 x87 狀態。這些 ARM64 API 觀察不代表原生 KVM/WHP 執行驗證。內容還原、堆疊回溯和動態函式表仍待實作。 `WindowsProcessServices.def` 宣告精確的模組限制：模型在 `kernelbase.dll` 中查詢此符號時傳回 `ERROR_PROC_NOT_FOUND`（127），與原生觀察一致，不憑空新增匯出。 [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` 使用 `os/windows/exception/` 中共用的 `X64SEH`（`NeverDEmulationWindowsException`，無需啟用驅動環境），處理 x64 `__C_specific_handler` 和 UNWIND_INFO V1。VEH 搜尋結束後支援篩選器、finally 回呼、非區域處理器跳轉、巢狀／衝突展開與重定位 EXE/DLL 堆疊框架，保留非揮發 GPR/XMM 狀態。篩選器選擇繼續執行時，VCH 使用同一份 `CONTEXT`。`WindowsSEHTests.cpp` 將 23 個原創情境與原生 Windows 比較；KVM/WHP/Unicorn 共用這些語意。派送在程序預算內重新驗證映像世代、標頭、展開／範圍位元組、語言處理器程式碼區域及 IAT 繫結。中繼資料遭修改或保留的映像被卸載時明確失敗。ARM64 框架式 SEH、C++ EH、動態函式表、通用 RtlUnwind/NtContinue、跨載入器／VEH／VCH 回呼邊界展開仍不支援。

當記錄包含 `EXCEPTION_NONCONTINUABLE` 而 x64 篩選器傳回 `EXCEPTION_CONTINUE_EXECUTION` 時，系統使用新上下文派送 `STATUS_NONCONTINUABLE_EXCEPTION`（`0xc0000025`，旗標 `0x81`，關聯記錄指標為空）。先重新執行 VEH，再從保留的邏輯堆疊重新搜尋，在相同深度與執行預算內保留 finally 順序及 EXE/DLL 框架身分。23 個原生情境包含 21 個成功執行及兩個終止情境：即使還原原始 `CONTEXT`，VEH/VCH 接受繼續這個二次例外後，它仍未處理。模型將此結果回報為執行期失敗。軟體例外位址等於儲存的 PC；內部派送器位址及暫存器配置由模型定義。 [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

`WindowsDynamicTests.cpp` 使用原始 x64/ARM64 DLL 與 EXE，比對獨立原生 Windows 觀測，涵蓋參考計數、共用相依、巢狀載入、附加失敗清理、轉送查詢、程序退出、無入口 DLL 及重新載入時的 TLS 初始化。額外回歸拒絕遭修改的載入器中繼資料與失效程式碼指標，保持累計準備額度，並確保中斷 API 的結果仍未完成。Windows CI 強制執行原生對照與 WHP 案例；交叉編譯及 Unicorn ARM64 不代表原生 ARM64 執行驗證。

`GetProcAddress` 轉發鏈任一位置缺少程式庫皆傳回錯誤 127；顯式 `LoadLibrary` 載入目錄中缺少的模組傳回 126。原生對照與各可用後端均斷言全部 41 個已宣告載入情境；Windows 上每種 DLL 變體都重複 16 次驗證全部卸載後從入口返回。 `GetProcAddress` 轉發目標初始化失敗也在清理後回傳 127。行程分離回呼保留退出呼叫端的堆疊內容。

`WindowsExportTests.cpp` 使用原始 x64/ARM64 DLL 與 EXE，驗證轉送的程式碼／資料／序號呼叫、別名、初始化查詢、重定位、大小寫敏感的缺失項、LastError、循環與非駐留目標、無效指標，以及成功查詢後的中繼資料修改。同一 EXE 有獨立原生 Windows 對照；原生 CI 強制執行 WHP 案例。C ABI／CLI 測試比對完整報告。原生 ARM64 硬體證據仍待補齊。 具有及不具有匯出表的 EXE 變體涵蓋兩種相依圖、PEB 串列順序、結束通知順序，以及名稱／序號／空指標的錯誤碼。

`WindowsLifetimeTests.cpp` 將固定通知序列與獨立原生 Windows 程序及 KVM/WHP/Unicorn 執行比對，涵蓋正常結束、進入點返回、兩個 DLL 初始化失敗、四處提早結束及無進入點 DLL。另驗證回呼故障、共用預算、重定位 TLS 欄位及 TLS 總容量。原生進入點返回探針保留初始執行緒控制代碼，重複64 次核對執行緒結束碼及精確執行緒／程序通知序列。觀察後終止其餘子程序執行緒，不將程序結束碼視為進入點返回值。

`NeverDUnpackTests`、`NeverDUnpackExecutionTests` 和 `NeverDUnpackPublicTests` 涵蓋加殼映像的還原；參見[脫殼](unpack.md)。`UnpackGeneratedTests.cpp` 用測試自己加殼的程式，在 x86-64 和 ARM64 上檢查入口規則。`X64ReturnPrefixTests.cpp` 在每種傳輸上檢查雙位元組近返回，並確認其它帶前綴的返回仍被拒絕。`WindowsDeferredTests.cpp` 檢查不透明入口與已停止行程的觀察；`ExecutionSessionTests.cpp` 檢查執行監視。 `DirectX64Tests.cpp` 另驗證部分頁監視、跨頁取指、恢復後僅執行一次、服務邊界、非法指令和逾時狀態。

`LiveHeapReferencesCannotPublishAnOrdinaryRecoveredImage`、`ExplicitSnapshotsKeepExternalHeapDependenciesVisible` 與 `ReleasedHeapStateDoesNotBlockRecovery` 在可用的逐指令與直接執行後端上比較獨立編譯的啟動及入口行為。`HeapReferencesInCapturedTLSCannotBeDiscarded` 涵蓋僅存在於 TLS 的相依性。公開介面測試要求拒絕時保留既有輸出檔案，並驗證 C API 與 CLI 的明確快照一致。位址匹配是保守證據，不是原生執行證明。

`DirectServiceBindingsRequireAnExplicitSnapshot` 涵蓋首次在擷取入口前後使用直接服務編號的情況；C API 與 CLI 也驗證拒絕時保留既有輸出。

`PrivateHeapDestructionReleasesOnlyOwnedBlocks` 驗證移動後的配置所有權、跨堆積拒絕、失效控制代碼及存活堆積容量的重用。

`UnpackLibraryTests.cpp` 在測試內替獨立 x64/ARM64 DLL 加殼，檢查相依順序、一般及產生的 TLS 回呼、附加失敗清理、輸入/宿主身分、自身檔案存取、匯出名稱/序號/資料/轉送器及無自身匯入。原生 Windows 透過獨立 EXE 載入原始和重建 DLL，並呼叫宣告的匯出；受檢與直接 WHP 案例均為必測。`CompletedGeneratedTLSCallsRequireTheAttachABI` 拒絕變更入口或參數；`GeneratedCallsNeedTheirReturnedStackAtTheContinuation` 拒絕錯誤返回堆疊。這些驗證涵蓋脫殼行為，不涉及去虛擬化。

`ExportObserver` 也觀察駐留來賓相依的可執行匯出；建模提供者仍透過服務分派觀察。輸入映像自身匯出被排除。模組變更會更新觀察點，每次修復仍須由即時匯出身分授權。發現紀錄不超過宣告的匯入上限。DLL 夾具同時要求修復系統 API 與來賓相依的跳板，並透過原生載入驗證不殘留模擬位址。

`WrappedEntriesRequireExplicitTransferEvidence` 涵蓋 DLL 包裝器透過更深的堆疊呼叫復原入口。預設結果仍為 `no_entry`；透過 `transfer` 選取該已觀察呼叫後，可重建可載入 DLL。僅憑深層呼叫無法區分入口與初始化器。

`WindowsDeferred.EarlierTLSCallbackMayGenerateALaterCallback` 要求獨立的 `.gentls` 區段具有 `IMAGE_SCN_CNT_UNINITIALIZED_DATA` 旗標，大小與宣告的緩衝區範圍完全一致，原始資料長度與指標皆為零。`WindowsDeferredCases.def` 統一定義儲存與組合語言，普通 `.data` 保持獨立。產生回呼與產生入口兩種情境皆保留 x64/ARM64 的嚴格拒絕及延後執行檢查。

`ExtendedRegistersLoadOrdinaryImportsAgain` 以受檢及直接 x64 執行驗證緊湊與帶填充的 R8-R15 導入載入。低暫存器案例涵蓋前置 REX 形狀位元組與僅含 CALL 的位址輔助常式；填充呼叫會跳過 CALL 後的任意位元組。`ImportCallHelpersCannotDiscardPersistentEffects` 要求持久副作用仍可觀察。`PERebuildTests.cpp` 拒絕缺少起點、結果證據及重疊起點，並保留六至八位元組視窗的精確 API 返回位址。

`OpaqueExportCallsAreRepairedBeforeTheExplicitStop` 還原純呼叫而不繞過未知 API。`ExportObservationIncludesTheOpaqueBoundary` 涵蓋靜態、動態與序號導出，保持執行和服務日誌不變。`OpaqueExportObservationPreservesAnUnreadableReturn` 要求缺少的返回資訊保持缺少。

`ExportIdentitySurvivesRebindingAndLateResolution` 改變不透明匯出的繫結順序，並在入口之後解析匯出。checked/direct x64 案例要求正確的 API 身分，並保留明確的 unsupported-service 停止。

Windows 虛擬記憶體新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及目前行程的 `FlushInstructionCache`。OS 層管理保留區域，`AddressSpace` 統一管理已認可頁面、權限和實體儲存。測試涵蓋動態程式碼改寫、存取錯誤和記憶體額度回收。

`WriteProcessMemory` 對不超過 4 KiB 的目前處理程序寫入，遵循 x64/ARM64 原生實測的已認可頁面語意。它保留各區域的權限、已複製前綴、位元組數和 LastError，包括 `ERROR_NOACCESS`、`ERROR_PARTIAL_COPY` 以及 RX 前綴寫入後傳回成功的情況。`WindowsMemoryWriteTests.cpp` 檢查全部 25 種權限組合；`check_windows_memory_write.py` 在原生 Windows CI 上驗證同一份原創可執行檔。未認可的目標區域仍明確不受支援。

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

外部寫入分離回歸涵蓋精確及不足預算、部分字、大小端、未觸及記憶體的身分、完整仿射槽邊界、晚到前驅覆寫與預設失效。公開 C API／CLI 檢查 v6 版面相容性和無效域。HighC、LLVMC 輸出均於 O0／O2 檢查回傳值、記憶體、堆疊與保留狀態；這些測試不等於原生等價證書。

有限分派迴歸涵蓋暫存器與框架內階段、兩種位元組序、確實可達的非法分支、後到前驅、耗盡的內層條件及相鄰探索預算。目標次數測試涵蓋算術關聯、巢狀迴圈、解碼模式、循序落入計數、總工作上限及舊旗標優先權。CLI 在 O0/O2 下執行兩種 C 路徑與原始碼 ABI；C/Python v8 測試檢查配置、非法欄位及未來尾部忽略規則。 迴歸亦驗證：大型無關有限選擇器之後的原生條件仍有可用探索預算，且最後一次允許的細化優先用於已提出的產生者候選。

`NeverDLLVMCPhiTests` 在 O0/O2 下執行獨立及交叉相依的迴圈更新，涵蓋零次迴圈、迭代邊界和隨機全位寬初值。可讀性斷言要求獨立更新不產生快照區域變數，複合交換只保留必要快照。既有分支、switch、移動分支體及交換迴圈案例繼續驗證實際選取的邊與同時指定語意。

`LLVMCInternalExitRegions` 新增5個獨立測試，涵蓋兩種退出極性、零次迴圈、退出邊和回邊的平行交換、迴圈頭/內部退出的四種巢狀組合，以及迴圈頭/本體/回邊區塊的觀察順序。共用目標出口和提前繼續路徑驗證可執行回退；有效迴圈之後遇到不支援區域時，不得發布部分結構化結果。整個模組與單函式輸出須一致且不修改 LLVM。原始 LLVM 和產生的 C 在 O0/O2 下與獨立無號 oracle 比對，共294,912次呼叫，C 啟用未定義行為陷阱。

`NeverDLLVMCPhiTests` 也驗證共同迴圈出口的不同後繼 PHI 對、有序觀察呼叫、輸出記憶體及呼叫者 IR 不變。整模組與指定函式的 C 都在 O0/O2 下對照獨立參考實作執行。不同退出判斷與額外分支前驅涵蓋保守處理。

`NeverDLLVMCPhiTests` 使用獨立 O0/O2 oracle 檢查多回邊迴圈運算合併、觀察呼叫順序、修改記憶體的呼叫前之值快照、窄位寬回繞及符號擴展。涵蓋整個模組與單一函式輸出且呼叫端 IR 不變，以及輸入衝突、共用根、poison 標註、未定義運算元、變數位移、受限制 intrinsic、例外函式及預算不足時完整拒絕。只有所有輸入運算一致才可合併旋轉呼叫。

`NeverDLLVMCPhiTests` 也在 O0/O2 下執行結構化純量區域：區塊順序打亂的巢狀迴圈、菱形分支、零次迭代、窄整數回繞、迴圈頭觀測呼叫、PHI 交換、存活的外層變數、共用步進值及漏斗位移端點。測試檢查來源 IR 不變、合併後僅三個區域變數，以及多出口、不可約與過大圖形的可執行回退。這些案例為獨立合成；原始碼輸出本身不構成原生復原證明。

`NeverDLLVMCValueTests` 在 O0/O2 下，將具型別的純量迴圈 C 與直接獨立編譯的 LLVM 對照執行，並為產生的 C 啟用未定義行為陷阱。邊界值及確定性全位元寬度輸入涵蓋窄整數乘法、位移前回繞、擴寬乘法與右移、寬整數截斷成布林值、有號比較與擴展、優先順序、條件運算式、布林運算、不支援操作的回退及深運算式的實體化。測試也檢查呼叫端 IR 不變，以及冗餘轉型已移除。

`NeverDLLVMCPhiTests` 與 `NeverDLLVMCValueTests` 涵蓋位元組計數器遞增遞減回繞、合併後的退出值、迴圈外的內聯使用、存活外層變數及 PHI 快照。獨立 O0/O2 檢查啟用未定義行為陷阱，驗證複合加法、反向減法拒絕、窄乘法及布林遮罩。巢狀區域測試要求計數器在迴圈內宣告、結果變數保持獨立，且不修改來源 LLVM。 可執行命名回歸讓外部函式與首次產生的結果變數及計數器同名，驗證呼叫與觀察副作用都被保留。

`NeverDLLVMCValueTests` 檢查加減和位元運算中兩種單位元分支位置、未變的基底值、內聯舊值依賴、已具體化的條件快照、窄整數真假判斷、非單位元分支及共用選擇值。產生的 C 在 O0/O2 下與獨立編譯的 LLVM 對照執行，並啟用未定義行為陷阱。`NeverDLLVMCPhiTests` 也檢查平行舊值快照，以及必須留在共用作用域的分支初值。呼叫端 IR 保持不變。

`NeverDUnicornDecodeTests` 檢查 AVX-512/APX CPU 模型的 EVEX 暫存器保留位，以及 ROUND 記憶體異常優先級、狀態保留和恢復。獨立 Linux x64 主機程式確認了傳統編碼的對齊異常和純量/VEX 編碼的分頁異常。這些引擎測試不擴展 checked 指令准入，也不代表 APX 原生執行證據。

## ARM64 CPU 效能測量

使用 Release CPU 建置；明確指定 HVF 需要原生 ARM64 macOS，軟體對比需啟用 Unicorn。工具驗證每個結果，啟動單獨計時；雙 CPU 切換包含中間 API 呼叫與檢查，其餘執行負載不含設定與驗證。

```bash
cmake --build build-cpu --target neverd-cpu-bench --parallel 4
build-cpu/bin/neverd-cpu-bench --backend hvf --samples 7 --warmup 1
build-cpu/bin/neverd-cpu-bench --backend unicorn --samples 7 --warmup 1
```

重新建置前保存基準執行檔，使用 Python 3.11+ 交替測量。保留編譯設定、原始碼標籤、二進位摘要及所有樣本，測量期間不要並行編譯或測試。

```bash
python3 scripts/benchmark_cpu.py \
  --baseline /path/to/before --baseline-label BEFORE_COMMIT \
  --candidate /path/to/after --candidate-label AFTER_COMMIT \
  --pairs 15 --output /path/to/new-comparison.json
```

入口計數為獨立診斷，會增加開銷，包含啟動探針；計數執行的耗時不得混入效能結果。局部負載不代表完整 OS 或跨架構效能。

[重現方法](../testing.md#reproduce-checked-arm64-cpu-measurements) · [HVF](macos-hvf.md)

`NeverDLLVMPrivateFrameTests` 涵蓋重疊寫入、所有入口路徑及迴圈回邊、保留的數值位址、外部輸出別名、未初始化／有序／未知記憶體、中繼資料，以及精確／少一單位預算下的原子拒絕。獨立 O0/O2 oracle 啟用未定義行為陷阱，比較完整回傳值、外部物件及框架位元組。編譯涵蓋 x86-64、AArch64、大端 AArch64 與 ARM32，不表示已支援這些架構的原生恢復。修改共用位址／副作用工具時，同時重跑 `NeverDByteMemoryForwardingTests`。

```sh
cmake --build build-release --target NeverDLLVMPrivateFrameTests NeverDByteMemoryForwardingTests --parallel 4
build-release/bin/NeverDLLVMPrivateFrameTests
build-release/bin/NeverDByteMemoryForwardingTests
```

`NeverDByteCellScalarizationTests` 涵蓋重疊字、兩條入口路徑與兩條回邊、寬及非二次冪位元寬度存取、兩種位元組序、儲存值選擇與 poison 義務保留、部分 poison 覆寫、完整使用圖拒絕，以及跨物件的精確/不足預算。一般 Thin/Deep 管線必須消除殘留陣列。獨立 O0/O2 oracle 對 8,192 個輸入的三個版本比較全部 24 個輸出位元組、周圍哨兵及回傳值，共 49,152 次呼叫，啟用未定義行為陷阱。x86-64、AArch64、大端 AArch64 與 ARM32 編譯檢查和原生執行涵蓋範圍分別記錄。修改共用記憶體契約時，應連同位元組轉送與私有框架測試一起執行此目標。

```sh
cmake --build build-release --target NeverDByteCellScalarizationTests --parallel 4
build-release/bin/NeverDByteCellScalarizationTests
```

`AndroidMutexTests.cpp` 使用獨立 O0/O2、一般/APS2/RELR 夾具測試三種 mutex、多等待者、喚醒後重新競爭、遞迴最終釋放、errno、原始事件、失效記憶體、死結及累計指令限制。相同案例於 Unicorn 與可用的 KVM/WHP/HVF 執行；不可用後端明確略過，不宣稱 Android 實機或平行 SMP 等價。

`HighControlFlowSemantics.DeepStableContainersPreserveEveryReturnPath` 使用獨立直譯器檢查 48 層區塊、迴圈、switch 和例外本體的進入及略過路徑，並以寬鬆的執行時間上限捕捉重複遞迴遍歷。這是結構化 HighIR 覆蓋；全映像方法恢復仍須獨立完成清單與相依性檢查。

`SwiftOnceSources.EarlyReturnsKeepExactObjCOnceThunkProofs` 涵蓋 ARM64/x64 中合併或分離 retain 呼叫的 thunk。`EarlyOnceCopyReturnsRequireTheSameCompleteTail` 拒絕變更儲存、缺少或重新排序 retain、有序載入、變更返回值及外部入口。`IgnoredNestedReturnCopiesDoNotObserveOnceContext` 檢查 void 回呼的兩條可行退出路徑，並在投影後重新驗證原始碼控制流程。

`HighControlFlowSemantics.ReturnTailCopyKeepsTheOuterLabelOwner` 使用獨立解譯器，比較同一位址在巢狀區塊中再次出現時進入與略過該區塊的路徑。`ReturnTailCopyIncludesTheFirstChildOfItsLabel` 保留合法的父陳述式與第一個子陳述式共用位址的情況及其指派。這些針對性檢查不能取代完整方法與原生相依性比較。

`JumpTailCopyKeepsTheOuterLabelOwner` 檢查跳轉尾部的相同歸屬規則。

`EarlyStringGetterReturnsKeepOnlyInertOnceAnchors` 檢查提前返回與 once 呼叫之間的空指令錨點，並拒絕其間的呼叫或儲存。

`SwiftOnceSources.ObjCThunkRootsShareTheNestedCallbackProof` 檢查共用根節點證明，並在葉節點開始觀察上下文後拒絕舊計畫。

`SourceABI.SwiftPointActionRequiresTwoDoublesAndContext` / `ObjCCallHints.CoreGraphicsPointActionsKeepSwiftFloatingCarriers` 涵蓋兩種架構，並拒絕變更提供者、弱匯入、衝突儲存及過期 ABI 載體。`HighCSourceCalls.SwiftCoreGraphicsPointActionsKeepCoordinatesContextAndOrder` 在 O0/O2 下執行產生的 C，以獨立 Swift 載體驗證函式檢查座標位元模式（含帶符號零、次正規數與 NaN）、接收者身分、呼叫順序和保護值。這些檢查證明呼叫 ABI，不代表上層方法已完整恢復。

`NativeSourceHints.CGContextCGRectMethodKeepsOrdinaryAndSwiftContextInputs` 驗證完整方法樹與精確載體，包括私有成員及拒絕的簽章。`SwiftFieldReceiver.CGRectMethodSelfKeepsItsLogicalParameterIdentity` 與 `CGRectMethodRejectsChangedEntryAndReceiverParameter` 涵蓋管線與發布重播，拒絕 self 索引、入口或參數型別的變更。編譯器記錄涵蓋四種 macOS/Mac Catalyst 目標；此入口宣告仍僅支援 arm64。 `HighCSourceCalls.SwiftCGRectMethodKeepsContextReceiverAndAllCoordinateBits` 在 O0/O2 下將產生的 C 與獨立 Swift 純量載體參考實作對照，驗證四個座標的完整位元模式、不同的 context/self 指標、單次呼叫與儲存保護。

`NeverDLowInstructionBoundaryTests` 可獨立執行 LowIR 指令來源測試，無須建置聚合提升測試的全部夾具。`BackwardSharedReturnEpilogueKeepsReturnAndCallerFrame` 驗證對齊 ADD 與後索引 LDP 堆疊釋放，包括由呼叫端恢復連結暫存器的情形；原始 RET X30 與共用入口仍獨立保留。`BackwardSharedReturnEpilogueRejectsChangedReturnAndOwnership` 拒絕其他返回暫存器、BR X30、缺失或未對齊的釋放、窄恢復、內部入口、修正、可寫或歧義映射、可重定位輸入及其他格式。解碼共用尾部不能證明原生 ABI：缺失呼叫端儲存或配置仍會使既有框架證明失敗。

`NeverDOwnInteriorCallTests` 涵蓋 x86 與 x86-64 函式直接 call 自身 unwind 範圍內標籤的情況，分別在 Microsoft x64 `.pdata` 條目、System V x86-64 DWARF FDE 與 i386 DWARF FDE 下測試。只為壓入返回位址而做的 call 會提升為壓堆疊加跳轉，x86-64 直線與迴圈兩種情形產生的 C 在 `-O0` 與 `-O2` 下以 AddressSanitizer 與未定義行為陷阱執行。返回時恰好彈出該 call 自身返回位址的目標仍是一般呼叫；切換堆疊之後的返回或低於入口堆疊指標的返回會被拒絕。從 i386 註冊鏈還原的範圍不能界定函式本體，因此其中的 call 仍是 call。

`NeverDSysVCallContractTests` 以 QtXml 中 `QDomNode::save` 與 `QDomNode::isDocument` 的形態檢查 x86-64 System V 呼叫約定。摘要顯示會讀取某個引數暫存器的直接被呼叫者會收到呼叫者的值，包括原樣傳遞的傳入 `this`；虛擬呼叫會取得支配區塊載入 `RDI` 的物件；在某條路徑上不寫 `RAX` 就返回、在其他路徑上只傳遞被呼叫者結果的方法為 void；在比較鏈之前寫入 `AL` 的位元組在每條路徑上都是返回值。產生的程式在 `-O0` 與 `-O2` 下搭配 AddressSanitizer 與未定義行為陷阱執行。

`ObjCCallHints.CIImageAffineValueKeepsProviderAndPhysicalCopyCarrier` 檢查 CoreImage 提供方、CIImage 工廠、完整 48 位元組邏輯記錄與 x2 指標，拒絕缺失或錯誤的提供方、x86_64 及衝突宣告。`ObjCImageValueCopy.OriginalFrameAndCompleteBodyAuthorizePublication` 區分原始呼叫與沿用相同機器位址的結果賦值。`RejectsChangedCopyCallBodyAndCurrentImage` 拒絕 24 類憑據、參數、儲存、框架、中繼資料、匯入、重複呼叫與儲存 IR 的修改，包括同時一致地修改 MedIR 和 HighIR。`GeneratedCExecutesAgainstIndependentPhysicalCopyABI` 在 ARM64 上以 O0/O2 執行未修改的產生 C，對照獨立從編譯器觀察到的 x2 指標接收函式，檢查六個浮點位元模式、選擇器與接收者身分、單次求值、傳回物件、合法副本寫入、輸入不變性與邊界保護。其他主機略過此實體 ABI 執行測試。

`ObjCCallHints.CurrentMethodEncodingMustAgreeWithCachedDeclaration` 拒絕與目前非空方法編碼或選擇器不一致的快取 ABI；僅提供明確型別宣告的用戶端保留原有約定。

`DarwinIndirectRecordCalls.MatrixFrameEffectsRequireExactCurrentContract` 涵蓋目前矩陣/仿射契約及 22 種必須拒絕的契約竄改。`ObjCAffineImageValueCopy.CurrentProducerInitializesThePublishedCopy` 證明 SDK 結果進入 CoreImage 副本並通過獨立發布重建；`RejectsWrongProducerFrameAndSavedIR` 對每個產生函式檢查 12 種竄改，包括輸入寫入缺漏、結果越界、錯誤提供者/ABI 載體及重用已消耗的 Concat 輸入。`GeneratedCMatchesOriginalMachineAndSDKResults` 在 Apple ARM64 上以 O0/O2 執行未修改的產生 C 與原始 ARM64 機器字，並呼叫原生 CoreGraphics：每個函式 1000 組資料比對全部 48 個結果位元組、兩個輸入記錄、選擇子/接收者身分、單次呼叫、回傳物件、私有副本改寫及邊界保護值。其他主機略過此原生 SDK 執行測試。

`MatrixFrameEffectsRequireExactCurrentContract` 也涵蓋 CGRect 使用者及 22 種必須拒絕的竄改。`ObjCAffineImageValueCopy.CGRectInputUsesTheSameCurrentFrameOwner` 驗證「旋轉 → CGRect 借用 → 旋轉重新初始化 → CoreImage 發布」；`CGRectBorrowRejectsExpiredInputsAndChangedABI` 拒絕初始化、輸入範圍、匯入及載體的八種修改。`GeneratedCMatchesOriginalMachineAndSDKResults` 也以 O0/O2、1000 組資料，將此完整順序的產生 C 與原始機器字、原生 SDK 比對，檢查儲存的角度、全部 48 個最終位元組、物件及保護值。

`FrameMetadataAccessorUsesCurrentCatalogAndABI` 檢查共用中繼資料宣告、兩個回應載體和目前框架見證的發布。`FrameMetadataAccessorRejectsChangedImportAndBytes` 拒絕弱匯入、provider/名稱/addend 變更、私有位址請求、部分 spill、錯誤重新載入及原始呼叫變更。原始 ARM64 與產生 C 的見證 oracle 也會實際呼叫 Foundation URL 中繼資料存取器：兩個分支均在 O0/O2 下執行 2048 組，檢查動態見證選擇、完整輸出位元組、輸入保持、呼叫次數和保護字。這些檢查不證明動態堆疊配置或見證記憶體效應。

`AArch64ExclusiveTests.cpp` 涵蓋標量與成對寬度、acquire/release 形式、暫存器重疊、別名、對齊及權限錯誤、快照、觀察回呼取消或失敗，以及雙 CPU 競爭。`RAMReservationTests.cpp` 涵蓋相同值寫入、ABA、配置重用、回復，以及 KVM/Unicorn 字串和 `ENTER` 寫入的干擾。原始 ARM64 Windows 行程範例執行獨佔迴圈；`scripts/check_aarch64_exclusives.py` 在 Windows ARM64 CI 執行原始指令並保存對齊例外記錄。原生指令證據不代表已驗證 ARM64 KVM/WHP 後端執行；不可用設定仍明確列為略過。 相同的獨佔指令案例也涵蓋軟體 Unicorn、跨契約干擾、同值及 ABA 寫入、同次執行中的可執行別名，以及 `DC ZVA` 寫入與觀察回呼取消。
 `windows-alignment-oracle.yml` 也執行 ARM64 探針：1,320 筆觀察記錄涵蓋所有未對齊偏移、四種讀寫序列，以及可寫、唯讀、不可存取和跨頁記憶體。探針保留完整位寬的暫存器測試值，並記錄錯誤前已提交的部分寫入。 `WindowsExclusiveProcessTests.cpp` 將 1,320 筆原始 Windows ARM64 觀察結果與 `WindowsExclusiveNative.def` 的原生摘要核對，保留暫存器值、例外中繼資料及 RAM 效果，只正規化程式碼與資料的放置位址。

`AArch64AtomicTests.cpp` 涵蓋 168 種獨立組譯的 LSE 編碼、暫存器別名、有符號比較、權限、取消、實體保留狀態與 NZCV 傳輸。`scripts/check_aarch64_atomics.py` 收集 1,100 筆原始 Windows ARM64 紀錄，包含完整運算結果、例外上下文與 RAM 範圍；解析測試拒絕缺失或不一致的證據。原生 KVM/WHP 執行仍需單獨驗證。 checked 處理程序回歸由 `WindowsAtomicProcessTests.cpp` 執行，完整紀錄摘要保存在 `WindowsAtomicResults.def`。

結構上已為常數的原生目標直接使用既有的可達性檢查排程。符號單目標只有在窮盡列舉後才重用傳入述詞。迴歸在直線執行的查詢預算內驗證 128 次常數跳轉，並按每次轉移兩次列舉查詢的預算驗證 32 次計算目標跳轉，保留未約束的位址高位元和分支域。完整狀態結果遭修改、缺少對齊約束、目標數量上限為零或查詢與指令預算不足時必須拒絕。多目標和未完成列舉的既有拒絕檢查仍然必要。

只有完成 UNSAT 證明、排除另一條邊後，原生分支才在目前的邊保留傳入域。測試在 512 個求解閘內驗證兩個方向各 32 次條件跳轉，並檢查精確與少一次的查詢預算及求解閘耗盡。修改或移除對齊條件、反轉比較及修改終態都必須拒絕；任意未定義控制和兩條邊皆可達的既有測試仍然必要。

位元展開快取與遍歷儲存僅記錄實際到達的運算式節點和變數。`NeverDSolverTests` 檢查稀疏的高位編號、增量斷言之間的上下文增長、快取位元重用、模型擷取和假設切換。無關的寬運算式不會被編碼；實際到達的寬度違規、格式錯誤的根節點和求解閘預算耗盡仍必須拒絕。

`SourceFrameAnalysis.CallStorage*` 涵蓋精確呼叫、到達定義、初始化、填充、逃逸、邊界和循環，不授予來源發布權限。`ObjCFrameBlockBorrows.*` 涵蓋描述符限定的同步借用，以及 19 種匯入、標頭、ABI 和機器指令修改。測試保留未證明的填充位元組並拒絕未初始化的所有權欄位；block 建構、擷取讀取及回呼相依閉包仍各自接受發布驗證。

block/副本發布測試亦涵蓋兩個獨立的 48 位元組範圍、描述符重疊、回呼本體變更、過期機器指令與 IR、脫離本體的呼叫位置，以及精確投影順序。`MixedWidthFrameCopiesMeetEveryInitializedByte` 與 `FrameCoverageCannotHideMissingBytesOrPointerJoins` 檢查兩種合併順序下的 8/16 位元組寫入、缺失位元組、可寫借用失效和部分覆寫後仍保留的指標身分。

迴圈關係測試涵蓋任意迭代次數下固定與變動的函式暫存值、兩側獨立偏移、配對方案、部分與未對齊範圍、兩種位元組順序及暫存值排名。原生組合僅在候選側保留新增儲存空間。缺少前綴、未宣告或未定義位元組、錯誤投影、遺漏賦值、程式行為變更及定義集合衝突均須拒絕認證。執行、查詢與觀察項預算在精確上限通過，少一單位失敗；推斷不能增加後續證明預算。存續期仍綁定摘要。這些檢查不建立一般原生 ABI。

原生迴圈關係測試在任意迭代次數下檢查延後條件分支收集及保留未審核拒絕邊界。手動與推斷方案都必須重新檢查完整入口域和歸納域；可達的錯誤分支、修改過的原生更新及查詢或指令預算耗盡都拒絕憑證。測試綁定變更的不可達邊界位元組，保留嚴格預設值與無效方案拒絕，檢查兩種見證及組合收集選項，並繼續拒絕靜態 API 與重疊指令。憑證語意綱要 17 綁定此准入；一般原生 ABI 和原始碼組合仍是獨立義務。

`ObjCSuperGetterSources` 涵蓋四載體 CGRect getter、十項發布變更拒絕案例，以及布林/CGRect 呼叫者共用機器碼的情況。O0、O2 執行驗證檢查精確回傳位元（含負零、無窮及 NaN 酬載）、接收者/類別身分和中繼資料呼叫之後的選擇子載入。Apple ARM64 同時執行原始編譯器 thunk 和產生的 C；其他平台使用本機紀錄 ABI 執行產生的 C。

`LowIRLoopInference` 涵蓋帶任意初始高位的 8、24、32 位計數器投影、遞增及遞減、暫存器、堆疊框架、函式暫存值和兩種位元組序。完整自證明通過，結果變化、停滯、窄位寬回繞及跳過相等退出條件均遭拒絕。操作、查詢、路徑、排名候選及拓寬預算在精確上限通過，少一單位失敗；最終證明的操作、查詢及觀察預算分別檢查。

`ObjCCallHints.SDKRecordData*` 檢查兩種外部記錄、每個 double 的偏移、兩種 Darwin 架構及提供者別名，並涵蓋匯入變更、弱連結、缺少程式庫、修正衝突、可寫儲存及不完整範圍。`python3 -m unittest scripts.tests.test_generate_darwin_record_data_declarations scripts.tests.test_generate_darwin_data_declarations` 檢查設定衝突、替代配置、無效大小/對齊、TLS 與各架構匯出。以固定 SDK、libclang、輸出路徑及 `--check` 執行 `generate_darwin_record_data_declarations.py` 可重現目錄；宣告檢查不證明原生間接結果已初始化，也不建立方法復原。

`LowIRLoopInference.ProjectedBounds*` 涵蓋計數器與邊界高位任意的窄位域等值退出，包含 8、24、32 位、三種儲存位置和兩種位元組序。完整證明拒絕邊界變化、停滯、跳過退出、回繞及被觀察的輸入高位元組變化。推斷與證明預算保持獨立，並檢查精確上限和少一單位的情況。

`LowIRLoopInference.LateCounter*` 涵蓋常數初始化後僅在泛化時才顯現的 8、24、32 位元計數器，包含暫存器、框架、函式暫存量、兩種位元組序及任意邊界高位元。等值退出通過完整證明；停滯、跳過退出、變化的邊界及可觀察高位元組被改動均被拒絕。推斷與最終證明分別檢查精確預算和少一單位預算。

`LowIRLoopInference.ProjectedComparisonBits*` 涵蓋迴圈頭處快取的窄位域等值條件，邊界高位元可任意，包含 8、24、32 位元計數器、三種儲存位置、兩種位元組序和常數或高位元填充初始化。完整證明拒絕快取值、可觀察高位元組、邊界被改動，以及停滯或跳過退出。推斷與最終證明分別檢查精確預算和少一單位預算。

`LowIRLoopInference.OrderedComparisonBits*` 涵蓋兩種布林編碼下快取的無符號大小比較退出，包含 8、24、32 位元計數器、三種儲存位置和兩種位元組序。完整證明拒絕不終止的更新、比較條件變化、邊界變化和可觀察高位元組被改動。推斷與證明分別檢查精確及少一單位預算。改變布林編碼的測試夾具在證明前重新綁定原始操作摘要。

`LowIRLoopInference.MutablePrefixBounds*` 涵蓋高位持續改變的 8、24、32 位元衍生邊界、三種計數器儲存、兩種位元組序及直接或快取退出條件，檢查不終止、移動的等值邊界、可觀察高位與快取變更，以及精確和少一單位預算。移動的無符號大小比較邊界可能在回繞時終止，另有完整證明回歸。

`LowIRLoopInference.SharedTemplatesFitIndependentOperationBudgets` 在 1,024 次推斷操作及 640 次獨立證明操作內證明巢狀的部分計數器。完全相同的純運算式與不可變前綴讀取僅在同一次切點重建內共用，保留運算元身分、輸出寬度及位置空間。測試觀測完整的計數器與界限字，拒絕被修改的前綴計算及少一次操作的預算。現有暫存器、框架、函式暫存值、位元組序及不終止案例仍須執行。

`LowIRLoopInference.CompletedEntailments*` 在兩種位元組序的終止及不終止框架迴圈之間檢查工作階段隔離、求解器與節點耗盡，以及獨立證明預算。可變邊界回歸在快取命中時驗證精確與少一單位的邏輯查詢預算；原生重複上下文證明檢查含域的重用。

`LowIRLoopInference.IncrementalEntailmentsKeepRollingBudgets` 檢查同一約束域內的編碼器重用。切換域會捨棄編碼器；累計邏輯閘容量耗盡時，僅以新編碼器重試一次，並額外計入一次查詢。兩種位元組順序皆保留對完整計數器和邊界字的觀測，並檢查精確及少一次的查詢預算、邏輯閘／寬度／搜尋超限拒絕，以及獨立的最終證明預算。

`LowIRLoopInference.RebuiltCounterLanes*` 涵蓋計數器其他位元從前綴值重建或獨立變化時，精確匹配的投影更新。暫存器、堆疊框架和函式暫存值案例包含兩種位元組順序、1／3／4 位元組計數器及直接／快取退出條件，並觀測完整計數器、邊界和標記。刪除可觀測的高位元標記、不終止更新、缺少退出守衛及少一單位的推斷／證明預算仍須拒絕。結構遞推匹配僅提出泛化和秩候選，仍必須通過完整轉移證明和最終證明。 固定的操作數與查詢數預算也涵蓋符號投影計數器，避免將常量前綴中的偶然匹配擴展為額外關係。

`LowIRLoopInference.ProjectedCounterCopies*` 涵蓋在遞增前經由帶獨立標籤的字複製計數器位段的巢狀迴圈。144 組完整狀態案例涵蓋暫存器、框架、函式暫存量、兩種位元組順序、1/3/4 位元組位段、直接或快取退出條件，以及整字複製對照。投影相等關係必須在儲存的到達狀態和每條傳入轉移上成立；高位保持獨立。轉移域蘊含證明可辨識由不同參數表示的加法遞推。捨棄高位標籤、無效更新、缺少保護，以及推斷或最終證明預算少一單位時仍會拒絕。

`LowIRLoopInference.TransferredCounters*` 涵蓋計數器在不同切點間搬移儲存位置、且其他迴圈可繞過各切點的情況。精確的符號單位步長轉移僅用於提出來源計數器保護條件，以及允許一個切點使用不同位置的備用排名；所有條件和排名仍須通過完整轉移證明。192 組完整狀態案例涵蓋暫存器、框架、函式暫存量、兩種位元組順序、1/3/4/8 位元組計數器、獨立標籤及位置不變的對照。結果或標籤變更、錯誤排名映射、不終止更新、缺少保護及推斷或最終證明預算不足均會被拒絕。計數器遍歷使用既有符號節點限額，可選圖選擇器的工作預算可維持為零。 案例同時涵蓋遞減至零及遞增至輸入上界。晚發現計數器時，待所有切點範本重建後再剪除界限及位元區段保護條件；其他擴展與獨立證明照常執行。

`LowIRLoopRefinement.GuardedCuts*` 與 `BinaryLowIRLoopRefinement.GuardedCuts*` 涵蓋同址切點、暫存器、框架及原生系統旗標、兩種位元組序、未匹配的有限與循環路徑、重疊及錯配拒絕、前綴泛化、未定義值見證、錯誤中繼資料、摘要與共用預算。獨立原生測試證明兩個 R10 上下文共用循環位址，並確認未稽核邊界檢查先於選擇條件。一般 ABI 認證仍是獨立工作。

`BinaryLowIRLoopInference.NativeSelectors*` 涵蓋兩個暫存器上下文、僅靠框架區分的上下文、三域合取、無法區分的範本、來源及原生迴圈本體變異，以及推導與證明各自的精確和少一預算。迴圈次數任意，不引入入口常數。

`NativeSelectorsGeneralize*` 驗證交替暫存器／框架階段的自動恢復與完整原生證明，包括高位元仍符號化的位元組遮罩。`NativeSelectorState*` 檢查錯誤排名／本體、遮罩外位元損壞、畸形賦值及工作量／中繼資料的精確和少一預算。明確有效的計畫另將正式建構器與變換前後的完整原生檢查組合，涵蓋重疊與不相交切點混合、原始側前綴來源保留、回退掃描計費及暫存位移溢位拒絕。這些明確計畫檢查與自動推導涵蓋範圍分別記錄。

`DarwinIndirectRecordCalls` 檢查目前 MakeScale 契約與 22 項匯入/ABI 變更拒絕案例，再透過共用按值副本證明使用完整的 48 位元組私有結果。未對齊、偏移、重疊或超出堆疊框架的結果範圍皆被拒絕。即使保留完整回傳 ABI，移除確定寫入效果也會被拒絕。

`SourceFrameAnalysis.IncomingResultAddressNeedsCompleteEntryIdentity` 拒絕十種入口、載體或寫入修改以及缺少的入口 ABI。`NativeSourceHints.IndirectResultTailCallRetainsExplicitOutputAddress` 重新提升直接尾呼叫，檢查明確輸出參數、六次寫入與發布門檻。

`NativeSourceHints.FourDoubleCallerDemandNeedsEveryUnchangedCarrier` 檢查四個低位通道、獨立高位寫入及九種宣告或控制修改。`FourDoubleReturnRequiresEveryComputedLowLane` 拒絕十二種結果不完整或契約失效的情況。`DarwinNativeRecordReturns.FourComputedDoublesExecuteAtO0AndO2` 在每個最佳化等級執行 2048 組輸入，與獨立算術判據比較全部 32 位元組結果。

`DarwinIndirectRecordCalls.AffineInvertSnapshotsItsCompleteAliasedInput` 在 O0、O2 各執行 2560 個同址、重疊或分離的輸入輸出配置，檢查輸入位元模式、單次呼叫、全部 48 位元組結果與完整含守衛儲存。它驗證實體複製與快照，不是原始機器碼或原生 SDK 執行。目前矩陣/仿射契約測試對每個契約保留 22 種拒絕修改。

`DarwinIndirectRecordCalls.AffineTranslatePreservesScalarBitsAndSnapshotsAliasedInput` 在 O0、O2 各執行 2560 組輸入，檢查兩個純量的位元模式、六個輸入欄位、單次呼叫、全部輸出位元組，以及同址、重疊和分離配置的受保護儲存空間。四種純量 ABI 修改和共用的 22 種匯入/ABI 修改均被拒絕。位元操作替身驗證實體參數和輸入快照，並非平移數學判據或原始機器碼執行。

`NativeFloatingReturnProof.HFAResultFieldsNeedTheExactCompleteDefinedCall` 涵蓋九個欄位選擇及十七種呼叫、載體、寬度、位移或 SSA 修改拒絕案例。`HFAFieldExtractionNeedsADominatingCall` 拒絕來自兄弟路徑的產生者。`NativeSourceHints.HFAFieldTypeRequiresCurrentCallAndFrameProofForPublication` 提升五條指令的 ARM64 呼叫者，依推斷的純量結果重新提升並檢查原始碼發佈門檻；錯誤提供者或缺少 LR/SP 恢復均被拒絕。這驗證原始碼型別與投影行為，並非原始機器碼主體執行。

CPU0 顯式搶占、虛擬時鐘語義與目前邊界見[驅動程式排程](driver-scheduling.md)。

`SwiftOnceSources.FoldedObjCGetterTailsExecuteOnceAndRetainsAtO0AndO2` 折疊實際 ARM64/x64 返回尾部，涵蓋合併或分離的 retain 呼叫、獨立或內聯謂詞，再以執行時替身於 O0/O2 執行輸出 C。檢查結果位元、一次初始化、呼叫順序及快取值變化。`FoldedObjCGetterTailsRevalidateCurrentStorageAndCalls` 在保留舊計畫時仍拒絕寬度、順序、內建操作、儲存、結果、呼叫及目前匯入修改。這是受控原始碼與執行時替身檢查，不是原始 WMF 機器碼或原生 Swift 執行時執行。

`SourceFrameAnalysis.CompleteOutput*` 涵蓋完整和較短前綴、各返回路徑合併、SDK 尾呼叫寫入、缺寫位元組、指標逸出及載體限制。呼叫端案例拒絕缺失或過短證明、錯位、超出堆疊框架邊界、覆寫保存暫存器、別名、後續寫入失效和存活不透明值重疊。`NativeSourceHints.CompleteNativeOutput*` 重播組譯產生的 ARM64 生產端和消費端，要求完整 SDK 輸入範圍，並拒絕過期程式碼、控制流程、ABI、稽核、提供端和呼叫實例證據。這些檢查驗證位元組初始化及原始碼門檻，不執行原始機器函式，也不證明原生邏輯返回值。

`MedCallingConvValueFlow.FPInputsFollowOnlyAuthenticatedCallPrefixes` / `NativeSourceHints.PreservedFPPrefixesRetainAllEntryInputsAfterSDKCalls`: 保留前綴回歸檢查三個有效讀取和十九個被拒絕的載體、前綴、呼叫歸屬及未消費值案例。組譯產生的 ARM64 呼叫端在 SDK 呼叫後保留全部四個入口 double 低位通道；重新提升檢查完整參數 ABI 和原始碼門檻。未修改的已獲准發布 WMF `0x36350` 方法還在 O0、O2 各通過 2048 例，與獨立原生 CoreGraphics 翻轉、平移、正規化、串接及套用運算式比較全部 32 位元組結果，並檢查接收者、選擇器和輸入保護區，涵蓋零、負數、無窮和 NaN 尺寸。未執行原始 WMF 機器函式。

`SourceABI.SwiftEntryCapturesAndReturnsTheErrorRegisterOnEveryPath` 檢查 ARM64/x64 入口擷取、兩條返回路徑，以及缺失或變更傳輸證明的拒絕。`NativeSwiftCallsKeepBothResultsAndPostCallErrorBranches` 保留呼叫結果、更新後的錯誤暫存器及後續條件判斷，並涵蓋私有名稱衝突與兩種輸出順序。產生的 C 在本機 Darwin 目標上以 O0/O2 執行，與獨立成功/失敗判據比較。`SwiftFunctionSymbols.RegularExpressionInitializerRetainsContextAndError` 拒絕別名、完整符號變更、非程式碼入口和不支援的映像格式。這些檢查不執行原始 WMF 建構函式，也不證明上層方法已完全恢復。

`NativeSourceHints.SwiftErrorDeclaration*` 在以編譯器宣告細化觀測所得的純量 ABI 後，重新提升實際組裝的 ARM64/x64 入口。目前稽核缺失或不完整時拒絕替換；選項、MedIR 或 HighIR 中的明確原始碼契約仍保留優先權。入口測試也會在轉成 HighIR 前拒絕缺失的 MedIR 錯誤輸出標記或錯誤的運算元寬度。

即使所有路徑都覆寫錯誤暫存器，入口捕獲仍作為存活值保留，包括 once 綁定後的最終原始碼清理。`NativeSourceHints.SwiftErrorCallsRequireCurrentDirectNativeProjection` 拒絕缺失、空值、被修改或未證明的被呼叫函式，以及目標變更、間接呼叫、不完整結果、缺失運算元和不相容效果。


`SwiftFunctionSymbols.RepeatedDeclarationsKeepEveryRecordField` 檢查完全相同的重複記錄，並拒絕名稱、大小、邊界來源或名稱來源變化。`NativeSourceHints.SwiftErrorCallResults*` 涵蓋 ARM64/x64 呼叫者的自動推斷，拒絕缺失的目前被呼叫函式、被修改的機器操作、過期稽核、不完整 ABI，以及缺失、變窄或來自其他值的結果提取。入口原始碼執行也涵蓋互相衝突的可選除錯宣告，確保繫結的呼叫慣例及錯誤、上下文角色保持有效。

Swift witness 產生器在 ARM64/x86-64 macOS 和 Mac Catalyst 上同時驗證 `CurrentValueSubject: Publisher` 與 `Range<Bound: Comparable>: RangeExpression`。`scripts.tests.test_generate_swift_witness_contracts` 拒絕泛型輸入、中繼資料回應型別或成員、原型、匯出提供方和完整資料流的變更。`ObjCSourceBindings.SwiftWitnessUndefRequiresExactDescriptorContract` 與 `SwiftWitnessUndefRejectsUnprovedInputAndABI` 在兩種架構上驗證兩個描述符，每個描述符與架構包含 33 種 runtime/匯入身分、弱或衝突儲存、ABI 和副作用變更。這些目錄不授予框架配置或借用契約。

`scripts.tests.test_generate_swift_data_declarations` 檢查完整 `String.Index` 描述符查詢，拒絕符號指標單元、配方位元組或長度、中繼資料與快取資料流、runtime ABI 的變更以及重複或缺失定義。`ObjCSourceBindings.SwiftRangeIndexDescriptorKeepsItsCompleteRecipe` 在兩種架構上檢查位於位移 3 的非首位描述符；`SwiftRangeIndexDescriptorRejectsStaleIdentityAndRecipe` 拒絕每種架構的 20 種變更，並複核既有位址提示與 helper 輸出。

`ObjCSourceBindings.PrivateFramePointerTailRequiresExactStoreOnEveryPath` 涵蓋 PHI 合併後的物件區域變數循環，以及同一循環中可達的私有堆疊框架值。物件循環保留精確指標溢寫證明；攜帶框架的循環、部分覆寫與未知框架洩漏會拒絕證明。

`ObjCCallHints.FoundationGenericNSRangeKeepsSixPointersAndTwoWords` 驗證兩種架構、匯出提供者及全部參數與結果載體，並拒絕弱匯入、附加位移、外來提供者、過期符號與虛構的借用效果。

`scripts.tests.test_generate_swift_witness_contracts` 的 String 讀取器檢查完整快取與查詢流程，拒絕 28 項儲存、ABI、流程突變與七種歧義宣告，並限制輸入預算。現有見證繫結測試涵蓋四個描述符與兩種架構，每個組合包含 33 項突變。描述符身分不提供堆疊框架配置或借用權限。

`PreparedFiniteKeys.*` 檢查上下文銷毀和重新命名、投影順序與上限、移動後明確失效、格式錯誤及不完整結果、空值域和非唯一值域，以及精確容量邊界。既有快取和堆疊偏移回歸也涵蓋預備鍵路徑。

`SourceABI.SwiftPointTransformKeepsTwoFloatingInputsAndResults` 在兩種架構上拒絕載體、佈局、上下文角色與間接結果變更。`SourceABI.SwiftPointForwardingPreservesBothIEEECarriers` 在 -O0/-O2 下編譯執行轉發原始碼，驗證兩個欄位的正負零、次正規數、無窮與 NaN 酬載。宣告測試接受編譯器及具名參數形式，拒絕簽名變更與歧義映像身分。

MainActor 測試資料檢查完整的固定中繼資料與靜態表流程，拒絕儲存、ABI、中繼資料擷取、表識別的變化、額外副作用、缺少或重複宣告及輸入預算耗盡。兩個目錄都要求每種目標提供全部三個配對 SDK 匯出。見證繫結測試在 arm64/x64 上涵蓋全部四種描述符，每種描述符與架構檢查 33 項輸入、匯入及 ABI 變異；主機執行階段檢查以五種具現化參數位元模式核對公開表。

`BitVectorEncodingClone.WatchMigrationAndGrowthOutliveTheSource` 在複製並銷毀來源後擴展文字表及共享 watch 串列，獨立修改同源副本，在一次 watch 存取處中斷傳播，並依據原始子句和獨立布林關係檢查恢復後的完整模型。

`ObjCCallHints.SwiftPublishedAccessorsKeepOpaqueValueAndAllKeyPaths` 在 ARM64/x86-64 與兩種規範 Combine 提供方上檢查兩個存取器，每個組合拒絕九項 ABI 變異與八項匯入身分變異。獨立 SDK 驗證在 ARM64 主機以 O0/O2 執行兩種原始碼架構設定產生的 C，在 128 次存取器呼叫中比較全部 24 個資料位元組、輸入輸出保護區與兩個物件身分。八種交叉編譯設定涵蓋兩種架構的 macOS 與 Mac Catalyst。執行階段檢查保留寫入器消耗的參照，不授予正式程式碼借用或所有權捷徑。

`ObjCCallHints.SwiftMainActorSharedKeepsObjectAndMetatypeContext` 在 ARM64/x86-64 上檢查完整結果與 swiftself 載體，每種架構拒絕十項 ABI 變異和十項匯入身分變異。獨立 SDK 驗證在 ARM64 主機上以 O0/O2 執行兩種原始碼架構設定的原樣生成 C：128 次呼叫保持單例與中繼型別身分，並平衡參考所有權。八種交叉編譯設定涵蓋兩種架構的 macOS 與 Mac Catalyst；x86-64 原生執行仍是獨立覆蓋項目。

`BitVectorEncodingClone.RootQueuePreservesDecisionsAcrossGrowthAndBudgets` 檢查根層已賦值與未決變數混合、非決策根變數、副本再次複製、來源物件銷毀、後續變數增長、兩種預設極性、預算中斷與恢復、衝突及重新啟動。完整模型與全部搜尋計數必須與全新編碼一致。

`ContextFiniteProofs.*` 檢查上下文與擁有者隔離、擁有者替換、權杖移動、精確謂詞與有序投影、節點追加、完整與不完整結果、儲存上限及 LRU 淘汰。框架測試要求快取前完成最終唯一性查詢，並在命中時保留符號節點限額。

`LinuxPriorityTests.cpp` 檢查明確任務狀態、執行緒隔離、缺少觀察值、無效 JSON、設定准入及拒絕效果。獨立的 x64／AArch64 原始呼叫程式在 O0／O2 下驗證 nice 限制、系統呼叫參數的 32 位元截斷、CAP_SYS_NICE／RLIMIT_NICE 權限邊界與核心 getpriority 編碼。在 `NeverDLinuxProcessTests` 中執行 `LinuxPriority.*` 與 `Backends/LinuxPriorityProcess.*`；涉及共用核心／JSON 時，再執行完整 Linux 程序、Android 原生及程序公開介面測試。缺少的可選原生傳輸仍明確跳過。

`LinuxKernelAvailability.*` 驗證明確缺少輸入與設定准入。`Backends/LinuxKernelProcess.*` 使用獨立的 x64／AArch64 O0／O2 原始呼叫程式，確認參數驗證前回傳 ENOSYS，且未指定與無關呼叫仍遭拒絕。Android syscall 範例對照原始 SVC 與 Bionic `syscall`，分別保留原始回傳值及 errno 效果。可用性變更須執行這些重點測試、完整 Linux 程序及程序公開介面測試，以及 Android syscall、原生入口和訊號測試。

`CompletedQueryCache.*` 涵蓋完整位元組域答案、所有緊湊槽位、成長、環境及所有者隔離、無效與不完整輸入及精確儲存邊界。原生分支迴歸維持固定邏輯查詢成本、精確預算與少一預算拒絕，即使完整答案省去了後端工作。


`BinaryLowIRRefinement.NativeTargetDomainsKeepIndependentProjections` / `FrameOffsets.FrameAndJointTargetProjectionsKeepIndependentSearches` 檢查帶分支變更和符號框架寫入的重複原生目標鏈、固定邏輯開銷、精確及少一次查詢預算、無效目標限額、閘預算耗盡和錯誤終點觀察。交錯的框架與相關目標投影還在謂詞替換前後保持完整元組、觀察順序及不完整結果拒絕。
`FrameOffsets.Cached*` 涵蓋根高位保持任意的位址平移、無號回繞、和式形狀變化、兩種快取模式、述詞隔離、零容量、查詢／節點預算拒絕，以及空值域與非唯一值域的差異。首次要求保留完整求解證明；後續平移可在沒有剩餘查詢預算時使用已完成的證明。

## 已釋出 Android GKI 核心契約

`AndroidTestExecution.def` 為 `FiniteRegistryRejectsBeforeSuccessAndCanBeReused` 設定 120 秒的整項 CTest 預算與 `RUN_SERIAL`。兩次工作負載各自保留 30 秒的有限執行預算，循序限制避免容量壓力案例爭用資源。六種 O0/O2 與重新定位設定皆採用此規則。

`LinuxPIDFD.*` 在載入前檢查分支解析、非法列舉、GKI 與缺失觀測衝突，以及格式錯誤、過量或矛盾的任務目錄。`Backends/LinuxPIDFDProcess.*` 以獨立 O0/O2 x64/AArch64 呼叫程式覆蓋八個分支的旗標、共用配置、限制、關閉再利用、封閉目錄、各版本非群組首領錯誤及純量/向量順序；缺失目標與非首領檢查先於 FD 耗盡，並覆蓋執行緒旗標及僅隱含 self 的空目錄。向量案例比較早期負長度與後續無法存取的中繼資料，以及原始跨度越過使用者上限、截斷跨度卻有效的緩衝區，覆蓋 pidfd 和兩個擷取串流。Android 的 `ReleasedGKIProcessDescriptorsShareRawAndBionicOwnership`、`ReleasedGKIVectorImportRetainsRawAndBionicErrors` 及 `ReleasedGKICatalogueRetainsRawAndBionicLookupErrors` 在六種編譯重定位設定中保留原始錯誤、Bionic errno、目錄及耗盡規則。先執行定向測試，再執行完整 Linux 行程、Android 原生及公開行程套件。測試執行模型，不啟動八個 GKI 核心。請參閱[已發布 GKI 契約](../android-gki-kernels.md)。

`ProcessCPUClocksRetainIdentityAndIdleSeparation`, `ProcessCPUClocksKeepMissingObservationBoundaries`, `LinuxClock.ProcessCPUObservationsShareAliasesAndRemainFixedWhileIdle`: `ProcessCPUClocksRetainIdentityAndIdleSeparation`、`ProcessCPUClocksKeepMissingObservationBoundaries` 與 `LinuxClock.ProcessCPUObservationsShareAliasesAndRemainFixedWhileIdle` 檢查八種固定版本的 raw／Bionic 身份、PROF／VIRT／SCHED、低 32 位引數、目標校驗先於指標故障、觀察值缺失、別名、CPU 非負值及牆鍾／CPU 空閒分離。`AndroidTimeTests.cpp` 檢查輸出和哨兵；協作式 syscall 樣例檢查當前非首領 TID 的程序組樣本。

`ZeroTimeoutPollRetainsReadinessAndOrderedCopies` 覆蓋八個 GKI 分支的 O0／O2 原始呼叫，檢查存活／負數／已關閉描述符、重複計數、引數收窄、超時／掩碼順序、只讀零 timespec、全部元資料先於就緒，以及後續故障保留較早 `revents`。`ZeroTimeoutPollKeepsUnobservedBoundaries` 保留核心、限額、掩碼、等待和就緒狀態的未知邊界。Android 的 `ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` 在六種打包配置中複驗共享表和 errno 所有權。

## 有界目錄批次屬性

bulk-attributes 檢查完整組、名稱/型別集合、未使用位元組保護區、low32 FD、bitmap 字、原生錯誤、dup 共用進度、獨立 open、快取 EOF 與零 rewind。字面值與未知模式僅用於虛擬環境。模型另涵蓋完整 stat、失效、NFD/255位元組名稱、輸入/輸出別名、傳輸/預算失敗、移動/SWAP/刪除/重用及明確授權。每個平台必需65個工作負載：ARM64 為195例，Intel 為130例；本地僅驗證匹配的 ARM64 HVF。native5s、guest/Python5,000,000us/quantum1024、public10s 不變。

## 有界 Darwin 硬連結

原始 link 跟隨最終符號連結目標；linkat 的 flags=0 連結符號連結物件本身，AT_SYMLINK_FOLLOW 跟隨目標。僅接受 low32 的 0/0x40，其餘低位在匯入前回傳 EINVAL。來源查找與目錄 EPERM 先於目標匯入；已存在目標回傳 EEXIST。目標父目錄需修改授權且雙方須屬明確建立的同一掛載域。初始身分別名及已知裝置/模式/旗標衝突仍不支援。

別名只消耗名稱項目及路徑/NUL 費用，不消耗新 inode；位元組、屬性授權及映射租約由共享物件持有。既有明確策略更新連結數與 ctime；缺少策略時完整 stat 未知。屬性修改使完整 stat 失效，內容修改使屬性觀察失效。描述物件及最後映射保留刪除名稱的費用；替換只抵扣可立即釋放的費用。子樹依確切身分及父目錄移動，樹外別名留在原位；相對符號目標使用所選項目的父目錄。

曾有多個名稱的物件，即使剩一個或零個名稱，F_GETPATH/ATTR_CMN_NAME 仍不支援；APFS 快取尚無通用模型。批量 NAME 來自實際項目，相同物件的普通重新命名/SWAP 保留雙方。EXCL 大小寫、O_SYMLINK、原生 Intel HVF、實體 iOS、ACL、映射一致性/EOF 訊號、dyld、Mach IPC、執行緒與完整框架仍是缺口。本節僅在上述契約內擴展前文限制。

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
65 mandatory workloads per platform / ARM64 195 / Intel 130
```

bulk-attributes 檢查完整組、名稱/型別集合、未使用位元組保護區、low32 FD、bitmap 字、原生錯誤、dup 共用進度、獨立 open、快取 EOF 與零 rewind。字面值與未知模式僅用於虛擬環境。模型另涵蓋完整 stat、失效、NFD/255位元組名稱、輸入/輸出別名、傳輸/預算失敗、移動/SWAP/刪除/重用及明確授權。每個平台必需63個工作負載：ARM64 為189例，Intel 為126例；本地僅驗證匹配的 ARM64 HVF。native5s、guest/Python5,000,000us/quantum1024、public10s 不變。

`MaterializedRuntimePreservesOwnedObjectsOnNativeWindows` 檢查原程式的模型執行、恢復、區段權限及原始與恢復程式的 Windows 原生執行，涵蓋私有堆積擴容與釋放、編碼內部指標、FLS 回呼重設、遞迴鎖、LastError，以及虛擬頁面的保留、提交與保護。`MaterializationRequiresKnownSupportedState` 拒絕缺失版本及動態 TLS；`RuntimeRestorationHasTheSameCAPIAndCLIContract` 比較精確位元組及報告。Linux 建構檢查與 Wine 觀察不能取代原生 Windows 生命週期證據。

`NeverDUnpackDriverTests` 涵蓋 DriverEntry 恢復、靜態／動態核心匯入、殘留核心資源、入口 ABI／控制狀態、畸形匯出、排程、要求／卸載生命週期、C API／CLI 一致性與 PE 檢查碼。原生驅動清單要求對應 KVM／WHP 案例執行；Windows 另以 ImageHlp 對照。這不代表原生核心載入驗收。見[脫殼](unpack.md)。

## 原生不透明狀態檢查

`X86PreservedState.*` 檢查重新解碼的純量形式、精確暫存器別名、嚴格重設及位元組/操作段/版本過期拒絕。`OriginalBinaryUndefinedIndependence.*Opaque*` 涵蓋分支、內部呼叫、完整間接目標、精確設定及獨立解碼的中繼資料預算恰好足夠/少一單位邊界。`BinaryLowIR*.*Opaque*` 涵蓋選擇見證與任意未定義選擇、多歸納來源、後期秩/預算拒絕、真實入口純量保持，以及 LowIR 不變但後續來源段位元組改變時執行摘要必須改變。`NativeUndefinedIndependence.*Opaque*` 和 `NativeStackControl.*FreshMemoryCall*` 檢查群組內部、切點前邊界、過期記錄及堆疊修改前的目標求值。重建受影響的中繼資料使用端，包括 `NeverDInterpreterLLVMRefinementTests`；分別報告 sanitizer、編譯故障注入與一般測試結果。
