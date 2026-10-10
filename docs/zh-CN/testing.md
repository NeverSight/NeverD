**语言**: [English](../testing.md) | [简体中文](testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← 文档索引](README.md)

# 测试 NeverD

NeverD 的测试回答三个不同问题：表示形状是否符合预期、完整 pipeline 路径能否
处理二进制 fixture，以及生成的代码是否保持行为。先选择能回答本次变更问题的
最小套件；对于高风险拉取请求，再运行更广的聚合测试。

## 配置测试构建

除非启用 `BUILD_TESTING`，否则测试不会构建。完整套件通常使用 Release；Debug
保留断言和单步能力，但有意不优化，不代表解码基准性能。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

完整 fixture 集要求 `clang` 能进行跨目标编译，并要求 LLVM linker（`ld.lld`
与 `lld-link`）位于 `PATH`。CMake 无条件构建许多可重定位 fixture，并在存在
对应 linker 时构建已链接 ELF/PE fixture。因主机无法编译或链接 fixture 而跳过
的测试属于未执行覆盖，不代表该目标通过。

克隆、构建配置与 macOS 预编译 LLVM 说明见
[CONTRIBUTING.md](CONTRIBUTING.md)。

## 解释器恢复检查

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

恢复 API 测试覆盖 v1/v2/v3 默认值、显式预算、截断结构、各层 reserved 字段和未来尾部兼容。CLI 测试在两种 ABI、两个源码后端下检查字段／查询预算耗尽及成功恢复，拒绝非法十进制上限，并要求 `--devirtualize`。预算耗尽不得发布源码或部分残余图。

v4 测试固定前缀大小及填充，拒绝截断布局和未知标志，保留旧接口/未来扩展行为，并验证未请求报告时 C 中仍保留有符号范围。独立 CLI 用例分别要求串接以保留关联、要求范围以证明无符号栈比较；两个 C 后端均在 O0/O2 下执行并启用未定义行为陷阱。关闭发现必须改变依赖发现的恢复结果。解析测试覆盖零串接、整数极值、溢出、错误范围和缺失前提。Python 检查布局、标志、签名及拥有所有权的失败报告。

`NeverDByteMemoryForwardingTests` 覆盖重叠写入的最后写者、双端序、直到 i128 的整字节宽度、已定义值、保持关联的 undef/poison 快照和部分覆盖。负例在未知别名、地址空间转换、调用、有序访问、生命周期变化、缺失字节、动态或无效偏移、分支和循环处保留读取。测试还覆盖大量输入的 PHI、零预算、精确预算、预算耗尽及默认拒绝快照。原始与改写 LLVM 在 O0/O2 下对照独立算术基准执行；现有 MBA、LLVMC 和解释器源码测试保护流水线兼容性。

新增回归在两种内存模式和端序下保留纯位运算 intrinsic，并检查普通调用、operand bundle、convergent、生命周期、内存副作用和陷阱仍构成边界。原始及改写代码在 O0/O2 开启未定义行为陷阱，对照独立基准检查返回值中的数值地址及缓冲区每个字节。

字节活跃性回归覆盖不重叠读取、部分被观察的写入，以及多个后续写入组成的覆盖。带条件的地址测试涵盖 AND/OR/XOR、32/64 位、两种端序、错误根、不完整掩码、绕过检查的合流及每个地址预算边界。O0/O2 独立 oracle 检查两个分支结果、全部十六种地址余数和输出缓冲区的每个字节。

地址关系回归覆盖整数和指针 PHI/select、双端序和指针宽度、稳定及变化的回边、未定义/freeze 入边、无入口锚点的环、相邻地址/输出预算和仅地址改写时的分析失效。O0/O2 循环逐次对照独立 oracle 的返回值和整个缓冲区，包括不能当作常量地址处理的移动地址反例。

数值地址测试覆盖单写者完整及子字转发、不新增快照的 undef/poison、双端序、32/64 位表示、模运算负偏移、部分重叠、不同根与 alloca 的别名、可抛异常的调用、循环、相邻工作量与用途预算，以及只删除写入时的分析失效。O0/O2 执行将原始与改写 IR 对照独立基准，同时检查返回值和别名缓冲区的每个字节。

`NeverDMedMutableSourceTests` 和 `NeverDLLVMCValueTests` 在 O0/O2 下执行独立编写的循环、块重排、入口回边、运行时栈运算、较早读取、分支汇合、部分别名、布尔真值和包含零输入的位计数。负例要求在发射前拒绝畸形输入、截断目标、歧义承载和预算耗尽。超过 SSA 限制的 CLI 用例要求 LLVMC 输出可执行，并要求 HighC 明确拒绝。 连续更新和跨块存储表达式链还会检查生成 C 的体积与执行结果。

新增回归限制 LLVM 提升前的私有读写数量和 C 输出大小，执行长混合运算链、重排的 SSA 块、重叠的客户内存写入及零返回值的 O0/O2 检查；关键用例还经过实际 LLVM 优化流水线，并验证模块生成被拒绝后可以安全复用发射器。

复合条件回归在 O0/O2 下执行包含非零常量相等、无符号比较、两种操作数顺序的有符号比较、扩宽布尔输入及全部布尔否定组合的合取和析取。C 发射必须保留完整真值表，且不得解引用不存在的零比较操作数。整数地址存储覆盖对齐与非对齐的 32/64/128 位承载；字节存储数组保留显式对齐以及精确的首地址和部分访问，不得生成标量对数组赋值或不兼容类型的别名访问。

`NeverDLowIRRefinementTests` 覆盖实际恢复的残余图、不同结构的有限循环、零次迭代、独立动态生产者、条件见证、重叠输入视图、复制与溢出关联、两边不可变读取证据、强制系统标志和返回槽保留。错误候选、额外写入、不完整或无限路径、过期证据、临时区冲突及共享预算耗尽必须拒绝证书；已有独立性测试仍拒绝可观察的任意值。

`NeverDLowIRRefinementTests` 中的 `CompleteModel`、`CompletedTargetFacts`、`ConditionalImplication` 和 `PartitionedCoverage` 用例使用小输入域的独立穷举判定，并检查畸形输入、过期缓存、不完整枚举以及恰好/不足预算。实际 LowIR 和二进制精化用例在固定门数限制下检查条件乘积及原始/恢复图的所有终止分支，拒绝被改动的最终观察、不相关输入域和缺失目标。`FiniteValues` 测试区分编码失败与搜索、值数量及全局预算导致的拒绝。

同一目标中的 `LowIRLoopRefinement.*` 和 `BinaryLowIRLoopRefinement.*` 覆盖任意 64 位计数、嵌套字典序排名、真实原生残余代码、入口前缀模板、重叠视图及相关溢出。负例拒绝错误循环体、缩小入口域、不下降的排名、无符号回绕、遗忘之前的写入、遗漏切点、畸形模板和共享预算耗尽。成功的有限分支不能授权不完整的归纳证明。

`LowIRLoopInference.*` 和 `BinaryLowIRLoopInference.*` 使用独立编写的计数器、栈存储、提前返回、原生调用和打包标志位用例，覆盖窄位宽算术拓宽以及表达式不同但语义相等的标志状态。畸形图、缺失或伪造的来源、不终止／回绕循环，以及推导或证明预算耗尽均不得产生证书。

零前缀回归检查拼接分组、非标准位宽及穷举字节对，并保留未知和非零位。独立编写的帧循环覆盖两种字节序中分开写入低位值和高位零、窄位宽、错误算术与填充、恰好及不足的推断预算，以及独立的完整证明查询预算。

共享入口与共享回跳块的回归覆盖零扩展的 32 位及完整的 64 位计数器、非单位步长标量排名、错误结果、不进展与回绕路径，以及标量和组合排名搜索之间恰好足够或耗尽的累计预算。`LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`。 另有递增与重置回归，要求无需每轮只展开一个计数器位即可收敛，并拒绝缺失进展和无符号回绕。 调度回归覆盖循环携带的非单位步长累加器、有效非单位步长标量排名旁可能回绕的单位计数器，以及有效组合排在早期窗口之后的三个计数器。恰好足够和少一次的排名预算检查确定性的继续搜索，并防止重复候选。

同一目标中的 `LowIRLoopPlanPairing.*` 检查寄存器重命名、不同算术体、双方独立前缀快照、谓词保留、共享帧输入、嵌套切点覆盖和独立证明预算。缺失关系、错误写入、无效临时值绑定、不完整配对或元数据预算耗尽均不得产生证书。

`LowIRLoopAlignment.*` 使用独立编写的普通和旋转栈帧计数循环：两个默认自关系计划分别证明成功，首次配对失败，另一个候选切点则证明关系成立。回归覆盖多切点排列、错误结果和栈帧写入、缺失或过期的原始记录、显式未定义值 witness、不递减或回绕的计数器、畸形图、失败尝试的累计查询、恰好足够的总预算及搜索限额耗尽。任何拒绝结果都不得包含证书。 新增用例覆盖分离的重置与进展阶段、移动退出判断后需要跨族配对的等价循环、不重复推断的共享缓存，以及缓存元数据合计超限。后接独立循环的用例以显式 16384 次查询上限验证完整周期覆盖。空候选族与重复候选族不消耗符号查询，切点不足必须拒绝。同时检查恰好足够与少一次的全局预算、错误结果、缺失进展、原始证据和未定义值 witness。 过滤候选族回归覆盖保持结果不变的算术菱形、局部汇合与仅在边界汇合，以及能够绕过可达汇合点退出或返回循环边界的路径。测试原始过滤族重复时仍尝试候选过滤族、先成功的过滤计划被后续完整分支尝试复用、恰好足够／少一次／零 `MaxCutSelectionWork`、失败后的累计 `CutSelectionWork`，以及全局图工作预算耗尽后不启动符号推断。两种分支候选族都验证完整周期覆盖；菱形关系使用显式推断与证明查询上限。

局部计数器回归覆盖栈帧和寄存器、递增和递减、低位／中间位／高位、非标准宽度、两种字节序及三字节帧字。测试检查延迟发现新区间、篡改保留位、不进展和无保护回绕、无效新增入口、恰好足够／不足的推导预算，以及原有单切点搜索。

前置阶段回归覆盖两个和三个顺序循环复用同一倒计数字、与原有嵌套循环阶段组合、恰好足够与少一次的排名／查询预算、不进展循环，以及重置回前一阶段。同宽但错误的阶段常量、错误结果和帧写入必须被完整检查器拒绝且不产生证书；缺失原始证据仍返回不支持。

`NeverDLowIRRefinementTests` 中的 `InterpreterMachineStateModel.*` 使用独立编写的 LowIR 用例，检查原始入口标志、状态码与客体 RAX 的区分、全部 17 个状态字、部分寄存器分片、打包标志、动态拒绝状态的持续保留、客体栈帧写入、两个分支以及循环推断后的全新证明。错误输出、丢失状态、内存变化、过期指令记录、非法输入和生成预算耗尽必须失败。已有机器状态源码测试还覆盖两条 C 路径的 O0/O2；模型测试本身不证明编译后的 C。

`NeverDLLVMInterpreterModelTests` 将独立编写的 LLVM 与完整状态 LowIR 参考实现比较，覆盖位宽、并行 PHI、switch、客体内存、独立状态码、poison 检查、内建函数值域、被拒绝的契约和四种建模预算。测试完成任意字长倒计数循环的完整证明，并拒绝被改写的状态码。独立 C 用例在 O1/O2 编译后必须满足同一观察契约。这些测试验证受支持的模型；自动不变量发现和编译器正确性仍是独立义务。 变量移位用例覆盖全部四种位宽、经掩码或分支限制的移位量、边界及越界移位量、无回绕与精确标志、严格 poison 拒绝，以及 O1/O2 编译后的 C。

`NeverDLLVMScalarEquivalenceTests` 覆盖完整循环域、零次循环、PHI 同时交换、switch、高位输入、最后分区反例、产生 poison 的额外更新、返回范围、不支持的契约，以及精确、少一单位和零预算。独立双宽与溢出参考实现覆盖各受支持字宽的漏斗移位端点和带溢出约束的乘法；独立嵌套循环 C 在 O1/O2 检查编译器输入形态。状态模型测试也检查漏斗移位端点。`SymExpr.ConstantWindowSharesActualWorkWithoutRelaxingQueryCeilings` 检查累计查询计费和不变的局部上限。

`LLVMScalarDecision.*` 覆盖深层精确移位/扩展约束、常量分支决策、两条循环回边、保留高数据位、末端未定义操作、不终止、已检查函数的后续修改，以及精确/少一/局部预算。`LLVMScalarDecisionCompiled.DeepOneAndTwoBackedgeOracles` 将独立编写的单回边和双回边递推与无符号 C oracle 在 O0/O2 下比较，共 32,768 次调用。这些是标量模型检查，不代表原生 ABI 或完整二进制还原覆盖。

`LLVMScalarDemand.*` 在固定工作预算内证明不同的非线性循环函数体，保留全部16个控制分区及自由高位输入，拒绝最后分区的输出或定义性失败，并检查精确/不足的工作预算和节点上限。`LLVMScalarDemandCompiled.*` 在 O0/O2 对照独立无符号算术 oracle，共运行262,144次调用。测试验证查询时机，不删除源操作或缩小输入域。

`SymKnownBitsTests` 以全部字节输入对和任意精度边界值检查事实，涵盖扩展恒等关系、不回绕加法、不同来源及全定义移位语义。还检查精确/少一预算、缓存命中计费、存储上限、独立上下文、深度、操作数数量与不支持的位宽。`SymExprExtensionTests` 检查常量高位及完整移位计数。标量等价回归在证明值域约束时保留符号数据，拒绝最后分区差异及已执行的 poison，并以精确/少一预算核验完整证明计费。 `SymMBAExtensionTests` 在关闭采样验证时要求推导证据，检查全部字节输入对，并保留有符号、窄进位、按位取反及工作量耗尽的边界。

`NeverDLLVMScalarLoopRecoveryTests` 覆盖前缀和前驱携带值重构、零次循环分支、自回边循环判断前移、仿射状态、回绕时的等式退路、额外更新的 poison、高位数据差异及不支持的输入契约。精确/少一累计预算检查原子拒绝；独立算术 oracle 在 O0/O2 执行原始与恢复 LLVM，覆盖全部字节控制输入。这些测试不代表原生 ABI 恢复或默认短 C 输出已完成。

循环谓词回归覆盖 8/16/32/64 位 PHI、交换比较操作数与分支极性、有符号/无符号扩展、不同外部初值、冲突入口观察、多回边、隐藏符号数据反例、截断拒绝、原始 poison/不终止及构造/证明/候选/转换限制的原子拒绝。独立无符号 oracle 在 O0/O2 对照原始与恢复后的 LLVM，共 458,752 次调用并启用未定义行为陷阱；源函数及父模块保持不变。

退出边界回归覆盖8/16/32/64位模运算条件、递减步长、两种极性、零次循环、共享出口、仅零数据一致的错误相邻边界、步长无法到达的边界、超限切片、poison、缺少同宽叶节点及原子预算拒绝。有效前缀初值前的40个未使用参数不得挤占有界搜索位置。独立无符号 oracle 在 O0/O2 执行原始与恢复后的 LLVM，共458,752次调用并启用未定义行为陷阱。

同一测试目标还覆盖 `recoverLLVMScalarSource`：搜索前准备、仅清理而无循环变换、全位宽未使用状态、死代码中的溢出/精确移位/除法及 assume 义务、不支持的副作用、精确/少一累计预算和有界继续。独立算术 oracle 在 O0/O2 执行原始与准备后的 LLVM，覆盖所有字节控制值及确定性全位宽状态。成功或拒绝都必须保持源函数及其父模块不变。

源准备守卫测试覆盖带注解的模恒等式、不同前驱中的等价表达式、不同分支值保留、原始溢出/exact 移位/截断/扩展拒绝及精确/不足累计预算。独立无符号 oracle 在 O0/O2 对照原始与准备后的函数体，共131,072次调用并开启未定义行为陷阱。已有语义 pass 测试继续覆盖独立谓词的拒绝规则。

掩码回归覆盖交换操作数、零字段、保留输入高位、完整数据证明失败后的替代值、所有回边、回绕/溢出拒绝、超过32项的批次以及精确/少一预算下的原子拒绝。独立 LLVM 与生成 C 的算术预期在 O0/O2 下检查其与位宽恢复的组合。自等价回归保留完整控制域、poison/undef 与不支持约束的拒绝、非终止、局部上限和精确/少一工作计费；修改同一函数后，旧结果不再有效。这些仍是标量 LLVM 覆盖，不是原生 ABI 认证。

共享掩码包含关系测试穷举全部字节输入对，覆盖非连续掩码和最高128位字，保留未知位/高位，并约束超出项数和位宽上限时的节点增长。带两条回边的符号循环必须相对独立闭式表达式证明掩码 XOR 递推。控制依赖测试按规范化后的移位量存储计费，并保留精确/少一预算和宽值保守退路。

位宽回归覆盖非零常量、不变签名、更宽输入和intrinsic、可观察高位、有符号次序、新增溢出及精确／不足预算。同一标量候选在x86-64、AArch64、大端AArch64及ARM32目标三元组下测试，属于LLVM层覆盖，不代表原生ABI认证。原始／恢复LLVM及生成C分别通过O0/O2独立算术oracle，C启用未定义行为trap。

种子回归覆盖全部外部入口一致性、多回边、隐藏高位数据、poison、并行交换、失败批次分拆、超过 32 个携带变量及原子预算。标量证明测试区分已完成的未知查询与全局工作量耗尽，并在不枚举数据位时证明安全移位。符号测试穷举字节值、掩码与计数，保留可观察高位、来源身份和大计数语义。跨位宽移位视图必须共享完整数值计数。这些检查不认证原生 ABI 恢复。

新增循环回归覆盖窄位宽末索引回绕、分离的 body/latch、两种入口条件方向、等式操作数交换、单位步长变量换序与递减、零次路径的高位数据差异、新增执行的 poison 和错误边界候选。精确/少一构造及证明预算和候选耗尽保持整体拒绝。原始 LLVM 与生成 C 在 O0/O2 下对照独立算术 oracle 运行。

`NeverDLLVMCScalarLoopRecoveryTests` 检查默认整模块与单函数输出、返回路径清理后的继续恢复、函数身份和属性、调用绑定、已有及新增内建函数与符号冲突、共享预算、副作用调用、缺失输入定义性、元数据、映像投影、外部块地址和外来函数选择。独立算术与旋转 oracle 在 O0/O2 下执行生成 C，并开启未定义行为陷阱；算术还与独立编译的原始 LLVM 对照，覆盖所有字节控制输入、边界字值和确定性全位宽数据。

`SymSimplifyPredicates.*` 还对照独立阶段与完整 pass 的策略、报告工作量、恰好及少一单位预算、禁用阶段和带混淆标记的函数。标量源码回归覆盖 8/32/64 位算术编码循环终止条件、有符号溢出拒绝，以及跨轮次和函数共享的构造限制。发布失败时保留原 IR；生成 C 在 O0/O2 下与独立编译的原始 LLVM 和算术 oracle 对照运行。

`SymKnownBits.*` 使用穷举字节对算术检查无损掩码和有符号移位往返，覆盖负数、不同源值、被丢弃的未知位、因子或移位量不匹配及宽位移过移。直至 128 位的检查保留精确/少一查询预算，且不增加 DAG 节点。标量决策测试加入不同回边上的正负数更新、源修改后旧事实及溢出拒绝、保持符号的高数据位，以及对独立无符号 oracle 的 16,384 次 O0/O2 调用。

`SymKnownBits.*` 还穷举字节对检查倍数排序和跨位宽乘积，拒绝回绕、错误系数或因子重复次数，以及把窄位宽溢出移到更宽的字。直至 128 位的查询保留精确/少一工作预算，且不增加 DAG 节点。标量测试在不枚举数据位的情况下证明重复加法及乘法，保留每个循环溢出义务，并对独立 oracle 执行 16,384 次 O0/O2 调用。

`LLVMScalarAssume*` 检查完整循环域、最后分区失败、不可达与已到达的假条件、全部字节输入上的累积定义性、精确/少一预算、IR 修改和不支持的调用契约。四个目标 triple 验证共享建模；8,192 次 O0/O2 调用对照独立无符号 oracle。状态模型测试另行检查相同义务及操作数 bundle 拒绝。

`LLVMScalarProjection.*` 覆盖嵌套字段、位窗口、未使用参数保留、多返回、回边、未选中运算的溢出/移位/assume 义务、最后分区失败、非终止、未知契约、输入修改和精确/少一预算。四个目标三元组验证共享语义。`LLVMScalarProjectionCompiled.*` 通过 LLVM 数组桥接原始聚合，与投影窗口及独立无符号算术在 O0/O2 下对照。`SymExpr.RightShift*` 穷举字节对，检查符号扩展、进位、保留高位、完整计数及有界发现。

`LLVMScalarInputs.*` 检查有序混合位宽映射、零参数及无名接口、死算术与 assume 的输入需求、输出突变、未知契约、包装拒绝，以及精确/不足的累计预算。证明测试恢复完整原始签名，不固定省略输入。`LLVMScalarInputsCompiled.*` 在 O0/O2 运行原始及精简循环接口，对照独立无符号 oracle，并改变所有省略参数。

`NeverDLLVMScalarStateProjectionTests` 覆盖重叠及非对齐窗口、8/16/32/64 位单元、循环、入口掩码、源修改、状态范围、保留的 poison、外部内存拒绝及精确/少一预算。原内存函数与 LLVM 聚合桥在 O0/O2 下对独立字节/算术 oracle 执行 172,032 次比较；标量证明另行检查。`SymKnownBits.*` 穷举字节对，检查 128 位、不同因子、扩大掩码、回绕求和及预算边界。`LLVMCIntrinsicSemantics.AssumeEvaluatesItsConditionAndRefusesBundles` 检查 O0/O2 的条件单次求值及 bundle 显式拒绝。

循环元数据回归测试在全部控制分区及精确/少一预算下，将计数循环与独立公式比较。过大或为零的剥离历史计数不能掩盖错误结果、不终止或 poison。通过 API 构造的畸形元数据单独验证导入器拒绝行为，避免与 LLVM 汇编解析混淆；机器状态测试保留状态副作用及输入限制。

初始化契约回归覆盖部分及分离的字节范围、固定别名、两个分支、每个返回点、循环首轮读取和循环内先写后读。先读后写、漏写、客体写入、未知别名、特殊内存访问、对象外范围，以及输入／工作预算耗尽，都必须失败。独立的仅输出状态字 C 用例在 O1/O2 编译后保留精确 LLVM 属性，并通过全新的原生到 LLVM 组合证明。

受条件保护的倒计数测试覆盖拒绝循环体模板后的重试、任意字长输入的完整循环头证明、切点与查询预算的累计计费，以及真实入口契约违规时立即拒绝。

`NeverDInterpreterLLVMRefinementTests` 检查全新的原生到 LLVM 组合证明、精确文本／函数绑定、独立预算、完整观察项及刻意扩大的源码域。修改字节、残余程序、结果、标志、状态码、栈帧写入、poison 或错误／过期循环方案，都必须拒绝组合凭据。任意字长倒计数要求两段归纳前提；独立 C 用例在 O1/O2 编译后验证真实序列化 LLVM 输入。状态模型回归拒绝隐藏入口回边，对入口集合计费且不复制附属来源信息。

`InterpreterLLVMRefinement.Preservation*` 覆盖局部／重叠范围、非法请求、独立计算的准备开销、两端相同的最终破坏、跨循环的入口保存／恢复、新鲜不透明状态证据及后期拒绝。`NeverDPEFixedImageTests` 也是 API 使用方，需要重新构建。未提供请求时的结果、计数与摘要另行对照基线。

`InterpreterLLVMRefinement.Collection*` 检查有限与归纳证明所需的保留／延迟策略、可达坏分支、后期源码拒绝、入口保存及全部四种原生策略身份。编译组合层遗漏传递的故障，确认每项必要选项确实到达原生检查器。

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

两层和三层循环的缓存相等退出测试覆盖操作数相关性、变化的边界、计数器重置和被破坏的复制。

比较缓存回归覆盖相等与不等、带守卫和常量折叠的初始化、扩宽后才出现的字段，以及字节、双字和四字缓存中的第 7/31/63 位。保留被检查位而仅改变相邻位，也必须被完整状态比较拒绝。零步长、移动边界、计数器重置和共享预算耗尽必须拒绝。

泛化前缀回归覆盖汇合入口、首个零次迭代见证、隐藏寄存器／栈帧差异、非规范布尔谓词、原生陷阱约束、相关联的栈溢出保存，以及错误或预算耗尽的计划。独立的两层／三层等值退出计数器及原生字节检查无符号输入边界、零值／最大值输入域、非单位步长和错误原始指令。推断及最终证明均必须拒绝不完整结果。

独立编写的分支循环回归覆盖两种分支方向、错误循环体、不终止的相邻分支，以及共享搜索／证明预算耗尽。 `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

嵌套推导回归覆盖两层及三层循环、递增及递减计数器、自动阶段常量和真实原生循环体切点。不可达或互斥的前缀域、错误循环体、不终止或回绕转换，以及共享搜索／证明预算耗尽都必须拒绝。前缀证据不能替代完整段覆盖。

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests` 检查完整无环 LowIR 图的两次执行独立性。两侧共享普通入口输入；每次新产生的架构未定义值在复制、重叠写入、溢出保存和重载中保持来源关联。控制谓词先于路径假设接受检查。证书要求 `Complete` 效果元数据，并精确绑定每条指令的完整边界和操作摘要。缺少证据、可达循环、调用、未知别名或预算耗尽都会拒绝证书。结论受显式观察项和无故障栈帧契约限制，不是原生代码到 C 的完整等价证明。

以下行为采用默认的严格审计契约。`NeverDOriginalBinaryUndefinedIndependenceTests` 使用独立编写、固定映射的 x64 字节，验证物理原生 CALL/RET、改写的返回目标、有限间接目标全集和不可变加载。同一测试目标还检查直接分支完整收集、精确字节／效果／映射／读取见证绑定、外层返回时入口 RSP 及返回地址槽保持，以及栈帧与映像分离前提的可满足性。缺失或重叠指令、不符合精确陷阱和显式环境投影规则的未审计分支、不终止或超预算的循环、不完整目标枚举、执行配置／契约不符和预算耗尽均须拒绝，且不产生证书或残余代码。成功要求每条可行原生路径完整结束。此可选门禁不认证循环不变量、异常分派、启用 CET 的执行或原生代码到 C 的等价性；普通恢复仍独立可用。该目标还检查严格提升的 `INT3`/`UD2` 终止边界及其完整字节、操作摘要绑定。未定义输出附属元数据的 `Missing` 必须保持不变；仅经符号执行证明不可达的陷阱可进入证书，任意可行陷阱路径都须返回 `ContractViolation`，且无证书、无残余代码。不建模陷阱后的顺序执行或异常恢复，不使用 `codeFollowsTrap`，静态 LowIR API 的支持范围保持不变。

物理返回回归覆盖直接和间接调用跳过无效内联字节、可达的错误返回位置、完整目标枚举及恰好和不足的预算。完整状态关系拒绝被改动的结果；独立 C 经 O1/O2 编译后也必须保留完整帧写入，返回值相同不能掩盖返回槽字节变化。

显式原生交叠测试覆盖真实 x64 跳入立即数的分支、两个可行分支的结果，以及位于先前指令内部的间接返回入口。合成提供器测试覆盖两种收集顺序的包含式交叠、未执行直接分支上的冲突字节、两种顺序的代码／读取一致性及候选读取。精确和不足的字节预算按间接转移累计计入重复交叠字节。分支结果改变、静态或循环接口使用及证据矛盾均必须拒绝证书；启用选项或改变额度会改变摘要。

拒绝边界的显式启用测试覆盖不可达的 RCL、LOCK 内存 XADD 和 REP MOVS、符号路径矛盾、任意值控制的分支，以及入口、间接跳转、CALL 和 RET 到达时的精确拒绝。测试还检查可达后缀的独立入口、候选与原生地址重合、格式错误或不完整的证据、资源耗尽、静态/循环接口拒绝，以及精化证明的三层摘要绑定。修改不可达指令，或在无保留边界时切换选项，都会改变证书摘要。这些测试验证声明的有限证明范围，不证明未审计指令的语义。

打包标志测试覆盖全部标量入口标志组合、特权掩码、两次执行的 TF/AC 条件、不同未定义产生点、相关副本、原生调用、兄弟路径状态、强制最终系统状态观察、畸形证据及资源计费。有限循环必须结束每条可行输入路径；安全分支不能掩盖无限或截断路径。RDSSPD/RDSSPQ 检查覆盖 16 个通用寄存器和两种宽度、高位保持、保留 `Missing` 证据及伪造投影拒绝。机器状态测试在两个 C 后端的 O0/O2 下开启未定义行为陷阱，与独立用户态标志位预言机比较，并检查环境失败状态不会被后续操作清除。 INCSSPD/INCSSPQ 测试覆盖两种宽度和全部通用寄存器、不可达边界保留、安全兄弟路径完成后的可行陷阱、零操作数及伪造陷阱证据。

`NeverDX86DecodeDetailTests` 覆盖三种解码入口、两种 x64 地址宽度、有符号位移边界、强制前缀、真正的 i386 disp16、moffs、截断输入及无详细信息的重复使用。仅精确的重定位字段可绑定，错误宽度、偏移或数值均不能绑定。原生独立性及关系证明测试还保留完整帧写入，拒绝可观察的任意标志位及被修改的移位候选。

`NeverDLowUndefinedDigestTests` 检查独立 SHA-256 向量、每个已存储字段、有符号序号位、顺序、填充字节排除及输入不变性，覆盖内联缓冲增长、199/200 操作边界和更长的增量跨度。`LowIRRefinement.StaleUnusedInputRefusesAcrossDigestStorageBoundaries` 在两条路径上检查真实过期证据的拒绝与重新绑定。保留 `NeverDLiftTests` 中的 `InputDigest.*` 覆盖；修改独立实现时重编译受影响调用者。记录实际 sanitizer、可移植路径及主机覆盖；摘要微基准不能单独证明原生等价。

```bash
cmake --build build-release --target NeverDLowUndefinedDigestTests --parallel 4
build-release/bin/NeverDLowUndefinedDigestTests
```

`NeverDX86UndefinedEffectsTests` 检查未定义位元数据、已定义／保留标志及过期证书拒绝。`NeverDX86CarryArithmeticFlagTests` 用算术参考实现检查寄存器和内存形式 ADC/SBB 的辅助进位。`NeverDX86LogicIdentityTests` 检查相同操作数的 AND 在 64 位模式下写入 32 位目标时，仍清零其所属 64 位寄存器的位 63:32，同时保留窄位宽写入未覆盖的位。

`X86RotateUndefinedEffects.*` 用标量算术基准覆盖全部原始计数、操作数宽度、CL 重叠、高字节别名及内存目的操作数。`X86BitTestUndefinedEffects.*` 覆盖寄存器/立即数索引、源与目的重叠、扩展寄存器、已定义标志和寄存器高位写入。元数据反例拒绝被修改的操作数、编码及不支持的形式。原生证明区分同一任意位的关联读取与不同任意位，检查恰好及不足的生产者预算，并拒绝可观察的未定义溢出位。完整状态细化检查接受选定见证，拒绝零位见证或被篡改的候选。

`X86XaddAudit.*` 通过无符号算术基准检查全部 65,536 对字节输入、更宽位宽的标志边界、寄存器/高字节重叠、两次写回、REX 字节宽度限制和完整寄存器保留。原生检查要求不产生新的任意位，同时保留先前的依赖；两种见证均接受未修改的 XADD，篡改和、交换源值或已定义标志则被拒绝。`/6` 别名使用完整移位计数矩阵，并通过修改组号/解码 ID 的反例拒绝语义错配。 内存测试还穷举字节输入对，覆盖地址覆盖前缀、扩展、IP 相对及 i386 16 位寻址、有符号位移、相邻字节和过期解码细节的拒绝。完整帧原生检查保留先前任意值依赖及精确／不足预算检查；篡改存储地址会违反返回槽契约。

`X86DoubleShiftUndefinedEffects.*` 以独立的逐位转移基准检查 16/32/64 位运算的全部原始字节计数，覆盖源／目的／CL 重叠及精确生成守卫。原生检查先丢弃 RAX 再隔离标志，区分计数 16 与 17，保留先前依赖并执行精确／不足预算检查。完整状态关系拒绝被修改的有定义部分及全零位见证；低字未定义不允许清除有定义的高位。畸形形式不发布部分证据。

`*Deferred*` 用例覆盖不可达跳转侧与落空侧、缺失或畸形代码、符号矛盾守卫、任意控制、选值见证及完整状态变异。合成提供者验证不会取不可达后继，而可达的畸形元数据仍被拒绝。精确／不足指令、操作、访问及查询预算、可达坏分支和无限循环均不能认证前缀。策略改变会改变证书摘要，静态与循环 API 拒绝该选项。

`NeverDPEFixedImageTests` 使用独立构造的 PE 文件，检查带重定位的指令与不可变数据、导入写入范围、畸形头部／表、别名和来源信息篡改。原生到 LowIR 及精确 LLVM 证明接受匹配候选，拒绝结果、状态或原始字节被修改的候选。准备预算耗尽保持独立分类，允许显式提高限额后重试；普通加载也接受含 40000 条有效重定位记录、超过默认分析预算的文件。

`FrameOffsets.*`、`NativeStackSpecialization.*` 和 `OriginalBinaryUndefinedIndependence.*` 检查 2/4/8/16/32 字节对齐的全部余数、自由高位、跨调用保存、倒计数循环、别名破坏、错误分派、无关大掩码、必要分区升级及恰好／少一次预算。独立原生控制检查带分支约束的对齐、内部无符号返回清理、错误清理量和带前缀返回。这些测试不代表分区循环已具备自动原生到 LLVM 的完整证明。

原生帧缓存回归覆盖以一次加载的查询预算执行 64 次重复对齐加载、查询预算少一、地址越界，以及一条路径返回后另一谓词下的相同地址。仍运行完整状态篡改和缓存键/容量测试。

重复可行性回归保留全部 130 条原始指令，同时使用与两条直线指令相同的查询预算；查询或指令预算少一仍拒绝，分支及入口域变化后的可达陷阱、求解门预算耗尽和候选状态被修改仍拒绝。

帧偏移回归覆盖 558 个切片宽度／对齐／余数／偏移组合，在不足以展开整个帧根相减的门预算下证明；同时检查来源或偏移不匹配的切片、未约束的稀疏掩码、模运算进位与回绕、嵌套掩码及节点／查询耗尽。原生测试验证部分对齐指针的精确存储，拒绝缺失对齐和帧外访问，并在完整状态精化中拒绝被修改的存储值。

入口同余证明测试覆盖对齐1/2/4/16的全部余数、两个不同根寄存器、自由高位、非法域、矛盾入口常量、不回绕范围、排除区间空隙、保存义务和精确/少一查询预算。循环归纳模板保留原入口根条件。重新执行的原生独立性/关系证明和原生至LLVM检查拒绝不匹配的域及改变的状态码，证书摘要绑定两侧域。用例独立编写，不从ABI或一次执行推断对齐条件。 另将独立编写的带入口检查C函数原样编译为O1/O2，在两个余数下证明与实际原生指令序列一致；同一编译产物在不同余数下必须拒绝。

寄存器分区回归覆盖大小端、高位栈帧根、覆盖及重叠字段、后续边、变宽的前驱、原生 CALL/RET 和刚好足够或差一步的预算。两种 C 路线在 O0/O2 执行全部四种内存情况。原生细化检查分别绑定两个选择器值，不构成无约束输入证明。独立 LLVM 用例验证：将假分支移到共享汇合点前，不能导致它在真分支后继续执行，并检查 PHI 复制与存储。

入口对齐回归覆盖更细分区、全部允许余数、不同高位根地址、最后案例失败，以及精确和少一预算。两个 C 后端均在 O0/O2 下用不可访问的被拒客体地址及无效标志验证：状态 2 必须保留全部状态字节。模型细化检查同一拒绝语义；C/Python 测试覆盖 v5 布局、所有权及旧版和未来尾部。原生证明控制明确拒绝未绑定的对齐域。

`StringTransfer.*` 与重复搬运回归检查重叠、零计数、临时变量隔离、容量／预算限制和指针失效。`MachineStringSourceTests.cpp` 对四种宽度和两个方向，将原生执行及两条 C 路径的 O0/O2 结果与独立的全寄存器、标志和栈参考结果比较。

`ControlDiscovery.*` 与 `NativeStackSpecialization.*` 覆盖根地址低位条件、高位和完整根依赖、不完整遍历、恰好足够及不足的遍历预算，以及有限不可变地址证据的保留。

未解析目标的守卫测试覆盖不可达的非法阶段、入口或回边之后真实可达的未知目标、精确细化次数上限，以及相邻的成功与耗尽发现预算。拒绝时不得发布残余代码、来源记录或读取见证。完整恢复之后的可选发现开销不是必需预算的下界。

按需求投影帧关系的回归覆盖自动及已有寄存器载体、高位根地址与模回绕、只约束低字节的条件、兄弟边事实冲突或缺失、相邻查询预算结果及求解器 unknown。窄位需求不能把部分指针证明冒充全宽证明；拒绝恢复时不发布残留代码或见证。

有限子字段回归覆盖高半部与低半部选择值、可观察的任意载荷、寄存器与帧槽、两种字节序、窄载体、后续值域扩大、互不相交的掩码及缺失事实。缺失的可达目标、无界选择值、共享查询预算耗尽及求解器 unknown 均不得发布残余代码或证明凭据。 测试还覆盖整字需求下的自动发现、未使用的手工提示、根派生载荷和前部常量窗口。

公开工作预算测试覆盖默认值和 32 位最大值、求值与依赖发现各自耗尽、两种源码 ABI 和 C 后端，以及非法 CLI 参数。C/Python 布局检查覆盖 v7、继承字段校验和 v1–v6 对未来尾部的忽略。预算拒绝时不得发布源码和恢复见证；节点求值计数达到最大值时不得回绕。

可选的重复目的地切链测试在同一固定操作预算下比较普通循环与嵌套循环，执行残余循环，区分解码模式，并检查零链长行为不变。关联反例必须在默认策略下成功、启用选项后拒绝发布。重复原生调用／返回槽、依赖重放及后到前驱用两种设置运行。公开标志测试覆盖两种源码 ABI／后端、默认与未知标志、旧 API 忽略扩展以及报告布尔值。

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer` 检查条件约束下寄存器和帧槽指针的合流、两种字节序、模回绕及高根地址、栈恢复和 120 个帧字节。配套反例拒绝损坏的指针，并检查查询、操作、求值和细化预算恰好足够或少一次的情况，以及依赖发现和上下文耗尽。

有限值观察者测试覆盖提前拒绝、常量、空投影及最后一次 UNSAT 查询。不可变读取回归确保遇到反例后保留运行时加载，对不同读取范围重新验证缓存地址域，并在证书畸形时拒绝发布任何部分证据。

仿射控制测试覆盖八次求解预算下的低位条件、两种字节序中的寄存器与帧槽载体、模回绕、帧字节观测和栈恢复。非字面常量形式的矛盾条件必须剪除不支持的分支；低 32 位相同而高位不同的两个根值必须保留各自的间接跳转目标。

`FiniteQueryCache.*` 检查恰好足够与少一个存储单位的边界、命中更新使用顺序、一次淘汰多个不同大小记录、反复淘汰和变量重命名后的查询。重复存入、未命中、格式错误的结果及超大候选均不改变使用顺序。缓存条目淘汰后，已返回的证明副本仍然有效。

`ControlStateRecovery.*Marginal*` 覆盖目标与业务字段的独立值域乘积超过联合上限、残余程序的具体输出、联合关系放宽后由较晚前驱引入新目标、缺失可达目标，以及超限或枚举不完整时丢弃整个值域。这些原创用例验证值域变化会重新调度分析，且失败不会发布部分图。 帧槽变体验证联合关系放宽后两种字节序下的别名失效处理。

`InterpreterTransferChain.*` 检查相关值、唯一及动态分支、晚到前驱、帧边界、别名拒绝和预算。新增原生 CALL/RET 测试检查重复指令、返回槽字节及栈恢复。生产者重放和原生到 LLVM 控制覆盖合同不匹配、凭据变化、错误结果及缺失的栈写入。

`NeverDX86NoIndexAddressTests` 检查 32／64 位地址宽度下无索引的 x86 SIB 寻址：忽略缩放位、目标寄存器位宽、加载／存储、完整未定义输出元数据、段偏移和地址来源。测试拒绝把伪寄存器用作基址或错误位宽的索引，并保留 REX.X 选择的真实 R12 索引。EVEX 广播和掩码移动测试还覆盖这些形式、非活动内存访问抑制，以及不一致的 SIB 元数据。

移位回归覆盖全部八位原始次数、零次移位的标志组合、两种 x86 模式、全部标量位宽、CL 与目标重叠、AH/CH/DH/BH、扩展寄存器和内存。按字节建模的符号执行与逐位算术模型对照，检查已定义结果及守卫触发条件。关系测试检查复制与新生标志、溢出保存、循环重访、未定义值派生次数、分支拒绝、畸形编码及摘要和预算失败。有限不可变读取测试覆盖 1/2/4/8 字节、输入相关选择、路径内单地址集合、完整读取见证和地址上限绑定，并拒绝依赖未定义值、缺失、可写、无文件字节、重定位或无界候选。

核心测试检查上下文拆分、固定点汇合、动态循环、重叠寄存器、别名失效、有限目标派发，以及拒绝时不提供部分替代代码。源码测试汇编原创的寄存器式、栈式和有限地址 x64 机器，恢复两条 C 输出路径，在 O0/O2 下开启未定义行为陷阱编译，并与独立的无符号算术和内存参考实现对照执行。有限地址 fixture 覆盖输入选择的记录和相关游标／key 控制字段；原生检查涵盖 SysV 和 Win64 调用约定。测试还覆盖公开 CLI、恢复预算及不支持输入的报告。需要支持跨目标编译的 Clang 和 LLD；原始 ELF 的执行另需 x64 Linux 主机。工具缺失或主机不匹配属于跳过的覆盖，不代表通过。

`ControlStateRecovery.LongTransparentLoop*` 覆盖独立编写的 20 阶段循环、动态算术参考实现、未知 selector 拒绝和预算耗尽。`LongTransparentPhasesKeepExactBitDemands` 检查 selector 同字节内的无关位仍是可观察的运行时数据，不会成为控制需求。`ProducerClosureChargesWorkBeforeAnotherRestart` 检查反向发现和重放在新图启动前消耗共享预算，且不发布部分结果。

`X86ShiftCarry.*` 用连续单比特移位校验窄位宽算术右移的进位、掩码后的计数，以及 APX 目标寄存器和标志抑制行为。
`NarrowArithmeticShiftCarrySurvivesBothSourceBackends` 在 O0/O2 下启用未定义行为陷阱，执行两条恢复 C 路径，覆盖全部字节值和原始计数。
`NeverDLLVMCIntrinsicSemanticTests` 还在 O0/O2 下执行 i1/8/16/32/64/128 的有符号和无符号整数 min/max，检查赋值和内联结果、操作数生成顺序及单次求值。不支持的标量位宽和畸形操作数必须明确失败。

## 结构化 C 控制流与调用检查

`HighControlFlowSemantics.*` 检查移动循环出口或尾部时是否保留其他跳转仍引用的标签。测试覆盖直接进入循环头部、尾部出口及替换后的 break，并以独立返回值预期执行 O0/O2 编译的生成 C。

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` 检查推断出的必需寄存器参数是否保留末尾未知槽位。读取未知的必需参数或条件必须明确触发陷阱；省略、空指针和嵌套操作数不能被悄悄替换为零。已知值和已证明不被读取的多余操作数仍可执行。陷阱是诊断边界，不是恢复行为等价的证明。

## CPU 执行测试

`NeverDIntegerABITests` 为 Windows x64、Linux x64 和 Linux ARM64 构建原始 Clang fixture，通过真实的十参数函数检查寄存器／栈参数与调用帧。Unicorn/KVM/WHP 矩阵会明确跳过不可用的主机／ISA 组合；跳过不代表通过。`NeverDExecutionBudgetTests` 不依赖定时 sleep，检查共享续接预算、预留失败和绝对 deadline。

`NeverDCPUEmulationTests` 覆盖 ARM64 指令、控制流、加载、CPU 上下文、别名、缓存失效和有界循环；软件配置还执行 FP/SIMD 与 TLS。`NeverDUserExecutionTests` 检查 CPL3/EL0 页权限、别名、保护故障、上下文和地址空间切换。`NeverDServiceRequestTests` 验证 SYSCALL/SVC 在进入传输前被拦截、保留状态并恰好消费一次请求；这是交接协议，不代表完整 OS 服务实现。`NeverDExecutionConfigurationTests` 验证工厂与报告共用配置解析、区分构建支持与实时探测，并在修改前拒绝不支持的要求。公开 SDK/CLI 测试不需要 Windows 模型。`NeverDThreadPointerTests` 检查 FS 基址、`TPIDR_EL0`、上下文恢复和权限。`NeverDKvmCancellationTests` 使用不会退出的 x64 来宾验证活动 KVM 中断、恢复及调用方信号状态不变；没有 KVM 时明确跳过。

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

缺少 ARM64 硬件或 hypervisor 属于原生覆盖被跳过，不是通过。Unicorn 和交叉编译不能证明原生 KVM/WHP 执行。

`NeverDParallelExecutionTests` 强制制造处理器调用重叠、写入等待期间取消，并验证独立 CPU 状态、物理别名原子竞争和私有传输暂存。`NeverDRunControlTests` 检查独立 WHP binding 可同时持有两份资源租约，而共享 binding 串行化访问。`NeverDMMIOAtomicTests` 比较原始 x64 原子／更新指令及全部 ARM64 LSE 用例的设备与 RAM 结果，覆盖宽写入的两次观察、过期预览、提供方失败及提交／停止竞争。`KernelMMIOFailure` 覆盖别名、同值写入、重复提交、电源变化、解除映射与所有者销毁。不可用平台明确跳过；包装层会合只证明处理器调用可并行，不证明硬件同时退休指令。ARM64 KVM/WHP 原生验证仍需对应主机。

```bash
cmake --build build-cpu --target NeverDParallelExecutionTests NeverDMMIOAtomicTests NeverDRunControlTests --parallel 4
ctest --test-dir build-cpu/unittests/emulation -L '^NeverD(ParallelExecution|MMIOAtomic|RunControl)Tests$' --output-on-failure
```

## Linux 进程配置测试

独立的[进程测试套件](process-emulation.md#验证)编译真实 x64/AArch64 ELF fixture。`NeverDLinuxProcessTests` 检查启动、program-header 策略、服务续接、二进制输出、来宾故障和资源停止。`NeverDProcessPublicTests` 通过 C API/CLI 验证且不修改分析映像。`NeverDExecutionSessionTests` 检查两个 CPU 共享内存／预算以及请求／故障恰好消费一次。`NeverDX64MemoryUpdateTests` 检查内存算术、SETcc、BT、XMM/MXCSR、写入观察器、REP 边界和预备设备读取。`DriverBackendParityTests.cpp` 运行原始与重定位 WDK fixture，并将完整可观察报告与 Unicorn 比较；缺失镜像／后端会明确跳过。

checked x64 还支持带屏蔽的传统 `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN` 和 `MAX` 的 `SS`、`SD`、`PS`、`PD` 形式。`X64SSEInstructions.def` 统一定义操作数宽度、对齐和准入规则。`MaskedSSEArithmeticMatchesIndependentHostExecution` 使用独立的本机 CPU 参照验证寄存器与 RAM 形式，覆盖四种舍入模式、FTZ、有符号零、次正规输入和 NaN；`SSEMemoryObserverStopsBeforeResultAndStatusChanges` 验证停止请求发生在效果提交之前。这不开放 未屏蔽异常、x87 或 AVX。

`X64PackedIntegerTests.cpp` 使用 `X64PackedIntegerCases.def` 中的原始编码和 180 组固定向量结果，并与原生 x64 编译器 intrinsic 独立核对。寄存器及页尾别名 RAM 用例保留其他 XMM、整数哨兵值、FLAGS、MXCSR 和源字节。观察器停止／失败与可恢复读取故障保留状态，修复后可重试一次。未对齐触发 `#GP(0)`；MMX、LOCK 和 MMIO 仍在回调前拒绝。原生 CI 强制执行 WHP 的两个特权级。

`X64PackedShiftTests.cpp` 与原始 `X64PackedShiftCases.def` 使用 16 个立即数计数和 21 个变量计数，将十种移位与独立标量计算、宿主 SSE2 intrinsic 对照。覆盖计数／目标别名、高位忽略、对齐、观察器、可恢复故障和设备拒绝。`X64VectorTestSupport.h` 与打包算术测试共用寄存器与 RAM 断言。原生 CI 强制执行 WHP 的两个特权级。

`X64VectorMaskTests.cpp` 将独立原始编码与标量位提取及原生 SSE intrinsic 比较，覆盖每个源位、全部 16 个 GPR × 16 个 XMM 组合和两种 REX.W 值。完整公共寄存器快照、RAM 与数据观察器验证零扩展和状态保留；指令停止、回调失败及不支持的形式不得发布副作用。KVM/WHP 原生验收要求两个特权级均执行。

`X64ShuffleTests.cpp` 使用 `X64ShuffleCases.def` 中的独立编码，并以原生 intrinsic 校验标量通道选择结果。覆盖全部 256 个控制值、寄存器与自身源、页尾内存别名、全部 XMM 寄存器配对、完整公开 CPU 状态和 RAM、观察回调停止与异常、权限、对齐故障和重试。MMX、VEX/EVEX、LOCK 与设备操作数必须无副作用地拒绝。原生 KVM/WHP 验收要求两个特权级的这些用例全部通过；不可用的宿主/ISA 组合仍明确跳过。 `UNPCKLPS`、`UNPCKHPS`、`UNPCKLPD` 和 `UNPCKHPD` 复用同一状态/故障矩阵及独立标量、原生对照，并检查内存源配合每个 XMM 目标寄存器。

`X64PartialMoveTests.cpp` 与 `X64PartialMoveCases.def` 对照独立的标量/原生加载存储参考结果，覆盖全部 16 个 XMM 寄存器以及 NaN/次正规数的原始位。非对齐访问、别名、跨页故障、权限修复、观察者停止/失败和重试均检查完整 CPU 状态及两页 RAM。页末操作数只需八字节，存储无需读权限。寄存器别名、拒绝形式和设备回调单独验证；原生 KVM/WHP 在两个权限级别均为必测。

`X64IntegerFloatTests.cpp` 使用独立的 `X64IntegerFloatCases.def` 编码、`APFloat` 期望值，以及保存/恢复浮点状态后执行的原生指令。覆盖两种整数宽度、四种舍入模式、精度粘滞状态、FTZ、所有 GPR/XMM 组合及完整 CPU/RAM 保留。非对齐、跨页、页末源、权限修复、观察者停止/失败和重试均检查精确范围。原生 KVM/WHP 在两个权限级别均为必测。

`X64FloatIntegerTests.cpp` 使用独立的 `X64FloatIntegerCases.def` 编码、`APFloat` 和原生寄存器/内存指令，检查舍入及截断转换。两种整数宽度均覆盖有符号边界、中点值、NaN、无穷、次正规数、全部舍入模式、粘滞状态和 FTZ。检查所有 GPR/XMM 组合、完整 CPU/RAM 状态、精确页末读取、可恢复跨页故障及观察者取消/重试。两个权限级别的原生 KVM/WHP 结果均为必测。

`X64SSEComparisonTests.cpp` 使用独立的 `X64SSEComparisonCases.def` 编码、`APFloat` 排序及保存/恢复宿主 FLAGS 和浮点状态的原生指令。21 种原始输入的全部配对覆盖 NaN/次正规优先级、零、无穷及相邻值。检查所有 XMM 配对与别名、MXCSR 粘滞位、舍入无关性、DF 保留、完整 CPU/RAM 状态、精确页末/跨页读取及观察者取消/重试。两个权限级别的原生 KVM/WHP 结果均为必测。

`X64SSEPredicateTests.cpp` 使用 `X64SSEPredicateCases.def` 中的独立条件、`X64SSEComparisonCases.def` 中共享的原始输入、`APFloat` 排序和原生指令对照。测试覆盖全部输入对、混合通道异常优先级、标量高位保留、XMM 别名、完整 CPU/RAM 状态、页尾/跨页读取、对齐优先级及观察者/故障重试。直接 Capstone 检查覆盖全部控制字节、两种语法、两种解码 API 和 32/64 位模式。两个权限层级的原生 KVM/WHP 结果均为必测；保留控制值、VEX/EVEX 和设备操作数仍排除。 数值矩阵按指令及比较条件拆分，舍入与控制矩阵按指令拆分。`NativeCPUTests.def` 仍要求所有原始组合；来宾时限和 15 秒 CTest 时限保持不变。编译期检查要求每个指令/条件组合恰好出现一次。

`X64SSEPrecisionTests.cpp` 结合独立的 `X64SSEPrecisionCases.def` 编码、`APFloat` 精度舍入和原生指令对照。范围检查使用无界指数下的舍入，覆盖定向舍入到有限值的溢出及舍入到正规数的微小结果。测试涵盖正负号、NaN 载荷、全部舍入/FTZ/粘滞状态、打包通道聚合、XMM 别名、完整 CPU/RAM 状态、精确源宽度、对齐优先级、页故障和观察者取消/重试。两个权限层级的 KVM/WHP 用例均为必测。

`X64PackedFloatTests.cpp` 使用独立的 `X64PackedFloatCases.def` 编码、有符号 `APFloat` 期望值及原生指令对照。测试涵盖全部输入对、整数边界、精度中点、舍入模式、粘滞状态与 FTZ，并在两个权限层级检查寄存器别名、所有 XMM 组合、完整 CPU/RAM、每种 m64 跨页位置、精确页尾、对齐优先级及观察者/故障重试。全部 KVM/WHP 用例均为必测。

`X64PackedFloatIntegerTests.cpp` 结合独立的 `X64PackedFloatIntegerCases.def` 编码、共享 `X64FloatIntegerCases.def` 输入、`APFloat`/`APSInt` 及原生指令。43 个原始输入的全部组合覆盖 signed32 边界、相邻舍入中点、NaN、无穷和次正规数；独立通道矩阵分别变化精确、非精确、无效及次正规输入。测试涵盖全部舍入/FTZ/粘滞设置、XMM 别名、完整 CPU/RAM、对齐页尾、对齐优先级与观察者/故障重试。两个权限层级的 KVM/WHP 均为必测。

`DAZBackends` 为比较、谓词、标量整数/浮点、打包整数/浮点及精度转换矩阵增加开启 DAZ 的覆盖。`X64DAZTestSupport.h` 使用独立的 `APFloat` 输入归一化，并在运行原生对照指令前检查宿主的 `MXCSR_MASK`。测试覆盖带符号零、次正规数、NaN、混合通道、全部舍入模式、FTZ 和粘滞状态，同时检查源字节、无关寄存器、FLAGS 及完整 RAM 保持不变。寄存器、别名和页边界操作数保留原有的访问观察检查。KVM/WHP 要求两个特权级的全部 DAZ 用例及 17 个原始宿主指令对照用例均通过；可移植运行中不支持的宿主会明确跳过。原有关闭 DAZ 的用例和超时限制保持不变。

`X64AlignmentTests.cpp` 验证已准入 aligned SSE 指令的未对齐操作数在数据观察器、权限检查或设备回调之前报告可恢复或终止性的 `#GP(0)`。故障保留完整公开 x64 寄存器上下文、PC 和 RAM；地址宽度回绕先于 FS/GS 基址相加，修复地址后重试原指令。直接 KVM/WHP 机器测试独立验证硬件边界。Windows ring3 已派发明确分类的 `operand_alignment` 故障；其他原因的 `#GP` 仍不支持。

`X64SIMDExceptionTests.cpp` 绕过 checked 准入层，在两个特权级验证 KVM/WHP 的原生 `#XM` 传递。`X64SIMDExceptionCases.def` 中八个原始用例覆盖六类异常，包括精确的次正规结果和无界指数下精度精确的溢出。寄存器及 RAM 形式发生异常时，除规定的 MXCSR 状态外，完整 GPR、XMM、x87、FLAGS、FS/GS 和来宾内存均保持不变。屏蔽异常后重试原指令；保留粘滞状态并修复操作数，验证旧标志不会再次触发异常。 公共 checked 和驱动契约也执行这些原始故障及重试用例。`WindowsSIMDExecutionTests.cpp` 执行真实原生异常、来宾 VEH/VCH 指令，以及跳过、屏蔽后重试或修复操作数后继续，分别检查处理器入口控制状态和保存的上下文。启动反例会拒绝缺失异常、错误向量及目标寄存器遭修改的结果，不发布能力。

`check_windows_simd.py` 根据 `WindowsSIMDCases.def` 和标量用例清单构建独立的原创 Windows x64 程序。6,144 组观察覆盖寄存器/RAM 操作数、全部异常屏蔽组合、清零或全部置位的粘滞状态，以及跳过、屏蔽后重试、修复操作数且保持屏蔽位不变后重试三条路径。汇编入口分别记录 VEH/VCH 的实时 MXCSR、x87 控制状态和保存的 `CONTEXT`。测试在恢复宿主状态前检查精确故障 PC、状态保持、修复后的上下文和重试结果；CI 保留原始记录和源码哈希。`--build-only` 仅证明编译成功。这些观察不启用 checked 模式下未屏蔽的 SIMD，也不代表 ARM64 原生执行。

`WindowsSIMDStatusCases.def` 固化了原生 Windows 上观察到的全部 63 种非空有效状态组合。`WindowsSIMDMappingTests.cpp` 检查精确代码与参数、拒绝不一致的故障和无效控制值，并通过注入故障边界检查异常记录、CONTEXT 中的两份控制状态及屏蔽异常后的继续执行。Windows CI 中的原创程序独立验证这些固定结果。注入测试不证明 Unicorn 能产生该异常，也不启用 checked 模式下未屏蔽的 SIMD。

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

不可用后端会明确跳过。交叉编译与 Unicorn ARM64 不构成原生 KVM/WHP 证据。

## 驱动模拟检查

同时启用 `NEVERD_ENABLE_DRIVER_EMULATION=ON` 与 `BUILD_TESTING=ON`，即可构建专项执行套件及共享 C API／CLI 检查：

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

fixture 覆盖来宾初始化、成功与失败返回、不支持的行为、内存故障、严格场景解析、有界执行，以及经过 create、传输、cleanup、close 和 unload 的同步 buffered／direct I/O、READ/WRITE、独立文件生命周期、MDL 权限、动态导出解析、来宾变参和结构化 CPU 故障。使用 [`emulate-driver` CLI](driver-emulation.md) 验证 JSON 与进程退出码。生产构建可在 `BUILD_TESTING=OFF` 时启用此功能；`libneverd` 不得依赖仅供测试使用的 Unicorn 配置。

补充测试覆盖驱动独立拥有的非分页池 MDL、描述符与缓冲区的独立生命周期、注册表查询布局与短缓冲区、句柄权限、删除和泄漏，以及无输出 IOCTL 的完整 64 位 `information_hex`。真实样本验收还包括 Zero 的同步直接读写和统计查询。

后端测试验证完整 CPU 上下文（寄存器、标志、SIMD、FPU、CR8）、共享内存及跨后端／故障上下文拒绝。编译后的 `driver_dispatcher.c` 样例实际执行 DPC 与工作项回调，覆盖定时器边界、通知／同步事件与定时器、原因 `Executive` 的非警报 `KernelMode` 等待、超时／延迟、多个阻塞栈、置位后重置仍保留唤醒、回调参数及非法 IRQL／生命周期。工作项测试继续覆盖待处理／完成、队列、停滞和共享预算。这些用例证明所述子集，不代表完整 Windows 异步支持。

`DriverThreadPriorityTests.cpp` 使用原创编译驱动 `driver_thread_priority.c`，在显式 Unicorn／KVM／WHP 的 driver 和 checked 契约下验证排队及阻塞线程的优先级修改、时间片内事件／计时器唤醒、同级轮转、DISPATCH_LEVEL 屏蔽，以及低优先级线程饥饿时计时器仍推进。成对计数循环对照证明抢占后保留准确的剩余时间片。模型测试覆盖有符号 ABI 参数、失败不改变状态、已退出对象引用、嵌套线程身份和独立回调栈复用。原生用例纳入 `NativeDriverTests.def` 强制清单；本地不可用后端明确跳过。

`DriverMutexThreadTests.cpp` 执行 `driver_seh_mutex.def` 中四种原创 WDK 模式：在 SEH 过滤器中递归获取、过滤器或异常 finally 获取后保留所有权，以及过滤器阻塞并在另一系统线程释放 mutex 后恢复。Unicorn／KVM／WHP 的 driver 和 checked 契约覆盖普通／有效 CFG 映像、首选／重定位地址及协作式／1／17 指令时间片。模型测试还验证嵌套栈退役后的 APC 禁用、错误线程释放和最外层返回检查；KVM／WHP 用例纳入 `NativeDriverTests.def` 强制清单。

`KernelWaitSetTests.cpp` 在不依赖 Unicorn 的情况下运行十六项模型测试：部分 `WaitAll`、首个就绪 `WaitAny`、索引快照、超时清理、后续非法对象／存储、64 对象边界、IRQL、已退出线程保留及两个同步定时器。`DriverMultipleWaitTests.cpp` 执行 `driver_wdm_multiple_wait.c` 和 `DriverMultipleWaitCases.def` 的七种原创 WDK 模式，覆盖 Unicorn/KVM/WHP、两种驱动契约、普通／CFG 映像、重定位及协作式／1／17 指令时间片。30 项模型／原生结果纳入 `NativeDriverTests.def` 强制验收。 回归还覆盖成功或超时后的重复完成、捕获状态被改动以及延时等待的重复完成。

`KernelMultipleWait.DispatcherScalarParametersIgnoreUpperRegisterBits` 检查高位污染、有符号边界、溢出及对象状态不变性。`DriverMultipleWaitCases.def` 中的裸尾调用包装器让 `driver_wdm_multiple_wait.c` 经真实 WDK 导入执行同样的合法 ABI 调用，并覆盖启用 CFG、重定位及按指令抢占。

`driver_context_limits.c`: API 的 IRQL 上限来自 `KernelAPIIRQL.def`，参数相关限制由所属模型检查。DPC 不能调用注册表 API，也不能分配、释放或访问分页池；Unicode `DbgPrint` 转换要求 `PASSIVE_LEVEL`，支持的 ANSI 输出和非分页操作仍可在 `DISPATCH_LEVEL` 使用。回调栈有明确边界，越界栈指针不能进入另一阻塞工作项的栈。设备扩展中的已启动定时器会阻止设备提前回收。这些检查并未开放通用 IRQL 切换。

`KernelDeviceStackTests.cpp` 检查独立的所有者／附着关系、栈顶选择、失败原子性、栈容量、不透明字段、打开句柄计数、拆链／删除时的工作项及请求保活，以及文件身份与派发栈顶的区别。原创 `driver_wdm_stack.c` 使用真实 WDK 头文件和内联 Copy/Skip/SetCompletion；普通／启用 CFG 映像由可选 `NEVERD_WDM_STACK_FIXTURE`／`NEVERD_WDM_STACK_CFG_FIXTURE` 配置。`DriverWDMStackTests.cpp` 覆盖重定位、真实下层状态、完成顺序和标志、延迟 pending 传播、工作项／DPC、等待、`STATUS_MORE_PROCESSING_REQUIRED`、直接 MDL 保留、嵌套完成及畸形游标／控制值。`DriverScenarioPublicTests.cpp` 覆盖 C API／CLI 转发及 C API 保留／嵌套完成，包括已配置 CFG 映像。缺少产物会明确跳过；Linux 证据仅证明同驱动设备栈子集，不代表 PDO／PnP／电源支持。 `KernelIRPStackTests.cpp` 检查计数游标、完整内联 Copy 前缀、已消耗栈位置清零、状态／pending 传播、MPR 与嵌套完成、续接所有者检查及保留路径。真实 READ/WRITE 与文件生命周期也使用内联 Copy 验证。

`DriverPnpScenarioTests.cpp` 检查 JSON／原生预检一致、显式初始事实、ID／数量限制、字段互斥、最终总线状态及可空观测报告。`KernelPnpDeviceTests.cpp`、`KernelPnpRequestTests.cpp` 和 `KernelPnpCompletionTests.cpp` 检查提供者所有权、AddDevice 成功／失败／泄漏、初始 IRP、文件准入、生命周期回滚、延迟完成、MPR／嵌套／等待续接及失败原子性。原创真实 WDK 样例 `driver_wdm_pnp.c` 使用可选 `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE`。`DriverWDMPnpTests.cpp` 验证普通／启用 CFG 且重定位的 AddDevice、文件 I/O、有序移除、延迟启动／移除、启动／query 失败及干净／泄漏的 AddDevice 失败。缺少产物会明确跳过。执行证据仅来自 Linux，只证明所述无资源 PnP 子集。 `DriverScenarioPublicTests.cpp` 还通过 C API 和 CLI 验证普通／启用 CFG 映像的七请求延迟 PnP 报告。

V9 schema 测试往返验证八种次功能名称，并与生命周期完成共享最终状态校验；QueryStop 0x119 在加载映像前拒绝。扩展模型及真实样例检查 query-stop 回滚、cancel-stop、停止／重启、突然移除、精确成功失败边界、停止／待移除状态的软件 I/O、突然移除后的来宾拒绝、设备身份及混合 AddDevice 结果。`DriverScenarioPublicTests.cpp` 通过 C API 和 CLI，在普通／启用 CFG 样例上运行 16 请求的停止／重启／突然移除序列，保留成功软件 IOCTL 字节、来宾拒绝的 IOCTL 和最终 cleanup/close/remove。公开执行为串行：保留 IRP 若无当前可用生产者，无法等待后续场景请求来启动或清理设备。Remove 排空要求是配置边界，不是通用 Windows I/O 准入策略。证据仍仅来自 Linux。

`KernelRemoveLocksTests.cpp` 检查独立锁／设备身份、NULL／重复 Tag、retail／DBG 大小、立即／延迟排空、失败获取义务、失败原子性、容量及退休。`KernelRemoveLockBridgeTests.cpp` 检查附加前初始化、扩展范围、不透明存储、IRQL 和修改前拒绝不安全 Delete／Detach。真实 `driver_wdm_remove_lock.c` 的 retail／DBG、普通／active-CFG 四种样例通过 `NEVERD_WDM_REMOVE_LOCK_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` 指定。`DriverWDMRemoveLockTests.cpp` 覆盖包退休后释放、锁排空后总线才完成、最后 release 先于回调返回唤醒、工作项等待及干净 AddDevice 失败。C API／CLI 使用既有 PnP 场景，保留总线接收／完成与最终拆除观测。缺少产物明确跳过；Linux 证据不代表完整 Driver Verifier 或通用并发排空。

`DriverPowerScenarioTests.cpp` 验证必填电源事实、JSON／原生一致性、不透明32位 context、响应 FIFO 限制和独立子报告。`KernelPowerRequestTests.cpp` 与 `KernelPowerCompletionTests.cpp` 检查包布局、路径标志、生命周期与设备通知的区别、FIFO 匹配、最终回调所有权、MPR、等待及释放边界。真实 WDK 原始样例 `driver_wdm_power.c` 使用可选 `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE`；`DriverWDMPowerTests.cpp` 覆盖普通／active-CFG 重定位、直接及嵌套 Query/Set、独立延迟完成、S0 先于 D0、跨等待五参数回调快照、工作项来源子请求、空回调、query 拒绝、独立 PDO 初值／FIFO 和缺失事实错误。`DriverScenarioPublicTests.cpp` 增加格式错误预检及 C API／CLI 的六场景请求／三子请求睡眠唤醒序列。缺少真实产物明确跳过；执行证据仅来自 Linux，只证明文档中的可分页无资源电源子集。

`KernelUsbIdleTests.cpp` 验证协议所有权、精确回调／D2 身份、借用及首个完成原因；`KernelUsbIdleBridgeTests.cpp`／`KernelUsbIdleReceiptTests.cpp` 覆盖真实 IRP、非法准入、排队取消、组合容量及嵌套收到／完成时序，`DriverUsbIdleScenarioTests.cpp` 检查原生／JSON 一致和报告证据。真实 `driver_wdm_usb_idle.c` 使用 `NEVERD_WDM_USB_IDLE_FIXTURE`／`NEVERD_WDM_USB_IDLE_CFG_FIXTURE`；`DriverWdmUsbIdleTests.cpp` 覆盖 idle、取消、D0／D3、真实唤醒、重发重启、独立／组合成员及直达 PDO／FDO 转发和分配方栈。`DriverWdmUsbIdlePublicTests.cpp` 通过 C API／CLI、普通／active-CFG、首选／重定位执行 [USB 场景](../examples/driver-wdm-usb-idle-scenario.json)。缺少产物明确跳过，证据仍仅限 Linux，不代表 KMDF USB 选择性挂起已实现。

`KernelFrameworkUsbIdleTests.cpp`、`KernelFrameworkUsbIdleStorageTests.cpp`、`KernelFrameworkUsbIdleBridgeTests.cpp` 验证策略、真实存储与类型化调度。真实 `driver_kmdf_usb_idle.c` 不自行发出 USB idle 包。`DriverKMDFUsbIdleTests.cpp` 覆盖无许可、受管 I/O 的延迟 D2／D0、回调前／中 StopIdle、arm 失败、显式 Maximum 能力、远程唤醒及组合成员。`DriverKMDFUsbIdlePublicTests.cpp` 通过 C API／CLI、普通／active-CFG、首选／重定位执行 [KMDF USB 场景](../examples/driver-kmdf-usb-idle-scenario.json)，使用 `NEVERD_KMDF_USB_IDLE_FIXTURE`／`NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`。缺少产物明确跳过，执行证据仅限 Linux。 模型测试还验证：唤醒 arm 回调成功后分配耗尽，仍执行真实 disarm 回调、取消 WAIT_WAKE，且不消费 D2 响应。

`DriverKMDFUsbPoFxTests.cpp` 覆盖首次 SystemManaged／WithHint 配置、独立 PoFx／USB 许可、D0 取消、延迟 D2／D0 与真实 worker 延迟 F0 确认、活动及 StopIdle、READ 前唤醒恢复、arm 失败、移除与重启。`DriverKMDFUsbPoFxPublicTests.cpp` 通过 C API／CLI、两种服务模式、普通／active-CFG、首选／重定位运行 [USB PoFx 场景](../examples/driver-kmdf-usb-pofx-scenario.json)。`DriverKMDFUsbIdleTests.cpp` 及公共测试保留原转发回归，新增直接 READ，验证不进入路由回调且 D0Entry 后投递。`KernelFrameworkRequestTests.cpp` 独立覆盖映射、caller-context 所有权、手动／停止队列与 IRQL。分配失败及独立门控由模型 USB／PoFx 桥测试证明，真实样本不耗尽 arena。外部样本缺失明确跳过，执行证据仍仅限 Linux。 `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` 另覆盖 RemovePending 期间真实 D0Entry 失败：精确 Required 确认与 quiesce 允许失败 IRP／硬件清理，不触发 F0／ActiveCondition；不证明普通在场设备 SET_POWER 失败或框架自主意外移除。

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: [原生 WAIT_WAKE 场景](../examples/driver-wdm-wait-wake-scenario.json) 使用真实 WDK `driver_wdm_wait_wake.c`，通过 `NEVERD_WDM_WAIT_WAKE_FIXTURE`／`NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE` 运行。测试覆盖实际 START 中提交、回调参数、唤醒不自动 D0、重发、取消、MPR、DPC 取消后工作线程提交 D0、精确事件捕获和独立提供方。普通／active-CFG 及首选／重定位地址执行证据仅来自 Linux；缺少文件明确跳过。

`KernelPowerCompletionTests.cpp` 覆盖 APC／DPC 准入、容量耗尽的原子重试、仅提供方同步／延迟有／无回调、捕获路径及 MPR。真实 `DriverWdmWaitWakeTests.cpp` 检查 DPC 取消回调直接请求 D0、独立 APC／DPC Query/Set、IRQL／CR8 不变、调用返回早于 PASSIVE 派发及完成，以及提升 IRQL 的 WAIT_WAKE 仍被拒绝。[提升 IRQL 场景](../examples/driver-wdm-elevated-power-scenario.json) 经 `DriverWdmWaitWakePublicTests.cpp` 覆盖 C API／CLI、普通／active-CFG 及首选／重定位；执行证据仍仅限 Linux。

`DriverResourceScenarioTests.cpp` 检查显式 JSON／原生事实、整数宽度、数量、物理／寄存器区间重叠、对齐、ID、空银行及配置序列化。`KernelMMIOTests.cpp`、`KernelMMIOFailureTests.cpp`、`KernelResourceBridgeTests.cpp` 与 `UnicornMMIOTests.cpp` 覆盖银行／映射所有权、别名、资源身份、紧凑清单生命周期、提供者时序、重启后的值保留、突然移除／电源可访问性、精确 CPU／API 事务及失败原子性。原创真实 WDK `driver_wdm_resources.c` 使用 `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`；`DriverWDMResourceTests.cpp` 执行真实标量及 REP 访问函数、普通／active-CFG 重定位、子区间别名、页尾映射、STOP／重启及非法访问。C API／CLI 测试在加载映像前拒绝无效事实，并执行相同的 14 请求重启场景，核对持久 IOCTL 输出及精确映射／取消映射次数。共享 [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) 需要此样例的寄存器／IOCTL 协议。缺少产物明确跳过，证据仅来自 Linux；测试不访问宿主物理内存，也不覆盖通用设备后端。

`DriverDMAScenarioTests.cpp` 验证显式能力、逻辑地址域、字节／数量／时间上限、严格事件方向和独立配置／观测。`KernelPhysicalMemoryTests.cpp` 与 `BackendBackingTests.cpp` 检查同页分配边界、固定引用、CPU 权限不变、MMIO／重入排除和整区间失败原子性；`KernelRequestMDLTests.cpp` 检查已构建描述符的别名及模型只读 PFN 与同一物理身份一致。`KernelDMATests.cpp`、`KernelDMABridgeTests.cpp` 和 `SchedulerDMATests.cpp` 覆盖实际 RAM 字节、适配器绑定的表调用、内嵌／排队 FIFO 所有权、独立回调／映射寿命、页片段、错误方向、释放预检、独立 PDO 地址域及资源代次／电源失败。原创真实 WDK `driver_wdm_dma.c` 使用 `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE`；`DriverWDMDMATests.cpp` 及 C API／CLI 覆盖真实适配器指针、公共／SG 存储和分别配置的 DMA／中断事件。共享 [driver-dma-scenario.json](../examples/driver-dma-scenario.json)要求该 fixture 的协议。缺少产物明确跳过；执行证据仅限 Linux，不代表宿主 DMA、PCI 或通用设备引擎。 `pluginsdk/python/tests/test_driver_dma_integration.py` 使用 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_FIXTURE` 和 `NEVERD_TEST_WDM_DMA_CFG_FIXTURE` 执行现有带所有权管理的 JSON 绑定，覆盖实际字节、回调顺序和报告中的失败。

`KernelSEHTests.cpp` 检查纯展开计划、作用域顺序、非易失 GPR 恢复、有界栈及明确不支持的元数据；`KernelExceptionTests.cpp` 检查确切 API 参数个数、低 32 位状态、类型化异常、IRQL 上限及模型／CPU 状态不变。真实 WDK `/GS-` `driver_wdm_seh.c` 使用可选 `NEVERD_WDM_SEH_FIXTURE`／`NEVERD_WDM_SEH_CFG_FIXTURE`；`DriverWDMSEHTests.cpp` 执行普通／活动 CFG／重定位镜像，覆盖直接及辅助函数抛出、嵌套处理器、再次抛出、真实过滤器、展开 finally、搜索顺序、稳定异常记录、受支持的 CPU 故障续接及完整 CPU 状态恢复；嵌套过滤器与冲突 finally 使用关联的逻辑栈，其他 CPU 故障仍明确拒绝。C API／CLI 执行 [driver-seh-scenario.json](../examples/driver-seh-scenario.json)，验证 null API 结果及真实来宾处理器消息。`pluginsdk/python/tests/test_driver_seh_integration.py` 使用 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_SEH_FIXTURE` 和 `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`。缺少外部镜像明确跳过；证据仍限 Linux，不代表已支持用户缓冲区或通用 SEH。

`KernelDMAChannelTests.cpp`、`KernelDMAChannelBridgeTests.cpp` 和共用 `SchedulerDMATests.cpp` 检查混合分配 FIFO、回调返回宽度、纯接纳／释放预检、寄存器复用、连续页片段、整次操作刷新、CurrentIrp 快照及包／MDL／设备生命周期。原创真实 WDK `driver_wdm_dma_channel.c` 使用可选 `NEVERD_WDM_DMA_CHANNEL_FIXTURE`／`NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`；`DriverWDMDMAChannelTests.cpp` 执行普通／活动 CFG／重定位驱动，覆盖真实 MapTransfer 与 FlushAdapterBuffers 调用、公共／SG／通道共享额度、显式设备事务、IRQ/DPC 完成、连续操作、两个 PDO 及失败案例。C API／CLI 运行七请求 [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json)，包括一个跨越两个已映射片段的单独事务。`pluginsdk/python/tests/test_driver_dma_channel_integration.py` 使用 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` 和 `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` 验证同一公开 JSON 接口。缺少产物明确跳过；Linux 证据不代表已支持系统 DMA 控制器或任意 HAL 映射／刷新模式。

`DriverInterruptScenarioTests.cpp` 覆盖显式原始／转换后描述符、混合及纯中断分配、严格事件字段／数量、源身份及独立 BOOLEAN 观测。`KernelInterruptsTests.cpp`、`KernelInterruptBridgeTests.cpp` 与 `SchedulerInterruptTests.cpp` 覆盖独占元组匹配、不透明令牌、资源代次／连接捕获、事件生命周期、精确选定的 Ex 字段、共用锁和 IRQL 恢复、回调所有权、同一时刻 ISR 优先级及修改前的容量失败。`KernelFrameworkRequestTests.cpp` 检查纯取消预览和批量令牌容量，不发布回调或消耗引用。原创真实 WDK `driver_wdm_interrupts.c` 使用 `NEVERD_WDM_INTERRUPT_FIXTURE`／`NEVERD_WDM_INTERRUPT_CFG_FIXTURE`；`DriverWDMInterruptTests.cpp` 执行普通／active-CFG 重定位、传统十一参数 ABI、Ex 版本 1／2／4、真实 ISR→DPC 完成、低位 AL 的 FALSE、同步／手动锁、独立 PDO、重启资源代次及非法硬件事实。C API／CLI 测试在加载映像前拒绝非法声明，并执行七请求的 [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json)，检查 pending IOCTL 字节和独立递送观测。缺少映像明确跳过，执行证据仅来自 Linux，不代表支持共享／电平／MSI 中断或指令级抢占。

`DriverGuardTests.cpp` 与四个原创 `driver_guard.c` 变体覆盖启用／未启用的 CFG、重定位、检查／分派 ABI 和畸形目标。`KernelFrameworkTests.cpp`、`KernelFrameworkControlTests.cpp`、`KernelFrameworkQueueTests.cpp` 和 `KernelFrameworkRequestTests.cpp` 覆盖绑定、可回滚的设备创建、队列路由、缓冲区逻辑长度，以及清理顺序与 IRP／上下文生命周期。原创 `driver_kmdf_lifecycle.c` 与 `driver_kmdf_control.c` 可选用真实 WDK 1.33 头文件编译，并通过真正的 `FxDriverEntry` 库链接。将 CMake 缓存路径 `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` 指向生命周期映像，将 `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` 指向普通／启用 CFG 的控制设备映像。缺少外部产物时会明确跳过。`DriverKMDFLifecycleTests.cpp`、`DriverKMDFControlTests.cpp` 及 `DriverScenarioPublicTests.cpp` 中的 C API／CLI 用例覆盖实际回调、缓冲／直接 I/O、工作项完成待处理请求、失败状态、卸载和重定位后的 CFG 执行。验证证据仍限于 Linux，不代表完整 KMDF 或 PnP／电源管理支持。

旧版取消测试将 API 续接保留到取消、嵌套清理与最终销毁结束；对于已经取消的请求，Ex 仍返回取消状态而不递送回调。`KernelFrameworkRequestAccessorTests.cpp` 与 `KernelRequestMDLTests.cpp` 覆盖共享的 64 位 Information、完成时长度验证、来源队列／IRP 身份、NULL WDF 文件句柄、保留句柄的 getter 结果、缓冲 MDL 缓存与首个方向的 ByteCount、直接描述符身份与延后映射、完成时回收，以及拒绝绕过 WDF 完成流程。真实控制设备 fixture 的 L、M、D、C 模式在普通／启用 CFG 映像中分别执行旧版取消、缓冲 MDL／信息、直接 READ／WRITE MDL 和完成后的访问。

取消测试覆盖仅允许传输请求配置的虚拟期限及报告字段、完成优先与已取消路径、标记／解除标记结果、排队与已递送回调的完成权限、回调等待及内部引用生命周期。调度器测试独立验证 DPC／取消／工作项顺序、容量、身份隔离和暂停／恢复。WDM 取消仍明确报告模型错误。


## 测试布局

`add_neverd_unittest` 创建一个 GoogleTest 可执行文件，并为每个发现的用例分配
与该可执行目标同名的 CTest 标签。

| 源码区域 | 目标与 CTest 标签 | 覆盖内容 |
|----------|-------------------|----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | 跨平台子进程调用、引号、重定向与退出码 |
| `unittests/libc` | `NeverDLibCTests` | 已知 libc 名称与分类 |
| `unittests/safety` | `NeverDSafetyTests`、`NeverDSafetyIntegrationTests` | 汇目录、身份优先序、参数预过滤、拷贝越界猎取、堆生命周期审计，以及强制执行的 PE/ELF/Mach-O × x86-64/AArch64 六单元矩阵 |
| `unittests/loader` | `NeverDRawISATests` | 二进制文件：从字节识别指令集（数据、测试程序自身代码、偏移两字节的代码、各家族 32 位与 64 位编码、以零开头的文件）以及 Cortex-M 向量表。`scripts/validate_isa_model.py --engine build/bin/libneverd.so` 按哈希下载模型从未见过的 180 个真实程序和库进行检查；它需要网络，不属于 CTest |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR 形状、IR 阶段、loader、重定位、格式 fixture、反编译与代表性 patch 流程 |
| `unittests/semantic` 中的大多数文件 | `NeverDSemanticTests` | 指令、ABI、控制流、C 表达式和 lift/recompile 差分语义 |
| `unittests/evm` | `NeverDEVMOpcodeTests`、`NeverDEVMBytecodeTests`、`NeverDEVMLoaderTests`、`NeverDEVMABITests`、`NeverDEVMAnalyzerTests`、`NeverDEVMDecoderPropertyTests`、`NeverDEVMProxyTests`、`NeverDEVMCallTests`、`NeverDEVMSemanticTests`、`NeverDEVMEmitterTests`、`NeverDEVMIntegrationTests` | 硬分叉元数据、输入规范化、ABI/签名歧义、CFG/SSA/恢复、穷举 decoder 边界与恶意输入、proxy/call 事实、解释器语义、LLVM/C/Solidity 差分执行及公共 API 路由 |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFProgramImageTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFVerifierTests`、`NeverDSBFISAConformanceTests`、`NeverDSBFAgaveConformanceTests`、`NeverDSBFSemanticTests`、`NeverDSBFEmitterTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`、`NeverDSBFMalformedCorpusTests`、`NeverDSBFUpstreamConformanceTests`、`NeverDSBFExternalOracleTests`、`NeverDSBFSolanaModelTests`、`NeverDSBFIntegrationTests` | v0-v4 元数据与 ELF 布局、严格 verifier/loader 行为、23 个固定 ELF 工件、独立 official oracle、全部 opcode 可用性、恶意输入、CFG/恢复及已执行的 LLVM/C/Rust 差分 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 四 ISA×三对象格式的重写/混淆等价性 |
| `unittests/semantic` 中的聚焦变换文件 | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 从大型语义二进制拆出的快速重链接探针 |
| `unittests/corpus`（子模块） | `NeverDWindowsEHCorpusTests`、`NeverDRustEHCorpusTests`、`NeverDGoEHCorpusTests`、`NeverDCxxItaniumEHCorpusTests`、`NeverDObjCEHCorpusTests`、`NeverDAdaDEHCorpusTests` | 从 545 个钉住的真实二进制中读出的异常与运行时元数据，每个都在清单里声明了其恢复必须达到的下限 |

注册的事实来源是
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt) 和
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt)、
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt) 和
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt) 和
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt)。

### 钉住的二进制 corpus

其它每个测试套件都自己构建被测对象，corpus 不是：它是一个子模块，装的是真实工具链
在本仓库够不到的宿主机上、为够不到的目标产出的二进制，每一个都按摘要钉住，旁边的
清单声明了它的恢复必须达到的下限。要回答"NeverD 从一个 `-O2` stripped 的 `armv7`
共享库里到底读出了什么"这类问题，只有这里给得出答案而不是论断。

这些套件只在 configure 被告知去找它们时才构建，所以这个开关就是它们是否受测的全部：

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` 跑全部产线；`check-neverd-windows-eh-corpus`、
`check-neverd-rust-eh-corpus`、`check-neverd-go-eh-corpus`、
`check-neverd-cxx-itanium-eh-corpus`、`check-neverd-objc-eh-corpus` 与 `check-neverd-ada-d-eh-corpus` 各跑一条。三个
CI 宿主都带着这个开关配置并跑全部六条产线：字节到处都一样，但读字节的东西不一样，
在一台宿主上跑通不能说明另外两台。`scripts/audit_ci_test_inventory.py` 会拒绝缺少六
个标签中任何一个的清单——构建悄悄不再读 corpus 是一种没有任何测试能捕获的回归，因为
消失的正是那个测试。

EVM 操作码审计每次运行都会用 `git fetch --depth=1 --force` 强制获取官方默认分支的远端
`HEAD`：`https://github.com/ethereum/go-ethereum.git`。脚本解析并报告刚取得的精确
SHA，再在 detached 临时 worktree 中探测该对象。每次运行都使用名称不可预测的私有临时 bare
repository，在 detached worktree 的整个生命周期持有官方 fetch 的 authority ref 与精确 SHA，
最后一起销毁 repository 和 worktree。不使用共享持久 Git repository 或 cache。
本地与 CI 都不读取 `local_docs`、已有源码 checkout 或 submodule。固定 submodule 反而会在
最需发现实时漂移时陈旧：

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

公开 CLI 唯一接受的选项是 `--manifest-output`，不提供 remote/ref/toolchain override。输出
manifest 的封闭契约是 `schema 3`。

每条 Git 命令都会先清空全部继承的 `GIT_*`（包括 `GIT_CONFIG_*`），再只装入经过审计的
设置。`GIT_CONFIG_NOSYSTEM` 与 `GIT_CONFIG_GLOBAL` 禁用 system/global 配置；
`GIT_ATTR_NOSYSTEM` 与按命令设置的 `core.attributesFile` 禁用 system/global attributes，
`core.hooksPath` 禁用 hooks。意外的 private-repository 配置、graft、`objects/info/alternates` 或
`refs/replace` 都会让校验失败；`GIT_NO_REPLACE_OBJECTS` 会禁用 replacement 查找。

CI 仅在 `dev` 分支 push、pull request、手动触发与每日定时任务中运行同一项在线审计。
Go 探针反射 `params.Rules` 导出的全部 bool 字段，针对每个映射分叉调用公开的
`LookupInstructionSet(params.Rules)`，并扫描全部 256 个 byte slot。
`EVMUpstreamOpcodePolicy.def` 管理名称别名及类型化的历史/未排期 EOF 排除项，并校验
overlap/inactive 不变量；正交的 `EVMUpstreamSemanticsPolicy.def` 管理封闭的 Rules
清单、分叉映射、base-stack 例外与 EIP-8024 dynamic opcode family 声明。封闭 manifest 检查精确
revision、fork activation、byte/name、`base_min_stack` 和 `net_stack_delta`，拒绝未知或
重复字段、规则、分叉、名称与字节。槽位分配只依据 `operation.undefined`；`HasCost` 只用于
费用交叉检查，因为已定义的零费用操作也返回 false。每个 `defined && !HasCost` 槽位都必须
从声明的分叉起与 `EVM_GETH_ACTIVE_WITHOUT_COST` 精确匹配。未定义却有费用、未经评审却已
定义，或 marker 消失都会封闭失败。失败的 CI 会上传精确 revision、manifest 与日志 artifact。
parser 与漂移诊断有独立 Python 单元测试：

`EVMUpstreamSemanticsPolicy.def` 用唯一一条 `EVM_GETH_RULE_FIELD` 将每个导出的布尔
`params.Rules` 字段归入 `MappedForkSelector`、`NoOpcodeAllocation` 或
`ExcludedSelectorExpectedError`。probe 每次只启用一个字段并调用 `LookupInstructionSet`；前两类
必须无错误，第三类必须报错，返回的完整 256 槽 opcode/stack 指纹都必须等于 `ExpectedFork`。
当前 `IsEIP155`、`IsEIP2929`、`IsEIP4762` 与 `IsPetersburg` 是 Frontier 指纹的无分配字段；
`IsUBT` 必须报错并呈现 Cancun 指纹。

EIP-8024 dynamic opcode family 的成员与启用条件由 `EVMUpstreamSemanticsPolicy.def` 声明；
`EVMEIP8024Immediates.def` 仍是 single/pair 各字节 immediate semantics 的唯一权威，其清单都
显式分类全部 256 个字节。生产代码直接查表；实时审计以 `go -overlay` 向 `core/vm` 虚拟注入
wrapper，取得真正的私有 `operation.execute` handler，并对每个 active table/family 执行
`DUPN`、`SWAPN` 和 `EXCHANGE` 的 `3x256` candidates 加 `3 missing-operand cases`。测试核对
接受性、PC 增量、marker 推导的 operand/stack 变更、有效值的精确 underflow 和缺少 operand 时的
`0x00`；Python 对照同一 `.def`，不重复公式。

`EVM_HARDFORK_LATEST` 只有一个规范目标；封闭的 `EVMUpstreamForkAliases.def` 将 Prague 映射到
Pectra，将 Osaka 与 BPO1 至 BPO5 映射到 Fusaka，而 Paris/Shanghai/Cancun/Amsterdam/Bogota
映射到自身。未知名称封闭失败。单次审计记录的 `audit_unix_time` 同时驱动
`MainnetChainConfig.LatestFork(time)`（必须等于 NeverD latest）和
`LatestFork(max uint64)` 的 alias/已探测规范分叉检查。探针枚举真实的
`canonical fork jump tables` 与 `mainnet active/scheduled jump tables`，逐表完整比较，并显式
记录 dynamic family 或分叉的 `inactive` 状态。只得到部分表、family 或探针的 `partial` result
不会被接受，而会封闭失败。manifest 固定
`authority=official-fresh-fetch`、官方 URL、请求的 `HEAD` 与 SHA；公开 CLI 没有
remote/ref/toolchain 绕过，probe 使用 `GOTOOLCHAIN=local`。

Go request/response 与 Python controller 会在分配恶意元数据前执行
`input/collection/string hard limits`，超限输入、数组或字符串均封闭失败。它们还独立执行
`bounded diagnostic output`：超长展示包含 full-content `digest` 与
`explicit truncated marker`。每条命令都有有界的子进程输出和共享 deadline；超时或输出超限会
终止整个 `process group` 及其后代 process tree，并排空 pipe。所有 `.def parser` 都会拒绝
unparsed、unknown、duplicate、missing、out-of-range 条目并封闭失败。

当前 schema-3 实时回执记录 `schema_version=3`、`audit_unix_time=1787534659`、
`authority=official-fresh-fetch`、`remote=https://github.com/ethereum/go-ethereum.git`、
`ref=HEAD`、revision `02b73d4ea7181464175e0a6cbecc0a3a2655a562`、本地 `Go 1.24.0`、
`stack_limit=1024` 与 `diagnostics=[]`。它覆盖 `21 fork tables` 和 `20 Rules probes`，分类为
`15 mapped/4 no-op/1 expected-error`。两个 `mainnet active/scheduled` 记录均报告
`upstream BPO2`，由封闭映射对应到 `NeverD Fusaka`。EIP-8024 有 `23 table targets`，其中只有
`Amsterdam/Bogota` 为 active，产生 `1536 candidate executions` 与
`6 missing-operand cases`。`three handler symbols` 在两个 active target 间一致。Python audit 为
`67/67`，`C++ Opcode 10/10`。macOS 真实运行在 `sandbox-exec` 下成功，最终 `go run` 保持
offline；Linux workflow 强制 `bubblewrap`。

所有 Go 阶段——`go env`、`go mod init`、`go mod edit`、`go mod tidy`、
`go mod download` 与 `go run`——都必须经过 `capability-root` 文件系统沙箱。其读取能力只包含
私有 probe、fresh geth、校验后的 `resolved GOROOT` 和精确必需的系统 runtime root；只有隔离的
environment root 可写。网络仅授予需要它的依赖阶段，最终运行保持离线。测试在
`host HOME/workspace` 中放置 sentinel，要求访问被拒，并要求任何输出都不含其内容。Linux 验证
同构的 `bubblewrap` 策略，且不使用 `/` broad bind。

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

当前 CMake 注册的 11 个 EVM 测试目标为：

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

`NeverDEVMDecoderPropertyTests` 会在每个改变 decoder 的分叉上穷举全部双字节输入，比较
完整解码和精确 `JUMPDEST` 边界；它还以长度受限的确定性恶意输入覆盖所有分叉。

修改 EVM 控制流时，先运行不动点与高度域契约：

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

这些用例覆盖跨基本块 internal return、有限多目标合并、循环收敛与确定性边排序、
路径相关 whole-stack lane、相关性保留、未知跳转、精确非法目标，以及包括
`MaxAbstractInstructionTransfers` 在内的 fail-loud 分析预算。strict 只在已证明
`Reachable` 的 lane 上拒绝未知或分叉未激活 opcode；`MayReachable` 只保留 CFG 候选，
不能产出确定语义。随后应
运行全部 11 个 EVM 测试目标与在线上游审计；CFG 修改也可能影响 emitter 与集成行为。

修改 MedIR/HighIR 数据流时，还要运行 constant-phi、selector、类型化操作数、
格式错误图和深链契约：

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

这些用例验证相等与冲突的循环 phi、非相邻和跨基本块 selector 表达式、等式两种操作数
顺序、精确 ABI 位宽检查、类型化 storage/event/calldata 操作数、只从 root lane 沿
dispatcher 不匹配边恢复 selector/receive/fallback、共享 selector 的标准歧义、逐标准
`KnownFunctionVariantInfo` 选择，以及只在所有已证明可达的成功终态返回形状一致时输出
return list。它们还覆盖格式错误 MedIR 的确定性处理与深 producer walk。

## fixture 如何生成

### Lift 与格式 fixture

`unittests/lift/CMakeLists.txt` 在构建期间跨目标编译 C 与汇编源码。Clang target
triple 生成 x86-64、i386、AArch64、ARM32 ELF 对象，PE/COFF 对象和已链接
镜像，以及 PIC/no-PIC Mach-O i386 对象。存在 LLD 时，选定对象还会链接为
patch 测试所需的可执行文件。`NeverDLiftTests` 依赖 `lift-test-objects` 目标，
因此正常构建该测试二进制会刷新生成的 fixture。

多数 lift 测试使用 `NeverDLiftFixture.h` 调用构建出的 `neverd` CLI，并检查
LowIR、MedIR、HighIR、LLVM IR、生成的 C 或重写后的二进制。聚焦手动实验可用
`NEVERD` 环境变量覆盖 CLI 路径；普通 CTest 运行使用 CMake 嵌入的可执行文件。

### 内存安全 fixture

`unittests/safety/fixtures/binaries` 检入了 x86-64 与 AArch64 的 PE、ELF、Mach-O 镜像，以及各格式对应的 PDB 或 dSYM 伴生文件，每个镜像还附带一份链接器 MAP。MAP 是被 strip 的构建唯一还会留下的身份信息，因此每个单元还会显式指定 MAP 再分析一遍，用来钉住在既无类型也无源码行号时结论还能说什么。`NeverDSafetyIntegrationTests` 在每个主机上运行全部六个单元；任何必需镜像或伴生文件缺失都会在配置阶段失败，测试不存在按宿主工具链跳过的路径。

六个等价二进制来自同一源文件。`make` 只重建宿主原生 smoke fixture；完整矩阵用：

```bash
make -C unittests/safety/fixtures matrix
```

完整重建需要 Clang 的 Linux／Windows 交叉目标、LLD COFF 工具、两个 Darwin 架构与 `dsymutil`。规则会重映射调试路径并关闭 CodeView 命令行记录，避免检入的伴生文件捕获开发者工作区绝对路径。

### Windows 异常重建

修改 Windows 表驱动异常时，既要测试表示层，也要对已链接 PE 运行 patch 测试。
下面的聚焦 lift-suite 过滤器覆盖规范化 unwind/SEH/C++ 模型、损坏输入处理、
异常 CFG 边、HighIR、LLVM WinEH 生成、异常目录替换，以及 Guard CF/EH
continuation 重建：

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

受保护的 x64 汇编 fixture 需要 Clang Windows target 和 `lld-link`；其 CMake
链接使用 `/guard:cf` 与 `/guard:ehcont`。因缺少交叉链接器而跳过，不能作为
final-image 路径的有效证据。集成用例通过后，才证明重写后的 PE 可以重新加载，
且 runtime-function、unwind、load-config、Guard CF 与 Guard EH continuation
表保持有序、由文件承载，并只指向可执行目标。

已链接的 FH3 fixture 独立覆盖原生 C++ 闭包：固定状态表、HighC 注释、
personality 保留、生成的 catch 目标，以及重新加载后的 IP-to-state 图。

分析/原生支持矩阵和 fail-closed patch 契约见
[Windows 异常重建](windows-exception-reconstruction.md)。

### 语言异常模型

除 Windows 表模型以外的一切都集中在一个聚焦 target 中。
`NeverDLanguageEHTests` 覆盖 DWARF 帧链、Itanium 语言特定数据区、ARM EHABI、
Darwin compact unwind、Go 运行时帧元数据、Rust panic 机制，以及三种
Objective-C 运行时：

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

本套件中的表是逐字节手工装配而非编译出来的，因为其中大多数要验证的组合
没有任何单一工具链会同时产出。Objective-C 是最典型的例子：三种运行时都发出
Itanium LSDA，差别只在类型表槽位里放什么——而这个差别是彻底的，不是程度问题。
Apple 的槽位指向 `objc_typeinfo`，其前两个字段刻意模仿 `std::type_info`；
GNUstep 的 Objective-C++ 槽位指向真正的 `std::type_info` 子类；GNU 运行时的
槽位根本不是指针，而是类名字符串本身。把一种运行时的约定套到另一种的表上
不会报错，只会报出一个从别的东西中间读出来的类名——所以在读任何槽位之前，
先由帧的 personality 确定运行时。

同一套件还钉住两个容易混为一谈、但混淆即错误的区分。`@catch(id)` 与
`@catch(...)` 是不同的处理器——前者接收任意 Objective-C 对象，并放外来异常
从旁边继续传播——而每种运行时对二者的拼写都不同，所以把两者都报成 catch-all
的解码器，等于给那些本会飞过去的异常安上了处理器。另外，setjmp/longjmp 的
call-site 表索引的是调用点序号而不是地址，因此没能认出某个 SJLJ personality
的读取器不会报错，而是会凭空造出程序从未指定过的保护区间和 landing pad。

认出这种形式，和拒绝解码它，是两回事。一条 SJLJ 条目是一对 ULEB128 值——
一个派发选择子和一个动作偏移——而这个动作偏移在此处的含义与地址形式中完全
一致，所以动作链、catch 类型、异常规格，全都能从一张根本不指名任何代码的表
里读出来。唯一读不出的是每条条目守护的区间，因为说明它的是函数自己对
call-site 槽位的写入，而不是表里的任何东西。该套件还钉住了此处唯一不可信的
那个字节：GCC 把 call-site 编码写成 `DW_EH_PE_uleb128`，LLVM 写成
`DW_EH_PE_udata4`，两者随后都照样发射 ULEB128，而没有任何 personality 会去
读它——所以解码器也不许读。

personality 身份同样在这里钉住，因为它决定了上面每张表该怎么读。GNAT 用
GCC 给每个前端的那三种拼法命名自己的例程——`_v0`、`_sj0`、`_seh0`——并且在
Windows 上注册一个符号却转发到另一个，所以这四种拼法都必须落到 Ada 上。D
则是镜像的情形：三个编译器，同一个例程的三个名字，背后是同一套表。

### Unicorn 差分往返

语义 fixture 测试行为而不是文本形状：

1. 编写一个小型 C/汇编用例，或构造 LLVM IR。
2. 用 Clang/LLVM 为请求的目标编译它。
3. 在 Unicorn 中执行原始机器码，并捕获期望返回值或 fixture 定义的其他状态。
4. 通过 NeverD 加载并提升，发射 LLVM IR，再将结果编译回机器码。
5. 使用相同 ABI、输入、内存布局和 CPU 模型执行再生成的代码。
6. 比较可观察结果。

主要实现位于
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h)。
patch-full fixture 使用 `Codegen::compileForRewrite`（与 patch 操作相同的重写
backend），随后在完整 4×3 ISA/格式网格中比较基线与变换后代码。

确定性的 NeverD 语义失败应当成为失败测试。跳过只用于明确的外部能力边界，并应
阅读 skip 原因：缺少跨目标 linker 的绿色摘要不能证明该格式路径实际运行。

### EVM 差分后端

EVM 解释器测试提供确定性的 256 位 oracle。emitter suite 直接编译执行生成 LLVM，
通过 Clang lower 生成 C23 并在同一 host harness 中执行；安装 `solc`、`anvil`、
`cast` 与 `jq` 后，还会将生成 Solidity harness 部署至本地 Anvil。测试比较 status、
storage 与 instruction trace count。独立 raw-bytecode corpus 直接在 Anvil 原生 EVM
中执行 pre-Fusaka 标量 ALU、calldata/memory 复制、重叠 `MCOPY`、Keccak 与 return data。

解释器在任何 opcode 特有副作用之前执行类型化 stack preflight；
`EVMForkSemantics.def` 规定字节 `0x44` 在 Paris 前为 `DIFFICULTY`、从 Paris 起为
`PREVRANDAO`。`REVERT`、fault、step limit 和资源耗尽都会回滚事务状态；分配失败标为
`ExecutionFaultKind::ResourceExhausted`，若入口快照都无法建立，
`HasPersistentStateSnapshot` 为 false，结果不可提交。

### EVM 公开边界与预算回归

公开 API 测试会分别篡改规范
`Code`/`Fork`/`Instructions`/`JumpDestinations`，以及每个 LowIR table、range、ID、lane 与
edge reference。`execute` 必须在查找 instruction 前返回 `llvm::Error`；`lowerToMedIR`
必须在建立索引或按输入规模分配输出前，拒绝完整结构非法或超预算的 LowIR。
`lowerToMedIR` 测试还强制 option validation、resource validation、structure validation 的顺序，
并要求它们先于逐字段 `canonical decode replay` 和 `lowerCanonicalLowToMedIR`。公开 HighIR
恢复会重放校验外部 LowIR/MedIR；只有 `analyze` 能对自己持有的规范 IR 使用
`lowerCanonicalLowToMedIR` 与 `recoverCanonicalHighIR`，既避免递归或重复重放，也继续强制
所有 HighIR option/resource 预算。解释器随后对
`EVMInterpreterLimits.def` 声明的所有上限执行 exact-boundary 与 +1 测试：`MaxSteps`
保持专用 `StepLimit`；`MaxMemoryBytes`、`MaxTraceEntries`、`MaxLogEntries`、aggregate
`MaxLogDataBytes` 与运行期 `MaxPersistentStateEntries` 耗尽都返回
`ResourceExhausted` 并回滚事务效果。初始 aggregate `MaxHostReturnDataBytes` 或 persistent
state 过大是 API error。初始 `MaxCalldataBytes`、横跨 `BlockHashes`/`Balances`/`CodeHashes`/
`ExternalCode`/`BlobHashes` 的 aggregate `MaxHostEnvironmentEntries`，以及 aggregate
`MaxExternalCodeBytes` 同样属于 API error。`const execute preflight` 会在复制 environment、
snapshot 或 result 前拒绝它们。测试也覆盖 return-data `ArrayRef` view 与排序表 `lower_bound`
lookup，无需复制 buffer 或建立 PC map。

独立的 LowIR 边界测试覆盖 aggregate diagnostic 上限 `MaxLowDiagnostics` 与
`MaxLowDiagnosticBytes`，验证线性 decode/CFG 构造按精确数量和最终字节预先计费并拒绝零上限。
HighIR 安全测试覆盖按 lane 排序的 `Any/Exact/Excluded` domain、相等 match/exclusion、原始
`XOR(selector, constant)` 的 false-edge match 与 true-edge mismatch、零 word/calldata
size/call value 的逐边精化，以及 unknown condition 的 fail-closed 行为。
测试还包含 `EQ` 与 `raw XOR` 两类 back-jump regression，确保 `arguments`、`mutability`、
`return shape`、`region` 不受另一函数污染。其 exact-boundary 与 -1 测试覆盖
`EVMAnalysisLimits.def` 中的 `MaxHighDispatchCandidates`、aggregate
`MaxHighRecoveredArguments`、`MaxHighDiagnostics`、`MaxHighDiagnosticBytes`、
`MaxHighReferenceVisits`、`MaxHighMemoryTransferCells` 与
`MaxHighMemoryValueVisits`；所有输出 diagnostic（包括固定 malformed diagnostic）都必须在
分配前计入数量与最终字节数。LowIR 与 HighIR diagnostic 预算会独立测试；构造默认根 CFG
region 时必须在 reserve 或复制 block-PC 清单前计入 `MaxHighRegionBlockReferences`。
外部 CALL/CREATE 结果作为非确定 host outcome 探索两条精确 CFG 边，因此保留 ERC-1167
fallback 恢复；不可读的 selector 条件仍是 Unknown，不能凭空产生 fallback 或 function 事实。

控制流测试从 `EVMLowFaultKinds.def` 取得 `InvalidJumpDestination`，并用于
`end-of-code JUMPI`：目标非法且条件确定为 true 时没有成功 tail，属于确定 fault；条件确定为
false 时成功；条件未知时保留可能成功的 false 路径，不把整条 lane 标为确定 fault。

ABI 测试在精确上限和 +1 位置验证 `EVMABIParserLimits.def` 的 grammar 边界，以及
`EVMABITableLimits.def` 的公开表基数/text 边界；还会拒绝非法 kind/standard/evidence
enum、错配 metadata、非规范 signature/return list、被错误标记为 independent 的共享
selector、悬空或重复 variant，以及非 word 宽度的 event-topic `APInt`，再进入索引化
selector 或排序 topic lookup。

`NeverDEVMOpcodeTests` 还约束 metadata 架构：每个已分配 opcode 都在 byte encoding 与
typed value 之间往返，测试 family helper 边界与 hardfork alias，完整 stack contract
和 host argument maximum 保持推导而不在 backend 中重复。

### Solana SBF 差分后端

SBF 元数据测试会验证每个版本特性、操作码冲突边界、Murmur3 syscall hash、重定位、ELF machine、寄存器和 VM 地址常量。Loader fixture 不依赖 vendored 二进制，直接生成旧式 v0-v2 section 布局和无 section 的严格 v3/v4 program-header 布局。

`NeverDSBFISAConformanceTests` 按 v0-v4 的每个版本，将每一种 byte encoding
与独立审计的 typed manifest 对照。`NeverDSBFExternalOracleTests` 随后把 activation
和 boundary 决策与单独构建的官方 Anza 进程比较。
`NeverDSBFUpstreamConformanceTests` 为固定 Anza revision 中的全部 23 个 ELF
指定明确结果。

`NeverDSBFSemanticTests` 直接执行已验证的指令字节而不消费 MedIR，因此修改或破坏规范化 IR 不会让源 oracle 与后端意外达成一致。覆盖范围包括非单调的 v2 语义、内存、syscall、内部调用帧、fault、trace 和资源限制。LLVM module 会被验证；生成的 C 以 warnings-as-errors 编译，Rust 使用 `-D warnings`。公共 API 测试从生成的严格 SBF ELF 出发，遍历所有 IR 阶段、反汇编、CFG、元数据、LLVM、C 与 Rust。

## 一次性目标

自定义目标会构建其依赖，然后以主机 CPU 推导的并行度运行 CTest：

| CMake 目标 | 选择范围 |
|------------|----------|
| `check-neverd` | 所有已注册测试 |
| `check-neverd-semantic` | 仅 `NeverDSemanticTests` |
| `check-neverd-sbf` | 所有 `NeverDSBF*Tests` 目标/用例 |
| `check-neverd-patch-full` | 仅 `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | 仅 `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | 仅 `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | 仅 `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` 与 `NeverDAvxUpperXformTests` 当前没有
`check-neverd-*` 便捷目标；请按下文先构建，再用标签选择。
`check-neverd-semantic` 也不包含单独的变换或 patch-full 二进制；完整聚合应使用
`check-neverd`。

## 增量 CTest 工作流

先构建所属可执行文件，再选择其标签。这样可以避免重链接无关的大型语义目标。

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

# 所有聚焦的 EVM 目标/用例
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# 所有聚焦的 Solana SBF 目标/用例
cmake --build build-release --target check-neverd-sbf --parallel 4
```

用 GoogleTest 派生的 CTest 名称运行单个回归：

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

常用选择器：

| 命令 | 用途 |
|------|------|
| `ctest --test-dir build-release -N` | 列出已发现用例而不运行 |
| `ctest --test-dir build-release -L '<regex>'` | 选择测试二进制标签 |
| `ctest --test-dir build-release -R '<regex>'` | 选择用例名称 |
| `ctest --test-dir build-release --output-on-failure` | 仅为失败显示诊断 |
| `ctest --test-dir build-release --stop-on-failure` | 第一个失败后停止 |
| `ctest --test-dir build-release --parallel 4` | 最多并行运行四个用例 |

GoogleTest 发现使用 `DISCOVERY_MODE PRE_TEST`，因此 CTest 枚举前必须存在对应测试
二进制。每用例 timeout 与独立的发现 timeout 定义于 `cmake/AddNeverD.cmake`，
只有存在测得的重型用例时才应放宽。

## 哪些测试应随代码变化？

| 变更区域 | 从这里开始 | 随后考虑 |
|----------|------------|----------|
| 架构 lifter 或 decode | `NeverDLiftTests` 中的命名用例 | 对应 ISA 语义往返 |
| LowIR CFG、函数检测、跳转表 | Lift CFG/switch 用例 | `NeverDSwitchXformTests`、`NeverDCFGLoopXformTests` 或 `NeverDTwoTableXformTests` |
| MedIR、ABI、标志、类型、SSA | MedIR/调用约定 lift 用例 | 跨 ISA 的 `NeverDSemanticTests` 用例 |
| HighIR 或结构化 C | HighIR/decompile 用例 | `NeverDCFGLoopXformTests` 与生成 C 编译检查 |
| PE/ELF/Mach-O loader 或输入重定位 | 对应的 `unittests/lift` 格式 fixture | 该单元格的全阶段加载/反编译测试 |
| 重写 codegen 或输出重定位 | `RewriteCodegenRTTests` 用例 | `NeverDPatchFullTests` 及存在时的已链接 patch fixture |
| patch 使用的 LLVM IR 变换 | 聚焦变换二进制 | `NeverDPatchFullTests` 组合 pass 网格 |
| C API 或 CLI | 直接 SDK/query 测试与 `unittests/semantic/CLIEndToEndTests.cpp` | 相关 pipeline/格式套件 |
| EVM loader、opcode、IR 或 backend | 最小的所属 `NeverDEVM*Tests` 目标 | 所有 EVM 目标，以及生成 C/Solidity 的编译检查 |
| SBF loader、ISA、IR 或后端 | 最小的所属 `NeverDSBF*Tests` 目标 | 所有 SBF 目标，以及生成 C/Rust 的编译检查 |
| Libc 识别 | `NeverDLibCTests` | 行为变化时的语义 call/ABI 用例 |
| 堆生命周期审计或拷贝越界猎取 | `NeverDSafetyTests` | `NeverDSafetyIntegrationTests` 的全部六个单元 |
| 进程执行或 quoting | `NeverDTestProcessTests` | 每个受支持主机上的一个受影响 CLI/语义用例 |

测试应在最低的稳定边界表达契约。LowIR 形状测试适合归因到 lifter；若两种看似
合理的 IR 形状可能行为不同，则必须使用语义往返。若小型 opcode、CFG 或可观察状态
断言已经足够，应避免保存整个函数的 golden dump。

## 与 CI 的关系

CI 在 Linux、macOS 和 Windows 上以 Release 开启测试构建，先审核发现的测试清单，
再应用平台特定的标签排除。配置定义于 `.github/workflows/ci.yml` 和
`scripts/audit_ci_test_inventory.py`。每个矩阵主机都必须包含 `NeverDSafetyTests`
和 `NeverDSafetyIntegrationTests`，而且每次都读取同一组已检入的 PE、ELF、Mach-O × x86-64、AArch64 fixture。由于没有单个矩阵 shard 代表所有昂贵套件，当机器具备全部跨目标工具时，本地 `check-neverd` 仍是最清晰的完整合并前信号。

## 当前 Solana SBF 一致性与 sanitizer 配置

本节的当前清单取代上方较短的 SBF 清单。source differential suite 除 clang 外还
需要 `rustc`；compiler skip 表示覆盖缺失。完整 aggregate 包括
`NeverDSBFProgramImageTests`、`NeverDSBFMalformedCorpusTests`、
`NeverDSBFISAConformanceTests`、`NeverDSBFUpstreamConformanceTests`、
`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`，以及 metadata、
loader、analyzer、semantic、emitter、integration target。integrated profile 记录
命名 target 与结果，不冻结快速变化的汇总 case 数。

sanitizer profile 单独构建在 `build-sbf-asan-ubsan`。按 revision 锁定的 prebuilt
package 已包含所需的 fork-only header，因此 integration 也在同一个 fail-fast
ASan/UBSan profile 中运行。

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

### 固定的 SBF 证据快照（2026-08-24）

gate 将 Anza `sbpf` 固定在
`2510663bb8d894e8e3094be351e4bb4b604f1f84`、Agave 固定在
`ef210d67f2fabeee1730498188fa78854260c679`、Solana SDK 固定在
`122f32e571ce39face4beffaccea733e37c207fd`。官方 ELF manifest 全部 23/23 通过；
`NeverDSBFExternalOracleTests` 经 `SBFOfficialOracleProtocol.def` 和
`SBFOfficialVerifierCases.def` 与 `SBFOfficialExecutionConstants.def` 对照
1,411 个 opcode/verifier boundary case。
`SBFOfficialELFMutations.def` 是畸形 ELF 的表驱动契约；其总数仍会演进，因此不冻结。
另有独立的 `41-case strict ELF differential`，将完整 strict-v3 mutation matrix 送入
官方 `verify-elf-batch` 与 NeverD；这 41 个 case 不计入 1,411 总数。
`NeverDSBFAgaveConformanceTests` 认证 Firedancer test-vectors 的
`68bb4af40235562e8852fa23d5727e49c2a0b862`，匹配全部 1,955 `sol_compat_elf_loader_v1` 个 loader fixture
（接受 1,399、拒绝 556），并为每个接受的 ELF 比较 `entry_pc`、`text_off`、`text_cnt`、
`rodata_hash` 与 `calldests_hash`。此门禁不运行后续 instruction verifier。

额外的官方执行矩阵单独统计：恰有 508 个 active `(Version,Opcode)` case，另有
58 个 boundary case，共 566 个 exact execution case。它既不替代、也不计入
1,411 个 verifier probe 或 `41-case strict ELF differential`。
Linux Release CI 使用 `--print-pinned-revision`、`--print-test-vectors-revision` 与
`--print-toolchain`，并导出 `NEVERD_SBPF_ORACLE` 和
`NEVERD_AGAVE_CONFORMANCE_ROOT`，因此两个 external gate 都强制执行；普通本地运行
未提供明确 oracle/corpus env 时仍会发现 case，但允许 skip。

`SBF_RUNTIME_VERSION` 让 `RuntimeVersionPolicy::ChainProfile` 按历史 cluster/slot
计算：官方 feature account activation 使最大 ISA 从 V0 依次推进到 V1、V2、V3；
当前仍是 V3。显式 v4 使用 `RuntimeVersionPolicy::UpstreamToolchain` 做离线分析。
当前 10 MiB 上限精确为
`10'485'760` byte；65,536 仅是历史 provenance/test。`SBFFaultCodes.def` 固定
execution fault 的稳定值；`SBFSourceStatuses.def` 单独拥有 generated-source ABI。

10,000 规模 fixture 守护 worklist、function ownership 与 multi-latch，不固定某台机器
的耗时。cluster/account/slot row 支持 `RPC activation audit`，普通测试仍保持
deterministic 与 offline。

## Android 类清单性能

测量 `neverd mobile INPUT --list-classes` 前，以 Release 构建 `NeverDMobileTests` 并运行其标签。读取器测试覆盖稀疏元数据、Unicode、无效引用、不支持的方法体、校验和及预算；归档测试区分全量提取与选中载荷查询。CLI 测试检查前缀筛选、JSON 范围、已有输出保护及 multidex 失败原子性。

独立夹具/测量工具在接受计时样本前，验证每个进程的完整描述符清单：

```sh
python3 -m unittest scripts.tests.test_benchmark_mobile_inventory -v
python3 scripts/benchmark_mobile_inventory.py \
  --output-dir /tmp/neverd-inventory-benchmark \
  --class-count 6000 --dex-count 3 --code-units 64 \
  --extra-string-bytes 8388608 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

每轮使用新输出目录。`--generate-only` 只写夹具和清单；`--workload` 为可选 `--peer-command 'tool {input} {prefix}'` 选择共同输入，输入准备不计时。报告保留哈希、命令、所有新建进程样本、热缓存前提，以及 Linux/GNU time 下的最大子进程 RSS。RSS 不表示多进程工具的同时总峰值。合成 APK 是查询容器，不能安装；类清单速度不证明引用搜索速度或 Java 恢复质量。

混合架构 CPU 上，应将工具及子进程固定到同一个允许使用的 CPU，例如 Linux 下 `taskset -c 4 python3 ...`，避免混用性能核和能效核。报告记录继承的 CPU 亲和性。

## Android 代码引用性能

引用查询共用恢复读取器的指令边界与代码校验，修改该边界后应运行移动套件。读取器测试覆盖各池操作数、匹配模式、方法归属、共享代码、payload/立即数干扰、损坏输入及资源限额。共享调试流按每个方法体的帧、范围和参数校验。存储上限覆盖须同时保留大型成员清单与分支密集方法体，持久索引和临时容器增长具有不同生命周期。

还需覆盖乱序/重叠项目、同宽但原型不兼容的共享代码、未对齐输入、跨搜索块的子串匹配。私有解码器数据的性能调整须保持完整的独立恢复模型和引用多重集，包括不支持的恢复元数据对应的失败行为。区分独立发射的预期结果与真实输入的跨工具一致性。

独立引用夹具在发射指令时记录预期出现位置，每次测量都核对完整方法身份、代码单元 PC、opcode、目标身份、UTF-16 单元及重数，并核对 NeverD 独立预期的覆盖计数与 `code_scan_complete`：

```sh
NEVERD_REFERENCE_TEST_BINARY="$PWD/build-release/bin/neverd" \
  python3 -m unittest scripts.tests.test_benchmark_mobile_references -v
python3 scripts/benchmark_mobile_references.py \
  --output-dir /tmp/neverd-reference-benchmark \
  --class-count 500 --methods-per-class 64 --matching-methods 2 \
  --dex-count 3 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

`--kind` 和 `--workload` 选择用例；`--extra-strings 65536` 覆盖真实32位字符串索引。默认启用 payload 干扰；`--no-payload-lookalikes` 保持布局和真实引用，只替换干扰数据，用于共同输入对照。正确性与计时结果都要保留；误报 payload 或漏掉真实引用的查询不接受计时。

可选 `--peer-command` 接受包含 `{input}`、`{kind}` 和 `{query}` 的 argv 模板。另一工具语义不同时须显式转换查询语法，比较完整出现位置的多重集。保留其声明的验证范围，不将其说成完整代码扫描。类清单基准的新目录、CPU 亲和性、新建进程、热缓存及 RSS 限制同样适用。只有 `NEVERD_REFERENCE_TEST_BINARY` 指向已构建程序时才运行可选 CLI 测试，否则必须报告跳过。

## 移动 SDK 导出证据

手动工作流 `Mobile SDK Export Evidence` 针对固定的 Xcode SDK 运行 `collect_mobile_ios_sdk_declarations.py --exports-only`。它原样留存 iOS 真机和模拟器 SDK 的 Foundation、CoreFoundation、UIKit 链接器映射，并记录目标、SDK 版本、SDK 设置哈希、文件大小和 SHA-256。常规声明收集器也会留存这些映射。文件缺失、为空、超出大小限制或位于 SDK 外部时，收集失败，并保留已完成的证据。链接器映射提供符号导出证据，不能证明调用 ABI 或方法恢复成功。

## 移动端 Swift String ABI 证据

手动工作流 `Mobile Swift String ABI Evidence` 使用 Xcode 26.5，为 arm64 iOS 设备与模拟器编译固定的 Swift 相等、排序比较探针及 C `swiftcall` 探针。`collect_mobile_swift_string_abi.py` 保存源码、LLVM IR、汇编、编译器身份、SDK 设置与 `libswiftCore.tbd` 及其哈希。两种语言都必须显示精确比较导入采用五个参数并返回 `i1`；C 必须将该结果显式扩展为一字节。目标或签名不符、命令失败及超时均保留部分证据并令采集失败。这些编译器证据不会安装运行时声明，也不证明方法已恢复。可在无 SDK 环境运行 `python3 -m unittest scripts.tests.test_mobile_swift_string_abi` 验证采集器。

## 模块化 MBA 简化

`SymSimplifyFinite.*` 覆盖 8 至 512 位的完整两值域片段、共享用途下的收益计算、所有支持的可产生 poison 的注解、独立易失读取和 freeze、显式 undef/poison、深层迭代遍历、预算及混淆标记。原始与化简后的 IR 在 O0/O2 下执行，对全部字节输入和随机全宽输入与独立判定程序比较。翻译对象测试要求不同有限值预算具有不同缓存身份。

合并值域测试覆盖 8–512 位嵌套选择、菱形 PHI 和复制循环；冲突回边、未定义条件、无来源分量、独立 PHI/freeze 观测及带标记产生节点的保留；以及精确的节点、边和工作量边界。O0/O2 运行 oracle 穷尽所有字节输入对，并改变完整位宽的操作数，验证选择、汇合及有界状态循环。

两值测试还覆盖 8–512 位嵌套合取掩码、操作数交换、OR/undef 拒绝、有界深层发现、独立工作计费与混淆标记策略，以及首次改写的精确预算边界。运行 oracle 穷尽所有字节输入对，并改变无关的 64 位数据，在 O0/O2 对照原始和化简 IR。

`SymSimplifyPredicates.*` 穷举四位偏移、符号和输入，检查布尔区间组合与不连续集合，并在 O0/O2 运行独立的字节及全位宽 oracle。覆盖 poison 注解、合流点隐藏的未定义输入、独立读取/freeze、保留的循环 PHI、共享用途收益、累计预算、高扇出、递归上限和混淆标记。翻译对象测试要求两种缓存键都区分条件分析预算。

`SymExpr.*` 用四位输入与掩码的穷举检查非低位常量窗口，并覆盖宽承载、嵌套结构操作、已知与未知字节混合重组及算术进位反例。预算回归把宽节点放在递归边界，并拒绝复制超出预算的宽常量。未知窗口必须保持符号形式，且不能扩展表达式 DAG。 `SymState.*` 还在两种端序下区分标量推导常量和区域字面常量，确保不增加 DAG 节点、不改变完整存储值。

`SymReadability.*` 覆盖减法与补码的打印形式、结合律运算的代价、单比特和宽字面量、共享树大小饱和、有预算的候选选择，以及关闭采样时三比特的穷举等价性。`SymMBASample.*` 将窄值和任意精度验证与 AP 求值器比较，覆盖全部运算符、确定性赋值和未使用的宽输入。比较不同评分版本的候选质量时，必须用同一指标重算两边输出；SDK 随版本变化的大小计数仅供诊断。

## ARM32 与栈帧转发测试矩阵

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

栈帧溢出测试矩阵通过两套 C 后端覆盖 x86-32（ELF/COFF/Mach-O）、ARM32（ARM 与 Thumb ELF）和 AArch64（ELF/COFF/Mach-O）。重复的私有栈帧读取必须化简为加减法，并在两个优化级别正确执行字节对、跨字边界对和确定性随机字。Clang AST 检查完整函数中的残留 MBA 运算，同时区分合法地址表达式。HighFrameStoreForwarding 覆盖精确访问位宽、局部变量变更、内存写入、前缀别名、部分重叠、有序内存、畸形或循环图，以及渲染展开预算；其窄位取补回归在语义化简后执行。HighCStoreForwarding 在四种架构上保持转发值所依赖的定义存活，包括浮点重新解释与额外直接使用。SymSimplifyGuard 检查加载身份和顺序、volatile/atomic 状态及 poison 边界。ELFARM32ModeTest 验证 ARM/Thumb 选择、地址规范化、纯映射符号对象、混合模式元数据保留及同一地址矛盾证据的拒绝。ELFARM32ModeCAPITest 在同一 SDK 会话中以混合元数据替换 Thumb 映像，验证反汇编、HighC、LLVMC 的明确错误，再重载 Thumb 验证解码器恢复。InstructionMode 覆盖相应的解码、代码指针、直接分支与代码生成边界。缺少跨目标 Clang 应标记为跳过，不能当作格式通过的证据。

`NeverDHighControlFlowTests` 中的 `HighBoundPrivateFrameCopies.*` 检查调用 ABI 绑定后经非逃逸私有帧槽传播的副本，覆盖分支、栈槽复用和不同守卫上下文的一致性。x64 与 AArch64 生成的 C 在 `-O0`、`-O2` 下启用未定义行为陷阱，并与独立算术结果比较。反例要求在帧地址逃逸、调用 ABI 未知、帧别名缺失或不一致、入口参数被重新赋值、访问重叠、有序或原子内存、畸形语句、循环及预算耗尽时保留原函数。普通值转换不能被标记为 PHI 复制。

`NeverDHighControlFlowTests` 中的 `HighIntegerSignedness.*` 检查一个后期 pass：它按每个寄存器或临时局部变量大多数用途的需要，把它声明为有符号或无符号。回绕算术、逻辑移位和无符号比较倾向无符号；有符号比较、有符号除法、算术移位和符号扩展倾向有符号；任何一处非整数用途都会让该变量保持原类型。生成的 C 在 `-O0` 和 `-O2` 下带未定义行为陷阱运行，并与独立的参考算术比对，其中包括对已变为无符号的局部变量做有符号比较的情形。

`NeverDHighControlFlowTests` 中的 `HighValueForward.*` 检查 HighC 写出器何时可以把只用一次的值折叠进它的使用处。循环条件会保留一个其变量在循环中被赋值的值，因为同一个名字可能代表多个 SSA 值；重新读取的栈槽在对该槽的写入之后仍保持原值，而在对其他槽的写入之后可以折叠。源变量在使用前被重新赋值时，副本保持原值。每种情形都在 `-O0` 和 `-O2` 下带未定义行为陷阱运行。

`NeverDHighControlFlowTests` 中的 `HighCIntegerConversion.*` 检查 HighC 写出器交给 C 完成的整数转换。操作数内部的转换若保留了外层转换所保留的字节，就不再单独输出强制转换；对已声明整数局部变量的赋值和 return 采用隐式转换，字面量写成转换后的值，而指针保留显式转换。内存写入与赋值一样转换，传给有类型的更宽参数的零扩展实参保留其扩展。每种情形都在 `-O0` 和 `-O2` 下带未定义行为陷阱运行，并与参考运算比较。

源码投影还会在清理后重新验证变参对象列表：允许空的指令地址锚点，但拒绝隐藏效果或控制转移。同步清理允许同一已保存接收者的单层 `int64_t` 或 `uint64_t` 视图；窄化、浮点转换、地址运算和重新赋值仍被拒绝。Foundation 对象集合及正常、异常解锁轨迹均在 `-O0` 和 `-O2` 下执行验证。

## x64 原生同步异常

checked x64 的 `DIV`/`IDIV` 使用处理器产生的结果和 `#DE`。KVM 通过私有 supervisor IDT/IST 接收异常，WHP 使用明确的异常拦截位图；异常保留原始上下文和可用的错误码，与后端传输错误分开。OS 模型必须先消费可恢复事件，再安装明确的继续执行上下文。Windows 驱动将零除及商溢出映射为 `STATUS_INTEGER_DIVIDE_BY_ZERO`，并执行实际 SEH filter、`__finally` 和重试。`NeverDX64ExceptionTests` 可在禁用 Unicorn 时构建；原始 WDK 用例由 `DriverWDMCPUException` 验证。缺少的 ARM64 主机覆盖会明确跳过。

## 分阶段提交 RAM 效果

`RAMTransaction` 在物理执行租约内，只保存一条指令明确声明的写入范围的物理并集。结果观察器运行前恢复原始 RAM；取消、后端传输错误和观察器异常不会发布部分 RAM 或寄存器。CPU 异常在 RAM 回滚后保留架构异常状态。ARM64 的单次和成对写入共用该内存权威层。x64 支持 8/16/32/64 位 `XCHG`、`XADD`、`CMPXCHG`，LOCK 或隐式锁定形式要求自然对齐。`NeverDRAMTransactionTests` 将结果与独立宿主 CPU 对照，并验证回滚、别名和权限；不可用的平台明确跳过。设备事务和并行 SMP 仍不在此契约内；CPU 快照不会撤销已经提交的 RAM。

`CMPXCHG8B` 和 `CMPXCHG16B` 在 KVM、WHP 和 checked Unicorn 的驱动及用户模式中执行原始指令。比较成功或失败都需要读写权限，故障按写访问分类。`CMPXCHG16B` 在访问内存前检查 16 字节对齐，不满足时报告 `#GP(0)`。两次结果观察属于同一 RAM 事务；任一次停止或抛出异常，都不会发布寄存器或内存变化。未加锁的 `CMPXCHG8B` 可以跨页，加锁操作仍要求自然对齐。`X64WideAtomicTests.cpp` 对照宿主机原始执行结果和直接原生故障，并检查别名、前缀、寻址、修复重试和取消。原创 Windows 驱动及 ring3 PE 样例覆盖两种宽度，WDK 样例还执行 `_InterlockedCompareExchange128`。CPU 模型必须支持 `CMPXCHG16B`。

## 完整 x87 状态

`NeverDEmulationArch` 独立负责 ISA、页表及 FP 状态布局，原生与 Unicorn 传输共用该层。x64 上下文保存 x87 控制、状态、TOP、物理标签、操作码、指令／数据指针和八个 80 位寄存器。`FP0`–`FP7` 使用 `RegisterValue`，标量访问拒绝截断；`FPTag` 是物理非空位图。`NeverDX64FPTests` 覆盖全部 TOP、精确运算的宿主 FXSAVE/FXRSTOR 对照及上下文恢复。这不新增 checked x87 指令，也不证明全部舍入语义；缺少原生主机时明确跳过。

`driver-strict` 支持匹配的 Linux x64 主机上的 KVM 和 Windows x64 主机上的 WHP；`auto` 选择对应原生传输，跨 ISA 执行选择 Unicorn。显式 Unicorn 和原有 V1 API 保留可移植软件配置。原生执行在进入 CPU 前检查规范地址和指令效果；硬件不可用时明确失败且不回退。未支持的指令及 OS 行为仍明确报错。Windows x64 原生 CI 在关闭 Unicorn 的配置下通过全部 359 项必跑检查：131 项 CPU 检查、26 个内置映像与 46 个 WDK 映像及 40 个场景组合在首选和重定位地址产生的 224 项驱动结果，以及 4 项 SEH 边界检查 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). 原生 ARM64 的实机证据仍待补充，这不表示兼容任意驱动或 Android/Darwin 环境。

上面的原生验证覆盖已声明的驱动入口和已发布场景。下文的逐功能回归以及 C API／CLI／Python 检查，除非明确记录了 Windows 执行结果，其证据范围仍限于 Linux；原生样例集通过不代表每一种测试变体都已在 Windows 验证。

使用 `executionCapabilities(Contract, ISA, Backend)` 查询所选后端的能力配置。`NativeLegacyX64` 描述原生 x64 驱动执行；`NeverDNativeDriverTests` 验证原有驱动集，也可在关闭 Unicorn 的构建中运行。

现有 CI 工作流在通用测试配置前运行完整模拟测试目录，并在 `emulation-focused` 保存发现清单、JUnit 结果和 CTest 日志。其他模块的失败不会阻止这组测试执行。硬件不可用及可选驱动样例缺失仍明确记录为跳过；软件运行或编译通过不能作为原生执行证据。

在 Linux 上，`NeverDUnicornDeadlineTests` 通过受控的 pthread 调度，让实际计时线程在客体入口前完成。测试覆盖 x64、ARM32 和 ARM64，要求入口前取消不产生客体效果，并验证下一次运行使用独立预算。测试调用公开引擎 API，不修改引擎私有状态。

`NeverDX64ExceptionTests` 中的 `X64StateTransition` 在原生 CPU 上执行独立 RAM 读取和 CR8 读取，交替改变 TLS 基址与权限级，在重复除法异常后恢复，并在取消进入后更改 TLS。修改原生状态传输时，应运行所属 CTest 标签，同时覆盖别名重映射、CPU 上下文、FP 状态和原始驱动结果对比。KVM/WHP 不可用仍显式跳过。


`NeverDKvmRunTests` 无需 `/dev/kvm` 即可验证 `KvmRunControl` 借用的传输回调。`StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` 检查准备、读取和被拦截的宿主进入使用同一线程，且中断重试期间只准备一次。其他用例覆盖准备失败而不进入、读取失败、准备期间停止及活动进入被取消；随后重新运行，确认不会复用旧回调。验证仍应包含真实取消、RAM 回滚、异常和原有驱动测试。 `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` 验证同一期限内的多次进入复用该线程，每轮传输只执行一次，并保持此前状态包不变。

`KvmHandoffPolicy` 将每次轮询等待限制为 8 μs，连续两次未命中后改用阻塞等待，并在 256 次交接后重试。调用线程和工作线程分别自适应；调用线程同时遵守原始期限和停止标记。原子就绪标记只提示调度：数据包、回调生命周期和取消确认仍由互斥量管理。`NeverDKvmRunTests` 检查无效轮询的上限、恢复、对端延迟变化及数据包复用前的取消确认。

KVM x64/ARM64 通过 `KvmRunControl` 在同一专用 vCPU 工作线程上准备状态、进入 `KVM_RUN` 和读取状态。`EINTR` 重试只准备一次；取消进入或读取失败不能发布状态。`KvmAArch64Machine.cpp` 在该线程上执行地址转换维护及完整标量、向量传递，并共用一次单步期限。调用线程只在确认完成后提交；ISA 解码、RAM 事务、OS 策略和观察器仍属于调用线程。ARM64 原生运行仍缺少实机证据。

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` 在宿主修改通用寄存器、首尾 XMM 寄存器、MXCSR 和 x87 控制字后，验证连续执行及真实 CPU 写入。停止进入后的实际 `FXSAVE64` 字节验证全部物理 80 位寄存器、TOP、标签、操作码和指针；重复除法异常也会使复用失效。这些机器边界测试不向 checked 配置开放额外的 x87 指令。

`NeverDKvmStateTransferTests` 在真实 KVM 执行后注入寄存器或 XSAVE 读取失败，然后用未改变的输入重试。独立的整数和打包字节结果证明失败的读取不会复用已经前进的原生状态。只有该测试程序包装 `ioctl`；原生主机不可用时明确跳过。

`NeverDKvmStateTransferTests` 还在真实 KVM 上覆盖 `KVM_CAP_SYNC_REGS` 缺失、单独支持和组合支持，以及能力查询失败。`SynchronizedCapturesRemoveOnlySupportedReadIoctls` 统计真实读取调用并核对连续单步后的完整 CPU 状态；`CancelledWarmEntryRequiresFreshSpecialStateOnRetry` 要求取消后重新读取特殊寄存器。捕获失败、整数/SIMD 重试、推测 RAM 回滚和异常优先级使用相同能力矩阵；原生覆盖不可用时明确跳过。

Checked ARM64 使用统一的完整状态提交边界。`Registers.def` 定义 39 个标量字段及 32 个 128 位向量寄存器；`captureAArch64State` 暂存全部读取、应用声明位宽及 NZCV 规范化，最后一次提交。Unicorn、KVM、WHP 和 HVF 传递相同清单，包括 TPIDR_EL0、TPIDRRO_EL0、TPIDR_EL1、FPCR 和 FPSR。原生适配器通过 CPACR_EL1 开启 FP/SIMD 访问。任一标量或向量读取失败、进入取消，都会保留完整调用方状态。

ARM64 KVM/WHP/HVF 初始化执行私有 `AArch64MachineProbe.def` 程序：NOP、向正无穷舍入的 FP32 加法和双通道 SIMD 加法。每步比较全部 39 个标量字段与 32 个向量，包括 TLS、NZCV、目标寄存器高位清零及保留和累积的 FPCR/FPSR 状态。自检只使用特权级监控存储，共享一个总截止时间。自检仅证明有界初始化。Linux ARM64 KVM 和 Windows ARM64 WHP 的工作负载验证仍待完成；macOS 原生结果记录于 [HVF 指南](macos-hvf.md)。 程序还包括密钥关闭时的 A/B 返回地址签名和认证，以及非防护页上的四种 BTI 指令。

自检还执行两次 `MRS CTR_EL0`，以及 `DC CVAU`、`DSB ISH`、`IC IVAU` 和 `ISB`，核对缓存几何信息稳定及完整状态。Checked EL0/EL1 接纳原始指令、所有具名基线 DSB 选项及 ISB SY。CTR 来自选定虚拟 CPU，不同传输可以不同。缓存目标必须在当前权限下指向可读普通 RAM，允许非对齐地址和别名；其他目标明确报未支持。维护操作不产生数据读写观察事件。投影保证指令执行的一致性，不模拟私有缓存内容或并行硬件 SMP。`NeverDAArch64CacheTests` 检查完整状态、只读页尾、拒绝项、停止、上下文、预算，以及通过跨页 RW/RX 别名更新 guest 代码；不可用的 KVM/WHP 主机明确跳过。

x64 KVM/WHP/HVF 原生初始化在私有 supervisor 页面执行 `X64MachineProbe.def`。一个时限覆盖 NOP、向正无穷舍入的 FP32 加法、双通道 SIMD 加法、FS/GS 加载及 CS/SS/CR8 读取；每一步比较完整的标量、XMM、物理 x87 和控制状态。x64 与 ARM64 自检都必须取得物理内存的独占执行租约。`MemoryProjection` 统一保存缓存身份（ISA、地址空间、映射代次、权限及监控变体）和各 ISA 已提交的页表根历史。构建器在改写私有字节前使缓存失效；失败的重建不能复用部分写入的页表，调用者也不能传入过期页表根。自检仅证明有界初始化。Linux ARM64 KVM 和 Windows ARM64 WHP 的工作负载验证仍待完成；macOS 原生结果记录于 [HVF 指南](macos-hvf.md)。

共享 XSAVE 解码器区分标准格式与压缩格式的 SSE 初始状态。XSTATE_BV[1] 清零时，两种格式都初始化 XMM 寄存器；标准格式仍读取并校验 MXCSR，压缩格式才初始化 MXCSR。`X64XsaveCases.def` 提供独立的数据布局和原创主机 XRSTOR 程序。`X64XsaveTests.cpp` 检查拒绝状态的原子性，并以真实主机执行对照两种格式，同时保留调用方 FP/SSE 状态。主机架构或所需指令功能不可用时，对照测试明确跳过。

`X64FPState.def` 声明压缩 AVX、AVX-512、CET_U/CET_S 和 AMX 传输布局，包括分量的 64 字节对齐。存在的扩展数据必须符合架构的全零初始状态；缺席分量的数据与对齐填充不定义状态。偏移由布局位决定，未知布局、非初始数据或错误长度会在发布前失败。`CompactedOffsetsFollowLayoutRatherThanPresentBits`、`WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`、`InitialCETComponentsDoNotHideFPState` 和 `InitialWideComponentsDoNotHideFPState` 覆盖 872 字节及 10752 字节 WHP 数据包。这项传输支持不准入上述扩展指令。

`WhpXsaveRegisters.def` 使用具名 x87/SSE 控制寄存器补充完整 XSAVE 数据包。最后操作码及指令/数据地址会显式写入并从主机回读；可补齐数据包中的零值字段，但非零元数据冲突或共有控制字段不一致时，会在发布状态前失败。`NamedMetadataRestoresOmittedPacketFields` 验证字段缺失场景，并保留完整 FP 数据。

原生 `FOP/FIP/FDP` 遵循宿主 x87 保存、恢复规则。没有未屏蔽的待处理异常时，AMD 可能清零这些字段；快照保留实际观测值。`X64MachineProbe.def` 与精确 NOP/上下文测试使用一致的待处理异常状态，确保每个字段有效并逐项比较，不屏蔽差异。宿主进程 FXRSTOR64/FXSAVE64 参考程序覆盖两种状态；后端不会用输入元数据替代宿主结果。

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` 对比客户代码实际执行 FXSAVE64 写入 RAM 的完整 FP/SSE 状态与宿主 XSAVE 回读。两种 API 都测试直接安装和客户内 FXRSTOR64，并分别使用默认指针保存特性及显式选择的宿主支持设置，以区分进入、客户执行和捕获边界。测试不修补返回值；不一致仍然失败。 边界矩阵还覆盖未屏蔽的待处理 x87 异常，并记录宿主进程直接执行 FXRSTOR64/FXSAVE64 的参考结果和处理器厂商，以区分条件式指针保存语义与 WHP 状态传输行为。

共享的 `encodeX64XsaveState` / `decodeX64XsaveState` 编解码层拥有标准及压缩 FP/SSE 数据包、物理 TOP 轮转、缺失组件的初始状态和原子校验。WHP 使用完整 XSAVE API，优先选择 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`，旧 XSAVE API 作为兼容路径。单独的旧 x87 寄存器接口不能替代完整数据包。非初始扩展组件、畸形头部、非法控制位和截断捕获明确失败。WHP 映射错误保留 HRESULT、GPA 和大小以便诊断。

`CheckedX64Instructions.def` 通过既有 CPU 后端准入 8/16/32/64 位无符号 `MUL` 和 `CBW/CWDE/CDQE/CWD/CDQ/CQO`。`NeverDX64IntegerTests` 使用独立的 `X64IntegerCases.def` 编码和预期值，在两种特权级验证部分寄存器保留、32 位零扩展、乘积高低两部分、已定义的 CF/OF 结果及符号扩展不改变标志位。普通 RAM 乘法保留完整访问范围的权限检查和读观察回调；故障或观察回调中止会保留隐式输出寄存器及 PC。设备操作数仍不支持。这些用例也在 checked Unicorn 上运行；不可用的原生后端明确跳过。

`X64BitInstructions.def` 支持 16/32/64 位寄存器及普通 RAM 的 `BT/BTS/BTR/BTC`。寄存器位索引按操作数宽度解释为有符号数并选中完整数据字；立即数索引限制在基址的数据字内。地址宽度截断先于 FS/GS 基址相加。CF 与写入值由处理器提供；`RAMTransaction` 在观察回调接受前保留私有执行结果。完整范围权限检查覆盖独立页面分配和别名。停止、回调失败或页面权限不足均保留原始 CPU 和 RAM。LOCK 仅支持自然对齐的内存修改形式；MMIO 和硬件并行 SMP 仍不支持。`X64BitStringTests.cpp` 使用独立编码与 x64 本机实际执行对照，检查负索引、宽度截断、跨页访问、取消及非法 LOCK 形式。参见 [Intel 指令参考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。

`X64StringInstructions.def` 统一管理普通 RAM 上 8/16/32/64 位的 `MOVS/STOS/LODS`；`CLD/STD` 只改变方向标志。每个 REP 元素在观察回调前验证整个操作数，并在一个可恢复边界提交。后续故障保留此前完成的元素；取消或回调异常不改变当前元素。FS/GS 仅作用于源地址，且在地址宽度截断之后相加。AL/AX 加载保留高位，EAX 加载零扩展。32 位地址模式的零次 REP 要求计数高位为零，MOVS/STOS 还要求参与的地址寄存器高位为零，否则不同真实 CPU 实现会产生不同结果。MOVS/STOS/LODS 的 REPNE 形式及 STOS/LODS 设备操作数仍不支持。`X64StringTransferTests.cpp` 用独立的主机指令对照宽度、方向、重叠和零次数，并分别检查权限、别名、回绕、故障和恢复。原创 WDK 资源驱动通过 `driver_resource_strings.def` 执行四种宽度的 STOS/LODS。

`X64StringInstructions.def` 还统一管理普通 RAM 上 8/16/32/64 位的 `CMPS/SCAS` 及 `REPE/REPNE`。每个元素在观察回调前验证全部读取操作数，更新六个算术标志，并在首次满足终止条件时退出。数据故障恢复本次连续 REP 执行开始时的标志，同时保留已完成的指针和计数更新；公开接口恢复执行时，以已发布的 CPU 状态重新开始。停止和观察回调异常不改变当前元素，提前终止也不会读取下一个元素。FS/GS 仅影响 CMPS 源地址；SCAS 保留累加器和未使用的源寄存器。设备操作数及有歧义的 32 位零次数高位状态仍不支持。`X64StringComparisonTests.cpp` 用独立主机指令对照标志、方向、别名、回绕、权限和恢复，并通过 Linux x64 信号测试读取真实故障时的寄存器。原创 WDK 资源驱动通过 `driver_resource_strings.def` 执行四种宽度的两类条件重复形式。参见 [Intel 指令参考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。 Linux 原生验证覆盖首元素执行前后发生的故障，并区分 Intel 恢复入口 flags 与 Hyper-V 下 AMD EPYC 7763 保留最后一次比较 flags 的行为（[原生观测](https://github.com/NeverSight/NeverD/actions/runs/37202522130)）；未知 CPU 厂商会明确失败。所有后端的 checked 来宾仍统一恢复入口 flags。

存活的 WHP CPU 共享一个原生分区；最终关闭与重新创建由同一注册表锁串行保护。`WhpResourceCache.h` 复用协作式 VP 0；切换逻辑 CPU 前先销毁该 VP 并撤销其映射。并行 CPU 保留独立 VP 和私有 GPA 区间。寄存器、XSAVE 和取消请求均指向各自的 VP。x64 保留宿主默认 XSAVE 功能集，并通过 `WHvGetPartitionProperty` 验证实际分区配置。默认调度仍为协作式。

`NeverDX64FPTests` 检查全部 79 个启动状态损坏位置，并在原生传输上执行独立汇编的 `X64ProbeCases.def` 指令，验证单一时限及来宾 RAM 不变。`NeverDProjectionCacheTests` 覆盖调用者切换、ISA 顺序、页表根历史、权限/监控变体、映射代次、地址空间身份及失败重建。`NeverDRunControlTests` 中的 `WhpXsaveTests.cpp` 检查新旧 API 数据包、所有 TOP、大小边界和失败时状态不变；内存协议测试不能替代 WHP 原生证据。不可用的原生传输明确跳过。

XSAVE 校验诊断区分长度查询、本地数据准备和捕获数据解码，并保留 API 名称、返回字节数、容量及有限的头部/控制字段；独立预期位于 `WhpHostFailureCases.def`，不打印客户寄存器载荷。`InvalidInputReportsPreparationWithoutHostMutation` 还验证无效输入不会调用主机或修改其数据。共享 ISA 编解码器仍是唯一校验入口。

WHP 在能力查询、分区/虚拟 CPU 初始化、寄存器/XSAVE 传输及执行中的主机调用失败，均保留 HRESULT 和 `WhpProtocol.def` 中声明的 API 名称；能力查询失败仍返回带类型的不可用结果。 `WhpHostFailureCases.def` 提供独立错误预期，覆盖与取消同时发生的主机失败，以及新版/旧版 XSAVE 查询、安装和捕获失败。 Windows 专项 CI 要求 210 项原生用例通过：16 项映射、2 项启动、10 项 FP/上下文、7 项共享 CPU、8 项整数用例，以及 `NativeInstallRetainsFPStateBeforeAnyGuestExecution` 的两种 API 变体。后两项在执行客户代码前对比完整 FP/SSE 与独立读取的元数据。 缺少注册、跳过、禁用或未运行都会使原生证据审计失败。 新增的 26 项检查覆盖 `X64BitStringTests.cpp` 在两种特权级下的全部用例。 Windows PE64 要求 67 项 WHP 进程用例和十一项独立原生 Windows 对照用例。

`NeverDMemoryLifecycleTests` 独立于 Unicorn 构建，也覆盖仅启用原生后端的配置。禁用 Unicorn 时，专用的软件投影/设备用例明确跳过；匹配主机的共享 CPU 用例仍会注册。`WhpMemoryTests.cpp` 使用 `WhpMemoryCases.def` 中的 16 个用例隔离原生内存 API：单页/投影大小的后备内存、共享/独立分配、未触页/已驻留字节，以及存在/不存在第一个虚拟处理器。每个案例保留两个存活的逻辑所有者，反复切换其映射分区，销毁非活动所有者，并验证剩余映射无需重建即可继续使用。真实映射错误保留 HRESULT 并使测试失败；这是内存 API 证据，不是指令执行证明。

`X64MachineProbe.def` 的启动诊断列出失败指令，以及所有不一致的标量、TLS、特权级、x87 控制字段、物理 FP 通道和 XMM 字，并保留预期值及观测值。`DiagnosticIdentifiesStepFieldAndBothValues` 使用独立的预期消息验证。状态比较仍要求完全一致；诊断用于区分传输丢失与指令执行问题，失败的原生探针仍判为失败。

`WhpResourceTests.cpp` 覆盖缓存复用、替换前销毁、失败恢复及截止时间/停止竞争。`LogicalCPUSwitchingRestoresPhysicalFPAndTLS` 在两种权限模式下交替执行两个存活机器，检查独立的物理 x87/XMM 和 FS/GS 状态，并在销毁同伴后恢复剩余机器。Windows CI 要求两种权限的 WHP 案例都执行通过。

`NEVERD_ENABLE_SEMANTIC_TESTS` 默认为 `ON`，控制 `unittests/semantic` 中的测试组及其聚合运行目标。构建不依赖 Unicorn 的原生 CPU 测试时，保留 `BUILD_TESTING=ON`，同时设置 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` 和 `NEVERD_EMULATION_BACKEND_UNICORN=OFF`。原生 KVM/WHP 测试仍可构建，包括具有对应 SDK 头文件的 Windows ARM64/MSVC 配置。在 Windows ARM64 上启用 Unicorn 仍需 ARM64 LLVM-MinGW 工具链。这项构建解耦不等于 ARM64 原生运行验证。

仅原生 CPU 的 CI 检出初始化固定版本的 Capstone 源码，并使用校验过的预构建 LLVM 包。设置 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` 且禁用 Unicorn 后端后，CPU 测试目标的配置、构建和链接均不需要 Unicorn 源码，也不依赖签名库及外部语料。默认 CI 仍启用完整语义测试组。

`ci.yml` 的手动模式 `native_cpu_only` 通过 `native_cpu_backend=whp` 选择 Windows x64（默认），或通过 `native_cpu_backend=kvm` 选择 Ubuntu x64。`NativeCPUTests.def` 共享 CPU／进程验收要求，分别声明后端专属目标和用例。`run_native_cpu_ci.py --require-whp` 或 `--require-kvm` 校验宿主，先构建全部目标再执行 CTest，并保留清单、JUnit、日志和结果分类。即使 CTest 成功退出，缺少或跳过必测用例仍会失败。CI 禁用 Unicorn；`--with-drivers` 要求所选后端执行同一组原址／重定位驱动样例。编译和建立探测不能证明来宾执行或 ARM64 验收。 Ubuntu 配置使用上游签名的 Clang/LLD 21 软件包；Clang 18/19 的 CR8 声明与固定版本的 WDK 头文件冲突。 Linux 原生验收使用 CMake 4.2.3。`NeverDNativeDriverTests` 显式设置 `NO_PRETTY_VALUES`，使 CTest 保留已声明的用例名称，不依赖参数诊断输出。

原生 CI 在构建前探测 `sccache --zero-stats`。探测失败时会清空 C 和 C++ 编译器启动器，保留原有编译器配置和全部必测清单。配置或编译失败仍会使任务失败。

KVM 验收要求真实的不主动退出 vCPU 取消，以及 `KvmStateTransferCases.def` 中 48 项状态传输结果，包括 ioctl 捕获和可选能力查询失败。其他同步寄存器模式在宿主支持时执行，否则明确跳过。稳定的参数名称不依赖 ioctl 数值或元组格式。协议测试补充原生执行证据，不能替代它。

`native-host-probe.yml` 在 Linux 和 Windows x64/ARM64 托管 runner 上运行独立的 `probe_native_host.py`。`NativeHostProbe.def` 声明能力查询、VM/vCPU 创建和清理证据的顺序。报告保留源码/二进制哈希、原生宿主 ISA 和每一步宿主状态码。`setup_ready` 只证明初始化成功，不执行来宾指令。缺失的 API/设备能力记为 `unavailable`；构建、初始化、清理、超时或证据格式错误会使任务失败。ARM64 托管环境的可用性须逐次观察，这个探测不构成 ARM64 工作负载验收。 两个 Linux 工作流均通过 `prepare_kvm_ci.py`，仅向当前托管 runner 账户授予已有 KVM 字符设备的访问权限，并记录设备身份及权限；脚本拒绝本机与自托管机器，不会创建缺失的设备。

`windows-alignment-oracle.yml` 通过 `check_windows_alignment.py` 和 `WindowsAlignmentCases.def` 收集 72 项原创 x64 Windows 异常观测：九种对齐 SSE 形式分别覆盖七种非对齐地址/权限场景，以及一个已对齐但页面不可访问的对照。它保留异常代码、参数、故障 PC、保存的上下文、原始输出和源码/二进制哈希，并验证输入与 RAM 未改变。这些观测仅建立 OS 行为依据，不代表 KVM/WHP 执行验收，也不新增 SEH 支持。

在 `native_cpu_only=true` 时，设置 `native_driver_tests=true` 可启用不依赖 Unicorn 的 `NeverDNativeDriverTests`。配置前，`build_wdk_driver_fixtures.py` 校验微软官方 WDK/SDK 10.0.26100.6584 包的完整 SHA-256，并从原始源码重建 48 个普通、CFG 或 DBG 驱动映像。`WDKDriverFixtures.def` 统一声明包身份、编译和链接参数及样例绑定。未经修改的微软文件和许可证保留在本地构建或缓存目录；CI 仅上传构建元数据和日志。清单记录工具版本、命令、源码与头文件摘要以及输出映像摘要。

`NativeDriverTests.def` 要求 `DriverBuiltinImages.def` 与 `DriverBackendParityCases.def` 中全部 115 个负载产生 230 项 WHP 结果：27 个内建映像、48 个 WDK 映像及 40 个请求场景，各覆盖原始和重定位地址。完整必测清单为 `5068 CPU + 230 WHP + 25 SEH + 77 scheduling + 30 wait sets + 11 driver UNPACK + 6 clock reads = 5447`。30 项等待集合检查包含十六项可移植模型测试及十四项原创原生驱动测试。`run_native_cpu_ci.py --with-drivers` 在禁用 Unicorn 时保留精确清单和 JUnit 证据；必需样例缺失或跳过会使此可选验收失败，普通构建仍可不提供外部样例。固定位址映像保留预期的重定位拒绝。ARM64 原生客体执行仍未验证。

`InterruptionRetainsPhaseCauseDeadlineAndLease` 在两条不同启动指令前注入超时、停止及二者同时发生的中断，检查精确阶段诊断、消息自身持有的生命周期、错误类型和原因位、步骤间不变的统一截止时间及内存占用释放。既有真实传输失败与状态不匹配仍分别处理。原生 x64 启动验证预算为 `5 s`；普通客体截止时间及单步宽限不变。

`WhpResourcePolicy.def` 为 WHP x64 和 ARM64 的资源创建设置独立的 `30 s` 期限，再执行 ISA 自检。同步宿主设置完成后，发布资源前仍检查该期限。指令自检和普通 guest 执行保留各自的限制。`WhpResourceTests.cpp` 检查初始化中断的类型与原因诊断、取消后资源清理、宿主错误优先级及普通执行期限不变。

`X64PopFlagsTests.cpp` 检查两种权限及 `driver-strict`：全部 256 种允许的标志输入与两种初态、九种编码、全部 64 个输入位、只读和可执行别名、跨页故障与修复、观察器中止及失败、设备栈拒绝和后续原生指令边界。`X64PopFlagsOracle` 在 x64 主机独立执行原始指令，核验 CPL3/IOPL0 和精确栈消耗。`driver_resource_flags.def` 使原 WDK 资源驱动通过两种操作数宽度设置、清除并恢复标志。测试保留完整整数、控制、x87、SSE 状态，不代表支持客体 TF/NT/AC/ID 或已有 ARM64 原生执行证据。

`X64StatusFlagsTests.cpp` 检查 `CLC/STC/CMC`、`LAHF/SAHF`、全部 256 个 AH 输入及已接纳的标志组合、所有 REX 前缀、完整 CPU 状态、内存不变性、观察器停止或错误、保存上下文和原生 ADC/存储续行。无效 LOCK 编码无副作用拒绝。独立主机指令判据在验证 CPUID 支持后检查 24 组前缀。原有 WDK 资源驱动覆盖全部五条指令，新增 22 项原生必测结果；不可用的主机或 ISA 组合明确跳过。 可移植 Unicorn 配置运行相同的七项用例；依赖的直接测试覆盖 16/32/64 位 AH 与 LOCK 行为、显式 REX 寄存器，以及长模式缺少特性时的拒绝。

`X64DoubleShiftTests.cpp` 覆盖全部已接纳的 imm8/CL 计数、重叠与扩展寄存器、已定义标志、准确的跨页 RAM 观察、取消、权限/未映射/设备故障、LOCK 与未定义计数拒绝、上下文及原生 ADC 续行。独立主机判据检查 5,184 次原始执行，12 个原有 WDK 驱动探针覆盖寄存器与 RAM 形式。原生门禁新增 145 项必测结果。

`X64ScalarShiftTests.cpp` 检查全部字节计数、两种进位输入、零/全一及带符号操作数、隐含单次形式、AH/SPL 与计数寄存器别名、完整 CPU 状态、精确 RAM 范围、观察回调回滚、故障及上下文续执行。独立原生对照检查 65,536 次执行。WDK 资源驱动加入 72 个原创探针。KVM/WHP 验证门要求此指令族的 769 项结果通过。 只有掩码后计数为零才保证保留全部标志位。非零计数的 `RCL/RCR` 完整进位环绕会保留操作数和 CF，但 OF 未定义；对照仅排除这一未定义位。

`X64LoopTests.cpp` 覆盖 21 种原创编码、计数器回绕、4 GiB 以上的有符号相对目标、完整 CPU/RAM 状态保留、观察回调取消、跨独立映射取指、目标取指错误与上下文恢复。指令字节不完整时在执行前拒绝；目标取指失败则保留已完成分支的计数器和 PC。WDK 资源驱动新增 12 个原创探针。KVM/WHP 在内核、用户和驱动契约下必须通过本指令族的 379 项结果。 原始宿主指令对照最多运行 1,008 个样例并报告次数。AMD 上采用 `66H` 的已跳转分支会访问宿主系统保留的低地址，因此这些形式在显式映射低地址的客户机矩阵中执行。Intel、AMD 的目标宽度及 REX.W 优先级分别验证。

`X64BranchTests.cpp` 检查全部 16 种 Jcc 条件、相对 JMP、九组前缀、短/近形式、4 GiB 以上的有符号相对目标、完整 CPU/RAM 状态、观察器停止与错误、跨页解码、目标取指故障和上下文恢复。独立 Intel 宿主对照执行 9,792 条原始指令；AMD 低地址目标形式留在客体测试中。两种解码模型均测试完整和截断字节，原生探针还检查失败时不发布结果。四个原创 WDK 资源探针覆盖驱动策略。KVM/WHP 验收各增加 274 项必需结果。AMD 软件模型覆盖不代表原生 AMD 或 ARM64 执行证据。

`X64StackTests.cpp` 以九类测试覆盖 42 种编码，检查宽度、寻址、完整状态、观察器顺序、取消、权限、跨页、物理别名、故障修复、设备拒绝和上下文恢复。独立宿主对照执行原始指令，六个 WDK 资源探针覆盖驱动路径。KVM/WHP 原生验收各增加 1135 项必需结果。不可用的后端在其原生强制验收之外仍明确报告跳过。

`X64FrameExitTests.cpp` 覆盖 14 种 `LEAVE` 编码、有效前缀顺序、完整 RBP 寻址、完整寄存器状态、只读别名、跨页栈帧故障与修复、权限、观察器、无效地址、设备拒绝和上下文重放。独立宿主对照执行 42 条原始指令，五个 WDK 资源探针验证驱动执行。两个原生验收各增加 379 项必需结果。

`X64FrameEntryTests.cpp` 覆盖 14 种编码、嵌套与重叠、物理别名、只写权限探测、跨页故障与修复、观察器取消、权限、前缀拒绝及上下文重放。独立宿主对照执行 882 组成功指令，并在 Linux x64 上执行 84 组故障，逐字节检查栈和寄存器。两项注入后端测试区分取消及失败回滚与架构故障提交。六个 WDK 资源探针执行原始驱动指令。原生验收新增 508 项 KVM 和 507 项 WHP 必测结果。

`DriverSIMDSEHTests.cpp` 以四种受支持的处置及一次 x87 修改拒绝、两种原生执行契约、普通/CFG WDK 映像及两个加载地址运行八类原始 SSE 故障。十项后端专属结果和三项纯内核 SSE 记录检查均为必测。`driver_seh_simd.def` 统一定义样例与模式；异步展开表覆盖故障辅助函数。微软内核 10.0.26100.9549 提供独立分类与恢复依据：隔离执行了 107,744 组指令路径分类及 8,192 组恢复。这不代表已在完整 Windows 内核中执行驱动；ARM64 原生 KVM/WHP 仍未验证。

C SEH 作用域仍使用左闭右开区间。合法的 `__C_specific_handler` 落点可能位于其保护区间内：[LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) 将 `EndLabel + 1` 写为区间末端。Windows OS 模型保留原始端点，并独立校验目标可执行性、所属函数和续接身份，重定位后同样如此。`KernelSEHContinuationCases.def` 保留原始样例布局；`ScopeEndLabelMayOverlapTheHandlerLandingPad` 覆盖常量处理器和过滤器。配套测试验证末端排除，以及非法目标被拒绝后派发状态仍可重试。这些纯模型检查纳入 `NeverDNativeDriverTests`，禁用 Unicorn 时仍会执行。

目标展开同样使用原始作用域末端：若处理器目标仍在某个 `finally` 的保护区间内，便不会退出该作用域。`FinallyRespectsRawScopeEndAtHandlerTarget` 检查边界两侧，并在 Windows x64 上直接对照 `ntdll.dll!__C_specific_handler`。NeverD 不修补编译器生成的区间。Clang 20/21 构建的原始样例在 `T`、`J` 模式下返回来宾失败，因为偏移后的末端包含选定目标；Clang 23 构建会执行两级清理。[LLVM 修改 #144745](https://github.com/llvm/llvm-project/pull/144745) 移除了旧的 `+1` 偏移。这类编译器相关结果与后端故障分别记录。

构建清单覆盖全部原创 WDM/KMDF C 样例和可选 WDK CMake 路径。每个公开的 `driver-*-scenario.json` 都在 `DriverBackendParityCases.def` 中有普通及 CFG 用例；缺失源码、构建或场景绑定会使清单测试失败。`Original` 清除场景中的加载地址覆盖并核对映像首选基址，`Rebased` 核对声明的重定位基址。驱动自有 IRP 场景会主动取消一个子请求，因此 `DriverNativeOutcomes.def` 在正常清理后仍保留其预期的整体失败结果。

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

`NeverDAArch64StateTests` 验证每个标量字段、每个向量的两个字、特权级改变、浮点指令未执行及传输错误诊断的保留。`NeverDAArch64FPTests` 在两种特权级的真实传输上运行 `OriginalProgramChecksCompleteStateAndOneDeadline`，使用独立汇编的 `AArch64ProbeCases.def` 原始指令。测试把这些与 PC 无关的指令迁到客户代码，保持监控页仅供特权级访问。Unicorn 执行和原生后端的明确跳过不能替代 ARM64 原生启动证据。

`CheckedAArch64Instructions.def` 和 `AArch64InstructionEffects` 在 EL0/EL1 接纳有界的基础 FP32/FP64 算术、比较、移动和定宽 SIMD 运算。FPCR 支持四种舍入模式、FZ 和 DN；FPSR 保留累积状态及 QC。未支持的控制位和状态位在修改前拒绝。FP16 算术、SVE/SME、未屏蔽异常、可选扩展及未列出的形式明确失败。这些 CPU 能力不代表已经支持 Windows ARM64 驱动加载或新增 OS 环境。

`AArch64InstructionEffects` 负责标量及 FP/SIMD 的单次、成对 RAM 访问范围，单个操作数最大 128 位。共享地址空间在进入 CPU 前验证每一页；`RAMTransaction` 只提交完整声明的物理写入。128 位写观察器在生效前按顺序收到两个 64 位字。停止和故障保留 RAM、向量和地址写回。Xn/Vn 的编号重叠合法；发生地址回绕的成对访问被拒绝。`NeverDAArch64MemoryTests` 使用独立的 `AArch64CrossPageCases.def` 与 `AArch64VectorMemoryCases.def` 编码。

`NeverDAArch64StateTests` 在两种特权级验证完整状态的全部 71 个读取位置，包括位宽规范化、缺失读取器和重试。`NeverDAArch64FPTests` 执行 `AArch64FPCases.def` 原始指令，验证全部向量通道、打包运算、标量及向量浮点结果、四种舍入模式、FZ/DN、累积 FPSR、上下文状态及扩展和控制位拒绝。`NeverDAArch64MemoryTests` 覆盖每个跨页偏移、观察器顺序和停止、权限拒绝、别名及恢复后的向量输入。`NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`) 在真实执行后注入每个标量、向量读取失败。不可用的原生传输明确跳过；这些测试及交叉编译不能替代 ARM64 KVM/WHP 实机证据。

`NeverDAArch64MemoryTests` 在两种权限级下覆盖 Unicorn、KVM 和 WHP 的 18 种标量及成对指令，检查所有跨页偏移、符号扩展及宽度结果、观察器顺序、第二页权限不足或缺失、显式消费故障后重试、重复物理别名，以及别名替换后的上下文恢复。修改前已复现合法跨页加载被拒绝的情况。不可用后端明确跳过；Unicorn 验证及交叉编译不能替代 ARM64 KVM/WHP 实机证据。

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) 检查零标志的未启用 CFG 元数据、两种加载地址下不变的回退指针、无效指针槽/目标及缺失重定位。执行用例显式选择 Unicorn/KVM/WHP，并使用 `driver-strict` 和 `checked-x64-v1`；不可用的后端分别跳过。`DriverPublicCLICases.def` 为 CLI 与兼容的 v1 C API 对比选择 `--backend unicorn`。原生后端及 `auto` 选择保留独立的公共接口覆盖，主机 API 不可用时不会静默回退。

checked Unicorn 使用 `MachineRunControl`：ARM64 维护、来宾执行和完整状态回读共用一次单步额度。`UC_HOOK_CODE` 在指令入口检查借用的停止令牌和期限；同步引擎调用返回前解除 hook 借用，而机器单步保留控制直到发布状态。Unicorn 与 WHP 暂存完整 CPU 状态，并在成功步骤发布前检查同一个控制条件。WHP 在准备前只创建一次额度。已确认的 x64 CPU 异常优先于回读期间到来的停止请求。回读取消时，checked RAM 事务丢弃推测写入；非受限软件契约不变。 `MachineInterruptedError` 区分已确认取消与主机或回读失败。共享 checked CPU 返回 `Stopped` 或 `Deadline`，保留 CPU/RAM 并允许重试；真实故障即使伴随停止请求也仍是 `BackendFailure`。

状态回读回归： `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests` 在真实 x64 和 ARM64 引擎的两种权限级执行 `UnicornMachineControlCases.def` 中的原始存储指令。`RejectedEntryPreservesStateAndRAMAndAllowsRetry` 检查单步前取消、实际来宾入口处停止或期限到期、全部输入状态与 RAM 保持不变，以及随后成功执行一次存储。测试专用入口包装无需虚拟机监控器，也不构成 ARM64/WHP 原生运行证据。

`RunDeadline::invoke` 在 WHP 入口已停止或过期时拒绝调用宿主，取消期间保留真实宿主结果，并在释放借用的停止标记前确认中断回调结束。KVM 和 WHP 在持有执行租约的调用线程上验证完整捕获的私有状态，然后分类同时到达的停止或超时。真实宿主错误、捕获失败以及经过认证的 x64 CPU 异常保持更高优先级。普通成功状态在取消检查结束前保持私有；已确认的中断丢弃推测性的 CPU/RAM 效果并允许重试。准备、原生执行和捕获共用一次单步宽限。这些控制提供协作式取消，不保证硬性墙钟时限。

`NeverDRunControlTests` 包含可移植的 `NativeEntryTests.cpp` 和 Windows 启用 WHP 时的 `WhpEntryControlTests.cpp`。内存宿主回调验证拒绝入口、重试、晚到取消、真实错误保留、完成结果优先级和已确认的回调生命周期，无需 Hyper-V。`NeverDKvmRunTests` 检查调用线程完成、错误优先级及重复进入拒绝。真实 `NeverDKvmStateTransferTests` 执行 `KvmStateTransferCases.def` 原始指令；`ActualCPUExceptionOutranksStopDuringCapture` 和 `PublicCPUExceptionOutranksStopDuringCapture` 在真实寄存器/XSAVE 读取后停止，并保留除零异常、原始上下文、RAM 和显式恢复。Wine 上采用 Windows ABI 执行的可移植测试仅提供线程及控制协议证据，不证明原生 WHP 执行。不可用的原生后端仍明确跳过。

`NeverDInstructionFetchTests` 通过 Unicorn、KVM 和 WHP，在内核态和用户态执行 x64/ARM64 checked 程序。`InstructionFetchCases.def` 覆盖操作数形式切换、相对分支、客户机和宿主通过代码别名写入、上下文恢复、权限撤销、独立页面存储、页尾预读、非法或截断编码及递归执行拒绝。Windows 原生 CI 要求全部 x64 WHP 用例通过。不可用的宿主/ISA 组合明确跳过；可移植 ARM64 执行不构成原生 ARM64 支持证据。

`WhpStateTransferTests.cpp` 对两代 XSAVE API 注入寄存器传输，检查精确变化组、完整捕获、填充忽略、部分失败、取消、异常优先级及分区重建。`ContinuedStepsReuseCapturedRegistersAndFP` 统计省略的安装；`PartialTransferFailuresPreserveStateAndForceFullRetry` 要求完整恢复。这些是协议检查，不是原生执行证据；既有原生 FP、状态转换、驱动和 ring3 测试仍为必要验证。

`CancelledDirectRunPublishesACompleteBoundary` 验证 direct 执行确认取消后发布的完整状态。`FailedDirectCapturePreservesStateAndForcesFullRetry` 要求取消期间寄存器、XSAVE 或元数据捕获失败时保留调用方状态，随后完整重试；两代 API 都不得发布部分寄存器前缀。

`WhpStateTransferCases.def` 还覆盖合并后的 32 寄存器读取中每个部分前缀失败，以及全部七个元数据字段冲突。两代 XSAVE API 均须保持调用方状态并在重试时完整恢复。同一套测试核对每步仅一次寄存器读取，以及从该读取补齐 XSAVE 省略的元数据。

`windows-pe64-v1` 支持有界 Windows x64/ARM64 控制台进程，包括 PEB/TEB、静态和动态 TLS、`DllMain`、具名 Win32 API 和显式无环 DLL 图。客户模块支持按名称／序号导入代码及数据、DIR64 重定位、转发导出和真实加载器链表身份。`LoadLibraryA`／`LoadLibraryW`、`FreeLibrary` 和 `GetProcAddress` 使用配置的模块目录。CRT／GUI、ARM64 基于栈帧的用户态 SEH、线程和通用 Windows 应用兼容性仍待完成；原生 ARM64 KVM/WHP 证据仍缺失。

输入文件总字节数和映像总范围各自受 `memory_limit` 限制，运行时映射也计入映像预算。准备阶段共享 65,536 条记录、64 MiB 元数据读取、名称长度和整个任务的截止时间限制；阻塞式主机 I/O 不保证硬实时。原创 EXE→DLL→DLL 样例检查重定位指针、序号调用、共享数据、API 指针身份、`MEM_IMAGE`、加载器链表及 EXE TLS 挂接／分离。`NeverDWindowsProcessTests` 包含这些检查和直接原生 Windows 对照；`NeverDPEProgramExportsTests` 验证畸形元数据及资源计费，`NeverDProcessPublicTests` 验证 C ABI/CLI 模块目录一致性。不可用后端明确跳过。

`WindowsProcess.ClockServicesUseConsistentUnitsAndPreserveLastError` 执行原始 x64/ARM64 PE 中的 `QueryPerformanceFrequency`、单调计数器、FILETIME、回绕 tick 计数和相对延迟调用。`WindowsProcess.UnmodeledDelaysStopWithoutClaimingCompletion` 验证警觉、正数绝对时间及 INT64_MIN 间隔在完成前被拒绝。`NativeWindowsOracleRunsTheSameExecutable` 也在 Windows 上直接运行成功的时钟情境；两项客户回归均纳入禁用 Unicorn 的 KVM/WHP 必测清单。 原始样例显式污染 `BOOLEAN` 参数寄存器的未使用高位，并以明确的 64 位类型定义常量，防止 Windows ABI 截断纪元值与 INT64_MIN。

`ARM64 native backend build` 使用固定版本的 LLVM 源码，在 `ubuntu-24.04-arm`（KVM）和 `windows-11-arm`（WHP）上关闭 Unicorn 并编译 `NeverDEmulationNative`。`audit_native_backend_build.py` 核对每个声明的原生源文件、启用的后端定义、编译命令、ARM64 ELF/COFF 对象及哈希。`probe_native_host.py` 记录宿主初始化能力和资源清理；能力不可用会明确记录，初始化错误会使任务失败。这些任务验证编译和宿主初始化，尚不构成客户机执行证据。 `NeverDCapstoneCompilerOptions.inc` 将限定符诊断选项限定于 Clang 的 C 编译；GCC 和 MSVC 保留各自的告警规则。 `native_arm64_only=true` 可单独运行这些 ARM64 组件构建与初始化探测，不启动完整 x64 CPU 验收。 构建审计会先规范化源码和构建目录，再比对路径，包括 Windows 8.3 别名。 ARM64 组件任务会在构建审计前显式启用选定的 KVM 或 WHP 后端。

`WindowsTestExecution.def` 将 Unicorn ARM64 的 `WindowsExclusive` 对照设为 `RUN_SERIAL`。CTest 策略避免它与其他客户负载争用资源，保留原有 60 s 客户机截止时间及全部结果、寄存器、权限和原生摘要检查。

`run_native_cpu_methods.py` 依据 `NativeMethodExecution.def` 校验布尔属性 `RUN_SERIAL`，并在方法分组及跨运行执行契约中保留该约束。各方法依次执行；子进程未退出时不会启动下一项。未知属性和被修改的分片契约仍会使验证失败。

`WindowsProcessLifetime` 在同一个 CPU 和执行预算下，按依赖顺序执行 DLL TLS 回调及 `DllMain`，随后执行 EXE TLS 和入口。每个模块都有独立 TLS 索引及对齐的数据块，从完成重定位和导入绑定的映像复制，共享 64 KiB 空间。TLS 保留参数为零，启动／进程退出的 `DllMain` 接收不透明非空值。显式进程退出按加载器链表的逆序分离已完成初始化的 DLL，再执行 EXE TLS 退出回调，即使 EXE 初始化尚未运行。启动 `DllMain(FALSE)` 以 `0xc0000142` 退出，不发送分离通知。故障和预算耗尽不伪造清理。带客户 DLL 的 PE 入口返回涉及尚未支持的线程终止，明确停止。非零 `SizeOfZeroFill` 仍不支持；实际 TLS 模板中的零初始化字节受支持。 无入口 DLL 接收 TLS 挂接通知，但不接收进程分离通知。

`WindowsProcessExports` 为静态导入和 `GetProcAddress` 共用名称／序号解析，覆盖代码、数据、别名及链式转发。 只有实际引用的启动转发才引入目录中的模块和初始化依赖，未使用的转发不加载文件。 导出名称区分大小写；名称缺失返回 NULL／错误 127，直接查询缺失序号（包括空洞）返回 NULL／错误 182，查询参数为空指针返回错误 87，成功保留 LastError。 未知模块句柄仍不支持。 有界 API 清单按精确提供方／名称一次性保留调用入口。 解析检查每个查询映像的实时 PE 头和导出元数据，拒绝修改或不可读字节，转发链最多 64 项，并共享准备阶段剩余的元数据额度及执行截止时间。 转发到空洞时返回目标映像基址并保留 LastError；转发到零序号返回错误 87。 返回基址是数据地址，不授予映像头执行权限。 运行时转发可以加载配置目录中的模块，并在返回查询结果前完成初始化。仍不支持实时改写导出表。

`WindowsProcessLoader` 从 `windows.modules` 加载 ASCII DLL 基名，统一管理显式引用、共享依赖和启动模块保留。重复查询转发导出不会增加额外引用。模块目录槽位在重载时使用新的驻留代次。TLS 和 `DllMain` 在同一 CPU 上、被暂停 API 的栈帧下方执行；恢复寄存器保留客户内存写入，并使用实时返回地址。动态附加／分离的保留指针为零。显式加载期间的附加失败在清理后返回错误 1114，同时保留已成功的独立嵌套加载。卸载释放映像映射和 TLS，重载恢复原始映像内容。模型之外对加载器链表或 TLS 指针的修改会明确失败。失败和重载都不会重置文件、映像与元数据工作额度。系统提供方以已映射 PE 的基址作为模块句柄。文件系统搜索、非 ASCII 路径、`LoadLibraryEx` 标志、循环导入及正在初始化或卸载的同一模块的重入转换仍不支持。

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 共用 PEB 进程参数中的实时客户环境块。名称限 ASCII 且忽略大小写，值为 UTF-16。修改前校验输入、容量及可写内存。快照不受后续修改影响，释放时回收客户内存。模型的环境块上限为 64 KiB；字符串与展开操作有明确边界并检查工作负载截止时间。未知指针归属、格式错误的环境块、ANSI 代码页及展开缓冲区重叠仍不支持。`WindowsEnvironmentTests.cpp` 在可用后端比较原创 x64/ARM64 样例，CI 必须执行独立的原生 Windows 对照。

`WindowsProcessHeap` 统一管理进程堆的分配、`HeapReAlloc`、释放和尺寸查询。调整大小保留原有有效数据；`HEAP_ZERO_MEMORY` 清零新增字节，`HEAP_REALLOC_IN_PLACE_ONLY` 禁止搬迁。重分配失败时保留旧块，返回 NULL 并设置 `ERROR_NOT_ENOUGH_MEMORY`（8），与原生观测一致。独立页内存使收缩和释放能归还容量，分阶段扩容及有界复制检查工作负载截止时间。自定义堆、异常生成标志、未知归属以及不可访问的复制或清零范围均明确停止。`WindowsHeapTests.cpp` 覆盖两种 ISA、强制搬迁、预算复用和失败原子性；CI 也在原生 Windows 上运行同一原创 EXE。 PE 批量用例采用测试框架统一的 120 秒 CTest 外层限时，每个客户工作负载仍有独立的有限预算。堆样例为每个进程保留 20 秒，以便 WHP 完成全部数据校验。

`WindowsSystemModules` 为两种 ISA 构造有界的 `ntdll.dll`、`kernelbase.dll` 和 `kernel32.dll` PE64 模型映像。ASCII `GetModuleHandleA` / `GetModuleHandleW`、`LoadLibraryA` / `LoadLibraryW` 与 `GetProcAddress` 共用其映射基址；PEB/LDR 和 `MEM_IMAGE` 描述同一批映像。静态导入、按名称查询和客户 DLL 转发使用相同 API 跳板与导出解析器。提供方固定驻留，不执行客户初始化回调，普通客户 DLL 全部卸载后不会阻止入口返回。头部或导出元数据改变会停止查询。未知系统导出名称和非零系统序号查询明确停止；已建模名称的大小写不匹配和空名称返回错误 127，空指针查询返回 87。生成的字节和地址属于模型策略，不复刻特定 Windows DLL 布局、原生序号或跨提供方别名。`WindowsSystemTests.cpp` 对照原始 x64/ARM64 EXE 与原生 Windows，并独立观察八次初始线程返回。

`WindowsSectionFixture.inc` 检查两个独立映像视图、句柄复用、关闭句柄后的读取、写入后的视图隔离、解除映射及原驻留模块的保留。负例覆盖命名空间、访问权限、固定地址和偏移。`WindowsThreadFixture.inc` 分别检查亲和性与隐藏状态、精确长度和带杂值的参数高位。原始样本也纳入 Windows 原生对照；缺少 `KnownDlls` 命名空间的 Wine 无法验证 section 场景。

`WindowsProcessExceptions` 在同一 CPU 和进程预算内实现 `AddVectoredExceptionHandler`、`RemoveVectoredExceptionHandler` 和 `RaiseException`。有序处理器可注册或移除处理器、触发嵌套异常、调用已建模 API、加载 DLL 以及退出进程。x64/ARM64 数据访问异常和 x64 整数除法异常可在校验客户对 `CONTEXT` 的修改后恢复；通用寄存器、SIMD 和受支持的浮点状态会保留。软件异常经模型提供方中的真实返回指令继续执行。模型限制为最多保留 128 个注册项、嵌套 16 层。非法处置值、被修改的异常指针、不支持的上下文字段和超限均明确失败。ARM64 基于栈帧的 SEH／展开、调试器派发及执行／保护页异常仍不支持。`WindowsExceptionTests.cpp` 将原创 EXE／DLL 场景与原生 Windows 对照；原生 ARM64 KVM/WHP 证据仍待补齐。 软件异常记录带有 `EXCEPTION_SOFTWARE_ORIGINATE`（`0x80`），与调用者传入的不可继续标志分别处理；原始 Windows 可执行文件精确核对软件异常和硬件异常的标志值。

`WindowsProcessContext` 保留每个派发帧的来源。已支持的 x64 数据访问和除法故障在 `CONTEXT.EFlags` 中呈现 RF（`0x10000`）；`RaiseException`（包括软件抛出的访问违规码）保留当前上下文。来源信息贯穿 VEH/VCH 和 SEH 搜索／展开。合法继续执行时恢复不含 RF 的逻辑 CPU 标志；客户修改 RF 会在发布状态前被拒绝。此受限配置不模拟指令断点或客户控制的 RF。`WindowsExceptionTests.cpp` 检查保存记录、恢复，以及拒绝时 CPU／RAM 不变。

Windows ring3 按独立原生观测，将 checked x64 的 `operand_alignment` 故障映射为 `STATUS_ACCESS_VIOLATION`，参数为 `[read, UINT64_MAX]`，存储指令也相同。原因由 CPU 层提供，Windows 不凭向量 13 猜测或重新解码指令。`WindowsAlignmentProcessTests.cpp` 执行原始 PE 指令，覆盖 72 种故障情境和 9 次地址修复重试（`72 + 9`），检查 PC、RF、XMM 和 RAM。未分类或字段不一致的故障仍会拒绝。进程与驱动故障报告保留可空的 `cause` 和十六进制 `error_code`，区分缺失与零。此派发适用于 checked x64 用户态执行契约。 每次故障或修复重试后，样例都会导出完整的 4096 字节页面；宿主核对全部 81 份快照及实际完成计数，来宾时限保持不变。 原生进程及初始线程观测使用 `CREATE_DEFAULT_ERROR_MODE`：GoogleTest 会启用可继承的 `SEM_NOALIGNMENTFAULTEXCEPT` 标志，使 Windows 自动修复正在测量的故障。因此原生验证使用系统默认行为，避免测试框架策略干扰结果。

`AddVectoredContinueHandler` 和 `RemoveVectoredContinueHandler` 管理独立的有序列表，与异常处理器共用最多保留 128 个注册项的限制。向量异常处理器接受继续执行后，继续处理器读取同一份可修改的异常记录和 `CONTEXT`；最终上下文校验在这些回调完成后进行，包含嵌套异常与 DLL 通知。两类处理器的句柄不可交叉移除。`WindowsContinuationTests.cpp` 将顺序、提前结束派发、增删、上下文修复、嵌套派发、加载器回调及进程退出的原创 EXE 场景与原生 Windows 对照。已测 Windows x64 向量处理路径允许在设置 `EXCEPTION_NONCONTINUABLE` 时继续执行；这不代表基于栈帧的 SEH 行为。原生 ARM64 执行仍未验证。

`RtlCaptureContext` 已通过 `kernel32.dll` 和 `ntdll.dll` 支持 x64、ARM64。共享的 `WindowsProcessContext` 与 `IntegerABI` 保存调用者 PC/SP，不修改 CPU 状态或 LastError。原生 Windows 观察确认 x64 标志为 `0x10000f`，未涉及的 home／调试／向量存储保持原样，x87 地址字段保留传统的低 32 位；ARM64 从 LR 保存 PC，并清零记录中的 X0/LR。寄存器、SIMD 与浮点控制来自客体；x64 选择子和 MXCSR 能力掩码遵循配置的客体 CPU。无效、未对齐或部分不可访问的目标记录在写入前明确失败。`WindowsContextTests.cpp` 覆盖静态导入、提供者查询、VEH 回调、跨页输出及失败原子性。`scripts/check_windows_context.py` 在原生 Windows x64、ARM64 上运行原创程序，并单独验证非空 x87 状态。这些 ARM64 API 观察不代表原生 KVM/WHP 执行验证。上下文恢复、栈回溯和动态函数表仍需继续实现。 `WindowsProcessServices.def` 声明精确的模块限制：模型在 `kernelbase.dll` 中查询此符号时返回 `ERROR_PROC_NOT_FOUND`（127），与原生观察一致，不凭空增加导出。 [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` 使用 `os/windows/exception/` 中共享的 `X64SEH`（`NeverDEmulationWindowsException`，无需启用驱动环境），处理 x64 `__C_specific_handler` 和 UNWIND_INFO V1。VEH 搜索结束后支持过滤器、finally 回调、非局部处理器跳转、嵌套／冲突展开以及重定位 EXE/DLL 栈帧，保留非易失 GPR/XMM 状态。过滤器选择继续执行时，VCH 使用同一份 `CONTEXT`。`WindowsSEHTests.cpp` 将 23 个原创场景与原生 Windows 对照；KVM/WHP/Unicorn 共用这些语义。派发在进程预算内重新校验映像代次、头部、展开／作用域字节、语言处理器代码区域及 IAT 绑定。元数据被修改或保留的映像被卸载时明确失败。ARM64 栈式 SEH、C++ EH、动态函数表、通用 RtlUnwind/NtContinue、跨加载器／VEH／VCH 回调边界展开仍不支持。

当记录包含 `EXCEPTION_NONCONTINUABLE` 而 x64 过滤器返回 `EXCEPTION_CONTINUE_EXECUTION` 时，系统使用新上下文派发 `STATUS_NONCONTINUABLE_EXCEPTION`（`0xc0000025`，标志 `0x81`，关联记录指针为空）。先重新运行 VEH，再从保留的逻辑栈重新搜索，在相同深度与执行预算内保留 finally 顺序和 EXE/DLL 栈帧身份。23 个原生场景包括 21 个成功执行和两个终止场景：即使恢复原始 `CONTEXT`，VEH/VCH 接受继续这个二次异常后，它仍未处理。模型将此结果报告为运行时失败。软件异常地址等于保存的 PC；内部派发器地址和寄存器布局由模型定义。 [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

`WindowsDynamicTests.cpp` 使用原始 x64/ARM64 DLL 和 EXE，对照独立原生 Windows 观测，覆盖引用计数、共享依赖、嵌套加载、附加失败清理、转发查询、进程退出、无入口 DLL 以及重载时重新初始化 TLS。额外回归拒绝被修改的加载器元数据和失效代码指针，保持累计准备额度，并确保被中断 API 的结果仍未完成。Windows CI 强制执行原生对照和 WHP 用例；交叉编译与 Unicorn ARM64 不代表原生 ARM64 已执行验证。

`GetProcAddress` 转发链任何位置缺失库均返回错误 127；显式 `LoadLibrary` 加载目录中缺失的模块返回 126。原生对照和各可用后端都断言全部 41 个已声明加载场景；在 Windows 上，每种 DLL 变体都会重复 16 次验证全部卸载后从入口返回。 `GetProcAddress` 转发目标的初始化失败也在清理后返回 127。进程分离回调保留退出调用方的栈内容。

`WindowsExportTests.cpp` 使用原始 x64/ARM64 DLL 和 EXE，验证转发的代码／数据／序号调用、别名、初始化期间查询、重定位、大小写敏感的缺失项、LastError、循环及非驻留目标、无效指针，以及成功查询后的元数据修改。同一 EXE 具有独立原生 Windows 对照；原生 CI 强制执行 WHP 用例。C ABI／CLI 测试对比完整报告。原生 ARM64 硬件证据仍待补齐。 有导出表和无导出表的 EXE 变体覆盖两种依赖图、PEB 链表顺序、退出通知顺序，以及名称／序号／空指针的错误码。

`WindowsLifetimeTests.cpp` 将冻结的通知序列与独立原生 Windows 进程及 KVM/WHP/Unicorn 执行对比，覆盖正常退出、入口返回、两个 DLL 初始化失败、四处提前退出及无入口 DLL。另行验证回调故障、共享预算、重定位 TLS 字段和 TLS 总容量。原生入口返回探针保留初始线程句柄，重复64 次核对线程退出码及精确线程／进程通知序列。观察完成后终止剩余子进程线程，不将其进程退出码当作入口返回值。

`NeverDUnpackTests`、`NeverDUnpackExecutionTests` 和 `NeverDUnpackPublicTests` 覆盖加壳镜像的恢复；参见[脱壳](unpack.md)。`UnpackGeneratedTests.cpp` 用测试自己加壳的程序，在 x86-64 和 ARM64 上检查入口规则。`X64ReturnPrefixTests.cpp` 在每种传输上检查双字节近返回，并确认其它带前缀的返回仍被拒绝。`WindowsDeferredTests.cpp` 检查不透明入口与已停止进程的观察；`ExecutionSessionTests.cpp` 检查执行监视。 `DirectX64Tests.cpp` 还验证直接执行的部分页监视、跨页取指、恢复后只执行一次、服务边界、非法指令和超时状态。

`LiveHeapReferencesCannotPublishAnOrdinaryRecoveredImage`、`ExplicitSnapshotsKeepExternalHeapDependenciesVisible` 和 `ReleasedHeapStateDoesNotBlockRecovery` 在可用的逐指令与直接执行后端上比较独立编译的启动及入口行为。`HeapReferencesInCapturedTLSCannotBeDiscarded` 覆盖仅存在于 TLS 中的依赖。公开接口测试要求拒绝时保留已有输出文件，并验证 C API 与 CLI 的显式快照一致。地址匹配是保守证据，不是原生运行证明。

`DirectServiceBindingsRequireAnExplicitSnapshot` 覆盖首次在捕获入口前后使用直接服务编号的情况；C API 和 CLI 也验证拒绝时保留已有输出。

`PrivateHeapDestructionReleasesOnlyOwnedBlocks` 验证移动后的分配所有权、跨堆拒绝、失效句柄及存活堆容量的复用。

`UnpackLibraryTests.cpp` 在测试内给独立 x64/ARM64 DLL 加壳，检查依赖顺序、普通及生成 TLS 回调、附加失败清理、输入/宿主身份、自身文件访问、导出名称/序号/数据/转发器及无自身导入。原生 Windows 通过独立 EXE 加载原始和重建 DLL，并调用声明的导出；受检与直接 WHP 用例均为必测。`CompletedGeneratedTLSCallsRequireTheAttachABI` 拒绝变更入口或参数；`GeneratedCallsNeedTheirReturnedStackAtTheContinuation` 拒绝错误返回栈。这些验证覆盖脱壳行为，不涉及去虚拟化。

`ExportObserver` 也观察驻留来宾依赖的可执行导出；建模提供者仍通过服务分派观察。输入镜像自身导出被排除。模块变化会刷新观察点，每次修复仍须由实时导出身份授权。发现记录不超过声明的导入上限。DLL 夹具同时要求修复系统 API 与来宾依赖的跳板，并通过原生加载验证不残留模拟地址。

`WrappedEntriesRequireExplicitTransferEvidence` 覆盖 DLL 包装器通过更深的栈调用恢复入口。默认结果仍为 `no_entry`；通过 `transfer` 选择该已观察调用后，可重建可加载 DLL。仅凭深层调用无法区分入口与初始化器。

`WindowsDeferred.EarlierTLSCallbackMayGenerateALaterCallback` 要求独立的 `.gentls` 段具有 `IMAGE_SCN_CNT_UNINITIALIZED_DATA` 标志，大小与声明的缓冲区范围完全一致，原始数据长度和指针均为零。`WindowsDeferredCases.def` 统一定义存储与汇编，普通 `.data` 保持独立。生成回调和生成入口两种情境均保留 x64/ARM64 上的严格拒绝及延迟执行检查。

`ExtendedRegistersLoadOrdinaryImportsAgain` 通过受检和直接 x64 执行验证紧凑及带填充的 R8-R15 导入加载。低寄存器用例覆盖前置 REX 形状字节和仅含 CALL 的地址辅助例程；带填充的调用辅助例程跳过 CALL 后的任意字节。`ImportCallHelpersCannotDiscardPersistentEffects` 要求辅助例程的持久副作用仍可观察。`PERebuildTests.cpp` 拒绝缺失起点、结果证据及重叠起点，并保持六至八字节调用窗口的精确 API 返回地址。

`OpaqueExportCallsAreRepairedBeforeTheExplicitStop` 恢复纯调用且不绕过未知 API。`ExportObservationIncludesTheOpaqueBoundary` 覆盖静态、动态和序号导出，保持执行及服务日志不变。`OpaqueExportObservationPreservesAnUnreadableReturn` 要求缺失的返回信息保持缺失。

`ExportIdentitySurvivesRebindingAndLateResolution` 改变不透明导出的绑定顺序，并在入口之后解析导出。checked/direct x64 用例要求正确的 API 身份，并保留明确的 unsupported-service 停止。

Windows 虚拟内存新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及当前进程的 `FlushInstructionCache`。OS 层管理预留区域，`AddressSpace` 统一管理已提交页面、权限和物理存储。测试覆盖动态代码改写、访问故障和内存额度回收。

`WriteProcessMemory` 对不超过 4 KiB 的当前进程写入，遵循 x64/ARM64 原生实测的已提交页面语义。它保留各区域的权限、已复制前缀、字节数和 LastError，包括 `ERROR_NOACCESS`、`ERROR_PARTIAL_COPY` 以及 RX 前缀写入后返回成功的情况。`WindowsMemoryWriteTests.cpp` 检查全部 25 种权限组合；`check_windows_memory_write.py` 在原生 Windows CI 上验证同一份原创可执行文件。未提交的目标区域仍明确不受支持。

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

外部写入分离回归覆盖精确及不足预算、部分字、大／小端、未触及内存的身份、完整仿射槽边界、晚到前驱覆盖和默认失效。公开 C API／CLI 检查 v6 布局兼容性和无效域。HighC、LLVMC 输出均在 O0／O2 下检查返回值、内存、栈及保留状态；这些测试不等于原生等价证书。

有限分派回归覆盖寄存器及帧内阶段、两种端序、确实可达的非法分支、后到前驱、耗尽的内层条件和相邻发现预算。目标次数测试覆盖算术关联、嵌套循环、解码模式、顺序落入计数、总工作上限及旧标志优先级。CLI 在 O0/O2 下执行两种 C 路线和源码 ABI；C/Python v8 测试检查布局、非法字段与未来尾部忽略规则。 回归还验证：大型无关有限选择器之后的原生条件仍有可用发现预算，且最后一次允许的细化优先用于已提出的生产者候选。

`NeverDLLVMCPhiTests` 在 O0/O2 下运行独立及交叉依赖的循环更新，覆盖零次循环、迭代边界和随机全位宽初值。可读性断言要求独立更新不产生快照局部变量，复合交换只保留必要快照。已有分支、switch、移动分支体及交换循环用例继续验证实际选中的边与同时赋值语义。

`LLVMCInternalExitRegions` 新增5个独立测试，覆盖两种退出极性、零次循环、退出边和回边的并行交换、循环头/内部退出的四种嵌套组合，以及循环头/循环体/回边块的观察顺序。共享目标出口和提前继续路径验证可执行回退；有效循环之后遇到不支持区域时，不得发布部分结构化结果。整模块与单函数输出须一致且不修改 LLVM。原始 LLVM 和生成 C 在 O0/O2 下与独立无符号 oracle 对照执行，共294,912次调用，C 启用未定义行为陷阱。

`NeverDLLVMCPhiTests` 还验证共同循环出口的不同后继 PHI 对、有序观察调用、输出内存及调用者 IR 不变。整模块与指定函数的 C 均在 O0/O2 下对照独立参考实现执行。不同退出判断及额外分支前驱覆盖保守处理。

`NeverDLLVMCPhiTests` 用独立 O0/O2 oracle 检查多回边循环表达式合并、观察调用顺序、修改内存的调用之前的值快照、窄位宽回绕和符号扩展。覆盖整模块与单函数输出且调用方 IR 不变，以及入边冲突、共享根、poison 标注、未定义操作数、变量移位、受约束 intrinsic、异常函数和预算不足时完整拒绝。只有所有入边运算一致，旋转调用才可合并。

`NeverDLLVMCPhiTests` 还在 O0/O2 下执行结构化标量区域：块顺序打乱的嵌套循环、菱形分支、零次迭代、窄整数回绕、循环头观测调用、PHI 交换、存活的外层变量、共享步进值和漏斗移位端点。测试检查源 IR 不变、合并后仅三个局部变量，以及多出口、不可约和超大图的可执行回退。这些用例独立合成；源代码输出本身不构成原生恢复证明。

`NeverDLLVMCValueTests` 在 O0/O2 下，将带类型的标量循环 C 与直接独立编译的 LLVM 对照执行，并为生成的 C 启用未定义行为陷阱。边界值和确定性全位宽输入覆盖窄整数乘法、移位前回绕、扩宽乘法与右移、宽整数截断到布尔值、有符号比较和扩展、优先级、条件表达式、布尔运算、不支持操作的回退及深表达式的物化。测试还检查调用方 IR 不变，以及冗余转换被移除。

`NeverDLLVMCPhiTests` 和 `NeverDLLVMCValueTests` 覆盖字节计数器自增减回绕、合并后的退出值、循环外的内联使用、存活外层变量和 PHI 快照。独立 O0/O2 检查开启未定义行为陷阱，验证复合加法、逆序减法拒绝、窄乘法及布尔掩码。嵌套区域测试要求计数器在循环内声明、结果变量保持独立，且不修改源 LLVM。 可执行命名回归使外部函数与初次生成的结果变量及计数器同名，验证调用和观测副作用均被保留。

`NeverDLLVMCValueTests` 检查加减和位运算中两种单位元分支位置、未变的基础值、内联旧值依赖、已物化的条件快照、窄整数真假判断、非单位元分支及共享选择值。生成的 C 在 O0/O2 下与独立编译的 LLVM 对照执行，并启用未定义行为陷阱。`NeverDLLVMCPhiTests` 还检查并行旧值快照，以及必须留在共享作用域的分支初值。调用方 IR 保持不变。

`NeverDUnicornDecodeTests` 检查 AVX-512/APX CPU 模型的 EVEX 寄存器保留位，以及 ROUND 访存异常优先级、状态保留和恢复。独立 Linux x64 主机程序确认了传统编码的对齐异常和标量/VEX 编码的缺页异常。这些引擎测试不扩展 checked 指令准入，也不代表 APX 原生执行证据。

## ARM64 CPU 性能测量

使用 Release CPU 构建；显式 HVF 要求原生 ARM64 macOS，软件对比需启用 Unicorn。工具验证每个结果，启动单独计时；双 CPU 切换包含中间 API 调用与检查，其余执行负载不含设置和验证。

```bash
cmake --build build-cpu --target neverd-cpu-bench --parallel 4
build-cpu/bin/neverd-cpu-bench --backend hvf --samples 7 --warmup 1
build-cpu/bin/neverd-cpu-bench --backend unicorn --samples 7 --warmup 1
```

重建前保存基线可执行文件，使用 Python 3.11+ 交替测量。保留编译配置、源码标签、二进制摘要及全部样本，测量期间不要并行编译或测试。

```bash
python3 scripts/benchmark_cpu.py \
  --baseline /path/to/before --baseline-label BEFORE_COMMIT \
  --candidate /path/to/after --candidate-label AFTER_COMMIT \
  --pairs 15 --output /path/to/new-comparison.json
```

入口计数为单独诊断，会增加开销，包含启动探针；计数运行耗时不得混入性能结果。局部负载不代表完整 OS 或跨架构性能。

[复现方法](../testing.md#reproduce-checked-arm64-cpu-measurements) · [HVF](macos-hvf.md)

`NeverDLLVMPrivateFrameTests` 覆盖重叠写入、全部入口路径及循环回边、保留的数值地址、外部输出别名、未初始化／有序／未知内存、元数据，以及精确／少一单位预算下的原子拒绝。独立 O0/O2 oracle 启用未定义行为陷阱，比较完整返回值、外部对象及帧字节。编译覆盖 x86-64、AArch64、大端 AArch64 和 ARM32，不代表已支持这些架构的原生恢复。修改共享地址／副作用工具时同时重跑 `NeverDByteMemoryForwardingTests`。

```sh
cmake --build build-release --target NeverDLLVMPrivateFrameTests NeverDByteMemoryForwardingTests --parallel 4
build-release/bin/NeverDLLVMPrivateFrameTests
build-release/bin/NeverDByteMemoryForwardingTests
```

`NeverDByteCellScalarizationTests` 覆盖重叠字、两条入口路径和两条回边、宽及非二次幂位宽访问、两种字节序、存储值选择和 poison 义务保持、部分 poison 覆盖、完整使用图拒绝，以及跨对象的精确/不足预算。常规 Thin/Deep 管线必须消除残留数组。独立 O0/O2 oracle 对 8,192 个输入的三个版本比较全部 24 个输出字节、外围哨兵和返回值，共 49,152 次调用，启用未定义行为陷阱。x86-64、AArch64、大端 AArch64 和 ARM32 编译检查与原生执行覆盖分别记录。修改共享内存契约时，应连同字节转发和私有帧测试一起运行此目标。

```sh
cmake --build build-release --target NeverDByteCellScalarizationTests --parallel 4
build-release/bin/NeverDByteCellScalarizationTests
```

`AndroidMutexTests.cpp` 用独立 O0/O2、普通/APS2/RELR 夹具覆盖三类 mutex、多等待者、唤醒后再次竞争、递归最终释放、errno、原始事件、失效内存、死锁和累计指令限额。Unicorn 及可用 KVM/WHP/HVF 执行相同用例；不可用后端明确跳过。这些证据不表示 Android 真机或并行 SMP 等价。

`HighControlFlowSemantics.DeepStableContainersPreserveEveryReturnPath` 使用独立解释器检查 48 层块、循环、switch 和异常体的进入及跳过路径，并以宽松的运行时间上限捕获重复递归遍历。这是结构化 HighIR 覆盖；全镜像方法恢复仍需独立完成清单与依赖检查。

`SwiftOnceSources.EarlyReturnsKeepExactObjCOnceThunkProofs` 覆盖 ARM64/x64 中合并或分离 retain 调用的 thunk。`EarlyOnceCopyReturnsRequireTheSameCompleteTail` 拒绝改变存储、缺少或重排 retain、有序加载、改变返回值及外部入口。`IgnoredNestedReturnCopiesDoNotObserveOnceContext` 检查 void 回调的两条可行退出路径，并在投影后重新验证源码控制流。

`HighControlFlowSemantics.ReturnTailCopyKeepsTheOuterLabelOwner` 使用独立解释器，对比同一地址在嵌套块中再次出现时进入与绕过该块的路径。`ReturnTailCopyIncludesTheFirstChildOfItsLabel` 保留合法的父语句与第一个子语句共享地址的情况及其赋值。这些定向检查不能替代完整方法与原生依赖比较。

`JumpTailCopyKeepsTheOuterLabelOwner` 检查跳转尾部的同一归属规则。

`EarlyStringGetterReturnsKeepOnlyInertOnceAnchors` 检查提前返回与 once 调用之间的空指令锚点，并拒绝其间的调用或存储。

`SwiftOnceSources.ObjCThunkRootsShareTheNestedCallbackProof` 检查共享根节点证明，并在叶节点开始观察上下文后拒绝旧计划。

`SourceABI.SwiftPointActionRequiresTwoDoublesAndContext` / `ObjCCallHints.CoreGraphicsPointActionsKeepSwiftFloatingCarriers` 覆盖两种架构，并拒绝变更提供方、弱导入、冲突存储及陈旧 ABI 载体。`HighCSourceCalls.SwiftCoreGraphicsPointActionsKeepCoordinatesContextAndOrder` 在 O0/O2 下执行生成的 C，使用独立 Swift 载体验证函数检查坐标位模式（包括有符号零、次正规数和 NaN）、接收者身份、调用顺序和保护值。这些检查证明调用 ABI，不代表上层方法已完整恢复。

`NativeSourceHints.CGContextCGRectMethodKeepsOrdinaryAndSwiftContextInputs` 校验完整方法树和精确载体，包括私有成员及被拒绝的签名。`SwiftFieldReceiver.CGRectMethodSelfKeepsItsLogicalParameterIdentity` 与 `CGRectMethodRejectsChangedEntryAndReceiverParameter` 覆盖流水线和发布重放，拒绝 self 索引、入口或参数类型的变化。编译器记录涵盖四种 macOS/Mac Catalyst 目标；此入口声明仍仅支持 arm64。 `HighCSourceCalls.SwiftCGRectMethodKeepsContextReceiverAndAllCoordinateBits` 在 O0/O2 下将生成 C 与独立的 Swift 标量载体参考实现对照，验证四个坐标的全部位模式、不同的 context/self 指针、单次调用和存储保护。

`NeverDLowInstructionBoundaryTests` 可独立运行 LowIR 指令来源测试，无需构建聚合提升测试的全部夹具。`BackwardSharedReturnEpilogueKeepsReturnAndCallerFrame` 验证对齐 ADD 和后索引 LDP 栈释放，包括由调用方恢复链接寄存器的情形；原始 RET X30 与共享入口仍独立保留。`BackwardSharedReturnEpilogueRejectsChangedReturnAndOwnership` 拒绝其他返回寄存器、BR X30、缺失或未对齐的释放、窄恢复、内部入口、修正、可写或歧义映射、可重定位输入及其他格式。解码共享尾部不能证明原生 ABI：缺失调用方保存或分配仍会使现有帧证明失败。

`NeverDOwnInteriorCallTests` 覆盖 x86 和 x86-64 函数直接 call 自身 unwind 范围内标签的情况，分别在 Microsoft x64 `.pdata` 条目、System V x86-64 DWARF FDE 和 i386 DWARF FDE 下测试。只为压入返回地址而做的 call 被提升为压栈加跳转，x86-64 直线和循环两种情形生成的 C 在 `-O0` 和 `-O2` 下以 AddressSanitizer 和未定义行为陷阱运行。返回时恰好弹出该 call 自身返回地址的目标仍是普通调用；切换栈之后的返回或低于入口栈指针的返回会被拒绝。从 i386 注册链恢复的范围不能界定函数体，因此其中的 call 仍是 call。

`NeverDSysVCallContractTests` 以 QtXml 中 `QDomNode::save` 和 `QDomNode::isDocument` 的形态检查 x86-64 System V 调用约定。摘要显示会读取某个参数寄存器的直接被调用者会收到调用者的值，包括原样传递的传入 `this`；虚调用会取得支配块载入 `RDI` 的对象；在某条路径上不写 `RAX` 就返回、在其他路径上只传递被调用者结果的方法为 void；在比较链之前写入 `AL` 的字节在每条路径上都是返回值。生成的程序在 `-O0` 和 `-O2` 下带 AddressSanitizer 与未定义行为陷阱运行。

`ObjCCallHints.CIImageAffineValueKeepsProviderAndPhysicalCopyCarrier` 检查 CoreImage 提供方、CIImage 工厂、完整 48 字节逻辑记录及 x2 指针，拒绝缺失或错误的提供方、x86_64 和冲突声明。`ObjCImageValueCopy.OriginalFrameAndCompleteBodyAuthorizePublication` 区分原始调用与沿用同一机器地址的结果赋值。`RejectsChangedCopyCallBodyAndCurrentImage` 拒绝 24 类凭据、参数、存储、帧、元数据、导入、重复调用及保存 IR 的修改，包括同时一致地修改 MedIR 和 HighIR。`GeneratedCExecutesAgainstIndependentPhysicalCopyABI` 在 ARM64 上以 O0/O2 执行未经修改的生成 C，对照独立从编译器观察到的 x2 指针接收函数，检查六个浮点位模式、选择器与接收者身份、单次求值、返回对象、合法副本写入、输入不变性和边界保护。其他主机跳过此物理 ABI 执行测试。

`ObjCCallHints.CurrentMethodEncodingMustAgreeWithCachedDeclaration` 拒绝与当前非空方法编码或选择器不一致的缓存 ABI；仅提供显式类型声明的客户端保留原有约定。

`DarwinIndirectRecordCalls.MatrixFrameEffectsRequireExactCurrentContract` 覆盖当前矩阵/仿射契约及 22 种必须拒绝的契约篡改。`ObjCAffineImageValueCopy.CurrentProducerInitializesThePublishedCopy` 证明 SDK 结果进入 CoreImage 副本并通过独立发布重建；`RejectsWrongProducerFrameAndSavedIR` 对每个生产函数检查 12 种篡改，包括输入写入缺失、结果越界、错误提供者/ABI 载体和复用已消耗的 Concat 输入。`GeneratedCMatchesOriginalMachineAndSDKResults` 在 Apple ARM64 上以 O0/O2 执行未修改的生成 C 与原始 ARM64 机器字，并调用原生 CoreGraphics：每个函数 1000 组数据对比全部 48 个结果字节、两个输入记录、选择子/接收者身份、单次调用、返回对象、私有副本改写及边界保护值。其他主机跳过此原生 SDK 执行测试。

`MatrixFrameEffectsRequireExactCurrentContract` 还覆盖 CGRect 消费者及 22 种必须拒绝的篡改。`ObjCAffineImageValueCopy.CGRectInputUsesTheSameCurrentFrameOwner` 验证“旋转 → CGRect 借用 → 旋转重新初始化 → CoreImage 发布”；`CGRectBorrowRejectsExpiredInputsAndChangedABI` 拒绝初始化、输入范围、导入和载体的八种修改。`GeneratedCMatchesOriginalMachineAndSDKResults` 还以 O0/O2、1000 组数据，将该完整顺序的生成 C 与原始机器字、原生 SDK 对照，检查保存的角度、全部 48 个最终字节、对象及保护值。

`FrameMetadataAccessorUsesCurrentCatalogAndABI` 检查共享元数据声明、两个响应载体和当前帧见证的发布。`FrameMetadataAccessorRejectsChangedImportAndBytes` 拒绝弱导入、provider/名称/addend 变更、私有地址请求、部分 spill、错误重载及原始调用变更。原始 ARM64 与生成 C 的见证 oracle 还会实际调用 Foundation URL 元数据访问器：两个分支均在 O0/O2 下运行 2048 组，检查动态见证选择、完整输出字节、输入保持、调用次数和保护字。这些检查不证明动态栈分配或见证内存效应。

`AArch64ExclusiveTests.cpp` 覆盖标量及成对宽度、acquire/release 形式、寄存器重叠、别名、对齐与权限故障、快照、观察回调取消或失败以及双 CPU 竞争。`RAMReservationTests.cpp` 覆盖相同值写入、ABA、分配复用、回滚，以及 KVM/Unicorn 字符串和 `ENTER` 写入的干扰。原始 ARM64 Windows 进程样例执行独占循环；`scripts/check_aarch64_exclusives.py` 在 Windows ARM64 CI 上执行原始指令并保存对齐异常记录。原生指令证据不代表已验证 ARM64 KVM/WHP 后端执行；不可用配置仍明确记为跳过。 相同的独占指令用例还覆盖软件 Unicorn、跨契约干扰、同值及 ABA 写入、同次执行中的可执行别名，以及 `DC ZVA` 写入和观察回调取消。
 `windows-alignment-oracle.yml` 也运行 ARM64 探针：1,320 条观察记录覆盖所有未对齐偏移、四种读写序列，以及可写、只读、不可访问和跨页内存。探针保留完整位宽的寄存器测试值，并记录故障前已提交的部分写入。 `WindowsExclusiveProcessTests.cpp` 将 1,320 条原始 Windows ARM64 观察结果与 `WindowsExclusiveNative.def` 中的原生摘要逐项核对，保留寄存器值、异常元数据和 RAM 效果，仅归一化代码与数据的放置地址。

`AArch64AtomicTests.cpp` 覆盖 168 种独立汇编的 LSE 编码、寄存器别名、有符号比较、权限、取消、物理保留状态及 NZCV 传输。`scripts/check_aarch64_atomics.py` 收集 1,100 条原始 Windows ARM64 记录，包含完整操作数结果、异常上下文和 RAM 写入范围；解析测试拒绝缺失或不一致的证据。原生 KVM/WHP 执行仍需单独验证。 checked 进程回归由 `WindowsAtomicProcessTests.cpp` 执行，完整记录摘要保存在 `WindowsAtomicResults.def`。

结构上已为常量的原生目标直接使用现有的可达性检查调度。符号单目标只有在穷尽枚举后才复用传入谓词。回归在直线执行的查询预算内验证 128 次常量跳转，并按每次转移两次枚举查询的预算验证 32 次计算目标跳转，保留未约束的地址高位和分支域。完整状态结果被修改、缺少对齐约束、目标数量上限为零或查询与指令预算不足时必须拒绝。多目标和未完成枚举的现有拒绝检查仍然必需。

只有完成 UNSAT 证明、排除另一条边后，原生分支才在当前边保留传入域。测试在 512 个求解门内验证两个方向各 32 次带条件跳转，并检查精确与少一次的查询预算及求解门耗尽。修改或删除对齐条件、反转比较以及修改终态都必须拒绝；任意未定义控制和两条边均可达的现有测试仍然必需。

位展开缓存与遍历存储仅记录实际到达的表达式节点和变量。`NeverDSolverTests` 检查稀疏的高位编号、增量断言之间的上下文增长、缓存位复用、模型提取和假设切换。无关的宽表达式不会被编码；实际到达的宽度违规、畸形根节点和求解门预算耗尽仍必须拒绝。

`SourceFrameAnalysis.CallStorage*` 覆盖精确调用、到达定义、初始化、填充、逃逸、边界和循环，不授予来源发布权限。`ObjCFrameBlockBorrows.*` 覆盖描述符限定的同步借用，以及 19 种导入、头部、ABI 和机器指令修改。测试保留未证明的填充字节并拒绝未初始化的所有权字段；block 构造、捕获读取及回调依赖闭包仍各自接受发布验证。

block/副本发布测试还覆盖两个独立的 48 字节范围、描述符重叠、回调本体变化、过期机器指令与 IR、脱离本体的调用位置，以及精确投影顺序。`MixedWidthFrameCopiesMeetEveryInitializedByte` 与 `FrameCoverageCannotHideMissingBytesOrPointerJoins` 检查两种合并顺序下的 8/16 字节写入、缺失字节、可写借用失效和部分覆盖后仍保留的指针身份。

循环关系测试覆盖任意迭代次数下固定及变化的函数临时值、两侧独立偏移、配对方案、部分和非对齐范围、两种字节序及基于临时值的排名。原生组合只在候选侧保留新增存储。缺少前缀、未声明或未定义字节、错误投影、遗漏赋值、程序行为变化及定义集合冲突均须拒绝认证。执行、查询及观察项预算在精确上限通过，少一单位失败；推断不能为后续证明补充预算。生存期仍绑定摘要。这些检查不建立普通原生 ABI。

原生循环关系测试在任意迭代次数下检查延迟条件分支收集和保留未审核拒绝边界。手工及推断方案必须重新检查完整入口域和归纳域；可达的坏分支、被修改的原生更新及查询或指令预算耗尽都拒绝证书。测试绑定被修改的不可达边界字节，保留严格默认值和无效方案拒绝，检查两种见证及组合收集选项，并继续拒绝静态 API 和重叠指令。证书语义模式 17 绑定此准入；普通原生 ABI 和源码组合仍是独立义务。

`ObjCSuperGetterSources` 覆盖四载体 CGRect getter、十项发布变更拒绝用例，以及布尔/CGRect 调用者共用机器代码的情况。O0、O2 执行验证检查精确返回位（含负零、无穷及 NaN 载荷）、接收者/类身份和元数据调用之后的选择子加载。Apple ARM64 同时执行原始编译器 thunk 和生成的 C；其他平台使用本机记录 ABI 执行生成的 C。

`LowIRLoopInference` 覆盖带任意初始高位的 8、24、32 位计数器投影、递增及递减、寄存器、栈帧、函数临时值和两种字节序。完整自证明通过，结果变化、停滞、窄位宽回绕及跳过相等退出条件均被拒绝。操作、查询、路径、排名候选及拓宽预算在精确上限通过，少一单位失败；最终证明的操作、查询及观察预算单独检查。

`ObjCCallHints.SDKRecordData*` 检查两种外部记录、每个 double 的偏移、两种 Darwin 架构及提供者别名，并覆盖导入变化、弱链接、缺少库、修复冲突、可写存储及不完整范围。`python3 -m unittest scripts.tests.test_generate_darwin_record_data_declarations scripts.tests.test_generate_darwin_data_declarations` 检查配置冲突、替代布局、无效大小/对齐、TLS 和各架构导出。用固定 SDK、libclang、输出路径及 `--check` 运行 `generate_darwin_record_data_declarations.py` 可复现目录；声明检查不证明原生间接结果已初始化，也不建立方法恢复。

`LowIRLoopInference.ProjectedBounds*` 覆盖计数器与边界高位任意的窄位域等值退出，包含 8、24、32 位、三种存储位置和两种字节序。完整证明拒绝边界变化、停滞、跳过退出、回绕及被观察的输入高位字节变化。推断与证明预算保持独立，并检查精确上限和少一单位的情形。

`LowIRLoopInference.LateCounter*` 覆盖常量初始化后仅在泛化时才显现的 8、24、32 位计数器，包含寄存器、帧、函数临时量、两种字节序及任意边界高位。等值退出通过完整证明；停滞、跳过退出、变化的边界及可观察高位字节被改动均被拒绝。推断与最终证明分别检查精确预算和少一单位预算。

`LowIRLoopInference.ProjectedComparisonBits*` 覆盖循环头处缓存的窄位域等值条件，边界高位可任意，包含 8、24、32 位计数器、三种存储位置、两种字节序和常量或高位填充初始化。完整证明拒绝缓存值、可观察高位字节、边界被改动，以及停滞或跳过退出。推断与最终证明分别检查精确预算和少一单位预算。

`LowIRLoopInference.OrderedComparisonBits*` 覆盖两种布尔编码下缓存的无符号大小比较退出，包含 8、24、32 位计数器、三种存储位置和两种字节序。完整证明拒绝不终止的更新、比较条件变化、边界变化和可观察高位字节被改动。推断与证明分别检查精确及少一单位预算。改变布尔编码的测试夹具在证明前重新绑定原始操作摘要。

`LowIRLoopInference.MutablePrefixBounds*` 覆盖高位持续变化的 8、24、32 位派生边界、三种计数器存储、两种字节序和直接或缓存退出条件，检查不终止、移动的等值边界、可观察高位及缓存改动，以及精确和少一单位预算。移动的无符号大小比较边界可能在回绕时终止，另有完整证明回归。

`LowIRLoopInference.SharedTemplatesFitIndependentOperationBudgets` 在 1,024 次推断操作和 640 次独立证明操作内证明嵌套的部分计数器。完全相同的纯表达式和不可变前缀读取仅在同一次切点重建内共享，保留操作数身份、输出宽度及位置空间。测试观测完整的计数器和边界字，拒绝被修改的前缀计算及少一次操作的预算。现有寄存器、帧、函数临时量、字节序及不终止案例仍须运行。

`LowIRLoopInference.CompletedEntailments*` 在两种字节序的终止及不终止帧循环之间检查会话隔离、求解器和节点耗尽，以及独立证明预算。可变边界回归在缓存命中时验证精确与少一单位的逻辑查询预算；原生重复上下文证明检查带域的复用。

`LowIRLoopInference.IncrementalEntailmentsKeepRollingBudgets` 检查同一约束域内的编码器复用。切换域会丢弃编码器；累计逻辑门容量耗尽时，仅用新编码器重试一次，并额外计入一次查询。两种字节序都保留对完整计数器和边界字的观测，并检查精确及少一次的查询预算、逻辑门／宽度／搜索超限拒绝，以及独立的最终证明预算。

`LowIRLoopInference.RebuiltCounterLanes*` 覆盖计数器其他位从前缀值重建或独立变化时，精确匹配的投影更新。寄存器、帧和函数临时量案例包含两种字节序、1／3／4 字节计数器及直接／缓存退出条件，并观测完整计数器、边界和标签。删除可观测的高位标签、不终止更新、缺少退出守卫及少一单位的推断／证明预算仍须拒绝。结构递推匹配仅提出泛化和秩候选，仍必须通过完整转换证明和最终证明。 固定的操作数和查询数预算还覆盖符号投影计数器，避免将常量前缀中的偶然匹配扩展为额外关系。

`LowIRLoopInference.ProjectedCounterCopies*` 覆盖在递增前经由带独立标签的字复制计数器位段的嵌套循环。144 组完整状态用例涵盖寄存器、帧、函数临时量、两种字节序、1/3/4 字节位段、直接或缓存退出条件，以及整字复制对照。投影相等关系必须在保存的到达状态和每条传入转移上成立；高位保持独立。转移域蕴含证明可识别由不同参数表示的加法递推。丢弃高位标签、无效更新、缺失保护，以及推断或最终证明预算少一单位时仍会拒绝。

`LowIRLoopInference.TransferredCounters*` 覆盖计数器在不同切点间搬移存储位置、且其他循环可绕过各切点的情况。精确的符号单位步长转移仅用于提出源计数器保护条件，以及允许一个切点使用不同位置的备用排名；所有条件和排名仍须通过完整转移证明。192 组完整状态用例涵盖寄存器、帧、函数临时量、两种字节序、1/3/4/8 字节计数器、独立标签及位置不变的对照。结果或标签改变、错误排名映射、不终止更新、缺失保护及推断或最终证明预算不足均会被拒绝。计数器遍历使用现有符号节点限额，可选图选择器的工作预算可保持为零。 用例同时覆盖递减到零和递增到输入上界。晚发现计数器时，待所有切点模板重建后再剪除界限和位段保护条件；其他宽化及独立证明照常执行。

`LowIRLoopRefinement.GuardedCuts*` 和 `BinaryLowIRLoopRefinement.GuardedCuts*` 覆盖同址切点、寄存器、帧及原生系统标志、两种字节序、未匹配的有限及循环路径、重叠和错配拒绝、前缀泛化、未定义值见证、错误元数据、摘要及共享预算。独立原生测试证明两个 R10 上下文共用循环地址，并确认未审计边界检查先于选择条件。普通 ABI 认证仍是独立工作。

`BinaryLowIRLoopInference.NativeSelectors*` 覆盖两个寄存器上下文、仅靠帧区分的上下文、三域合取、无法区分的模板、来源及原生循环体变异，以及推断和证明各自的精确与少一预算。循环次数任意，不引入入口常量。

`NativeSelectorsGeneralize*` 验证交替寄存器／帧阶段的自动恢复与完整原生证明，包括高位仍符号化的字节掩码。`NativeSelectorState*` 检查错误排名／本体、掩码外位损坏、畸形赋值及工作量／元数据的精确和少一预算。显式有效计划还把正式构造器与变换前后的完整原生检查组合，覆盖重叠和不相交切点混合、原始侧前缀来源保留、回退扫描计费及临时偏移溢出拒绝。这些显式计划检查与自动推断覆盖分别记录。

`DarwinIndirectRecordCalls` 检查当前 MakeScale 契约及 22 项导入/ABI 变更拒绝案例，再通过共享按值副本证明消费完整的 48 字节私有结果。未对齐、偏移、重叠或越出栈帧的结果范围均被拒绝。即使保留完整返回 ABI，移除确定写入效果也会被拒绝。

`SourceFrameAnalysis.IncomingResultAddressNeedsCompleteEntryIdentity` 拒绝十种入口、载体或写入修改以及缺失的入口 ABI。`NativeSourceHints.IndirectResultTailCallRetainsExplicitOutputAddress` 重新提升直接尾调用，检查显式输出参数、六次写入和发布门槛。

`NativeSourceHints.FourDoubleCallerDemandNeedsEveryUnchangedCarrier` 检查四个低位通道、独立的高位写入及九种声明或控制修改。`FourDoubleReturnRequiresEveryComputedLowLane` 拒绝十二种结果不完整或契约失效的情况。`DarwinNativeRecordReturns.FourComputedDoublesExecuteAtO0AndO2` 在每个优化级别运行 2048 组输入，与独立算术判据比较全部 32 字节结果。

`DarwinIndirectRecordCalls.AffineInvertSnapshotsItsCompleteAliasedInput` 在 O0、O2 各运行 2560 个同址、重叠或分离的输入输出布局，检查输入位模式、单次调用、全部 48 字节结果及完整带守卫存储。它验证物理复制与快照，不是原始机器码或原生 SDK 执行。当前矩阵/仿射契约测试对每个契约保留 22 种拒绝修改。

`DarwinIndirectRecordCalls.AffineTranslatePreservesScalarBitsAndSnapshotsAliasedInput` 在 O0、O2 各运行 2560 组输入，检查两个标量的位模式、六个输入字段、单次调用、全部输出字节，以及同址、重叠和分离布局的带守卫存储。四种标量 ABI 修改和共享的 22 种导入/ABI 修改均被拒绝。位操作替身验证物理参数和输入快照，不是平移数学判据或原始机器码执行。

`NativeFloatingReturnProof.HFAResultFieldsNeedTheExactCompleteDefinedCall` 覆盖九个字段选择及十七种调用、载体、宽度、偏移或 SSA 修改拒绝案例。`HFAFieldExtractionNeedsADominatingCall` 拒绝来自兄弟路径的生产者。`NativeSourceHints.HFAFieldTypeRequiresCurrentCallAndFrameProofForPublication` 提升五条指令的 ARM64 调用者，按推断的标量结果重新提升并检查源码发布门槛；错误提供者或缺少 LR/SP 恢复均被拒绝。这验证源码类型与投影行为，不是原始机器码正文执行。

CPU0 显式抢占、虚拟时钟语义及当前边界见[驱动调度](driver-scheduling.md)。

`SwiftOnceSources.FoldedObjCGetterTailsExecuteOnceAndRetainsAtO0AndO2` 折叠实际 ARM64/x64 返回尾部，覆盖合并或分离的 retain 调用、独立或内联谓词，再以运行时替身在 O0/O2 执行输出 C。检查结果位、一次初始化、调用顺序及缓存值变化。`FoldedObjCGetterTailsRevalidateCurrentStorageAndCalls` 在保留旧计划时仍拒绝宽度、顺序、内建操作、存储、结果、调用及当前导入修改。这是受控源码与运行时替身检查，不是原始 WMF 机器码或原生 Swift 运行时执行。

`SourceFrameAnalysis.CompleteOutput*` 覆盖完整和较短前缀、各返回路径合并、SDK 尾调用写入、缺写字节、指针逃逸及载体限制。调用方用例拒绝缺失或过短证明、错位、越过栈帧边界、覆盖保存寄存器、别名、后续写入失效和存活不透明值重叠。`NativeSourceHints.CompleteNativeOutput*` 重放汇编生成的 ARM64 生产方和消费方，要求完整 SDK 输入范围，并拒绝过期代码、控制流、ABI、审计、提供方和调用实例证据。这些检查验证字节初始化和源码门禁，不执行原始机器函数，也不证明原生逻辑返回值。

`MedCallingConvValueFlow.FPInputsFollowOnlyAuthenticatedCallPrefixes` / `NativeSourceHints.PreservedFPPrefixesRetainAllEntryInputsAfterSDKCalls`: 保留前缀回归检查三个有效读取和十九个被拒绝的载体、前缀、调用归属及未消费值用例。汇编生成的 ARM64 调用方在 SDK 调用后保留全部四个入口 double 低位通道；重新提升检查完整参数 ABI 和源码门禁。未修改的已获准发布 WMF `0x36350` 方法还在 O0、O2 各通过 2048 例，与独立原生 CoreGraphics 翻转、平移、归一化、连接及应用表达式比较全部 32 字节结果，并检查接收者、选择器和输入保护区，覆盖零、负数、无穷和 NaN 尺寸。未执行原始 WMF 机器函数。

`SourceABI.SwiftEntryCapturesAndReturnsTheErrorRegisterOnEveryPath` 检查 ARM64/x64 入口捕获、两条返回路径以及缺失或变更传输证明的拒绝。`NativeSwiftCallsKeepBothResultsAndPostCallErrorBranches` 保留调用结果、更新后的错误寄存器及后续条件判断，并覆盖私有名称冲突和两种输出顺序。生成的 C 在本机 Darwin 目标上以 O0/O2 执行，与独立成功/失败判据比较。`SwiftFunctionSymbols.RegularExpressionInitializerRetainsContextAndError` 拒绝别名、完整符号变化、非代码入口和不支持的镜像格式。这些检查不执行原始 WMF 构造函数，也不证明上层方法已经完全恢复。

`NativeSourceHints.SwiftErrorDeclaration*` 在用编译器声明细化观测得到的标量 ABI 后，重新提升实际组装的 ARM64/x64 入口。当前审计缺失或不完整时拒绝替换；选项、MedIR 或 HighIR 中的显式源码契约仍保留优先权。入口测试还会在转成 HighIR 前拒绝缺失的 MedIR 错误输出标记或错误的操作数宽度。

即使所有路径都覆写错误寄存器，入口捕获仍作为活跃值保留，包括 once 绑定后的最终源码清理。`NativeSourceHints.SwiftErrorCallsRequireCurrentDirectNativeProjection` 拒绝缺失、空、被修改或未证明的被调用函数，以及目标变化、间接调用、不完整结果、缺失操作数和不兼容效果。


`SwiftFunctionSymbols.RepeatedDeclarationsKeepEveryRecordField` 检查完全相同的重复记录，并拒绝名称、大小、边界来源或名称来源变化。`NativeSourceHints.SwiftErrorCallResults*` 覆盖 ARM64/x64 调用者的自动推断，拒绝缺失的当前被调用函数、被修改的机器操作、过期审计、不完整 ABI，以及缺失、变窄或来自其他值的结果提取。入口源码执行还覆盖相冲突的可选调试声明，确保绑定的调用约定及错误、上下文角色保持有效。

Swift witness 生成器在 ARM64/x86-64 macOS 和 Mac Catalyst 上同时验证 `CurrentValueSubject: Publisher` 与 `Range<Bound: Comparable>: RangeExpression`。`scripts.tests.test_generate_swift_witness_contracts` 拒绝泛型输入、元数据响应类型或成员、原型、导出提供方和完整数据流的变更。`ObjCSourceBindings.SwiftWitnessUndefRequiresExactDescriptorContract` 与 `SwiftWitnessUndefRejectsUnprovedInputAndABI` 在两种架构上验证两个描述符，每个描述符与架构包含 33 种 runtime/导入身份、弱或冲突存储、ABI 和副作用变更。这些目录不授予帧布局或借用契约。

`scripts.tests.test_generate_swift_data_declarations` 检查完整 `String.Index` 描述符查询，拒绝符号指针单元、配方字节或长度、元数据与缓存数据流、runtime ABI 的变更以及重复或缺失定义。`ObjCSourceBindings.SwiftRangeIndexDescriptorKeepsItsCompleteRecipe` 在两种架构上检查位于偏移 3 的非首位描述符；`SwiftRangeIndexDescriptorRejectsStaleIdentityAndRecipe` 拒绝每种架构的 20 种变更，并复核既有地址提示与 helper 输出。

`ObjCSourceBindings.PrivateFramePointerTailRequiresExactStoreOnEveryPath` 覆盖 PHI 合并后的对象局部变量循环，以及同一循环中可达的私有栈帧值。对象循环保留精确指针溢写证明；携带栈帧的循环、部分覆盖和未知栈帧泄露会拒绝证明。

`ObjCCallHints.FoundationGenericNSRangeKeepsSixPointersAndTwoWords` 验证两种架构、导出提供方及全部参数和结果载体，并拒绝弱导入、附加偏移、外来提供方、过期符号和虚构的借用效果。

`scripts.tests.test_generate_swift_witness_contracts` 的 String 读取器检查完整缓存与查询流程，拒绝 28 项存储、ABI、流程突变和七种歧义声明，并限制输入预算。现有见证绑定测试覆盖四个描述符与两种架构，每个组合包含 33 项突变。描述符身份不提供栈帧布局或借用权限。

`PreparedFiniteKeys.*` 检查上下文销毁和重命名、投影顺序与上限、移动后显式失效、格式错误及不完整结果、空域和非唯一域，以及精确容量边界。已有缓存和栈偏移回归也覆盖预备键路径。

`SourceABI.SwiftPointTransformKeepsTwoFloatingInputsAndResults` 在两种架构上拒绝载体、布局、上下文角色和间接结果变化。`SourceABI.SwiftPointForwardingPreservesBothIEEECarriers` 在 -O0/-O2 下编译运行转发源码，验证两个字段的正负零、次正规数、无穷和 NaN 载荷。声明测试接受编译器及具名参数形式，拒绝签名变化和歧义图像身份。

MainActor 测试数据检查完整的固定元数据与静态表流程，拒绝存储、ABI、元数据提取、表身份的变化、额外副作用、缺失或重复声明以及输入预算耗尽。两个目录均要求每种目标提供全部三个配对 SDK 导出。见证绑定测试在 arm64/x64 上覆盖全部四种描述符，每种描述符和架构检查 33 项输入、导入及 ABI 变异；宿主运行时检查以五种实例化参数位模式核对公开表。

`BitVectorEncodingClone.WatchMigrationAndGrowthOutliveTheSource` 在复制并销毁源后扩展文字表及共享 watch 列表，独立修改同源副本，在一次 watch 访问处中断传播，并依据原始子句和独立布尔关系检查恢复后的完整模型。

`ObjCCallHints.SwiftPublishedAccessorsKeepOpaqueValueAndAllKeyPaths` 在 ARM64/x86-64 和两种规范 Combine 提供方上检查两个访问器，每个组合拒绝九项 ABI 变异和八项导入身份变异。独立 SDK 验证在 ARM64 主机上以 O0/O2 执行两种源码架构配置生成的 C，在 128 次访问器调用中比较全部 24 个载荷字节、输入输出保护区和两个对象身份。八种交叉编译配置覆盖两种架构的 macOS 与 Mac Catalyst。运行时检查保留写入器消费的引用，不授予生产代码借用或所有权捷径。

`ObjCCallHints.SwiftMainActorSharedKeepsObjectAndMetatypeContext` 在 ARM64/x86-64 上检查完整结果与 swiftself 载体，每种架构拒绝十项 ABI 变异和十项导入身份变异。独立 SDK 验证在 ARM64 主机上以 O0/O2 执行两种源码架构配置的原样生成 C：128 次调用保持单例与元类型身份，并平衡引用所有权。八种交叉编译配置覆盖两种架构的 macOS 与 Mac Catalyst；x86-64 原生执行仍是独立覆盖项。

`BitVectorEncodingClone.RootQueuePreservesDecisionsAcrossGrowthAndBudgets` 检查根层已赋值与未决变量混合、非决策根变量、副本再次复制、源对象销毁、后续变量增长、两种默认极性、预算中断与恢复、冲突及重启。完整模型与全部搜索计数必须与全新编码一致。

`ContextFiniteProofs.*` 检查上下文与所有者隔离、所有者替换、令牌移动、精确谓词与有序投影、节点追加、完整与不完整结果、存储上限及 LRU 淘汰。帧测试要求缓存前完成最终唯一性查询，并在命中时保留符号节点限额。

`LinuxPriorityTests.cpp` 检查显式任务状态、线程隔离、缺失观察值、无效 JSON、配置准入和拒绝效果。独立的 x64／AArch64 原始调用程序在 O0／O2 下验证 nice 限制、系统调用参数的 32 位截断、CAP_SYS_NICE／RLIMIT_NICE 权限边界和内核 getpriority 编码。在 `NeverDLinuxProcessTests` 中运行 `LinuxPriority.*` 与 `Backends/LinuxPriorityProcess.*`；涉及共享内核／JSON 时，再运行完整 Linux 进程、Android 原生和进程公共接口测试。缺失的可选原生传输仍明确跳过。

`LinuxKernelAvailability.*` 校验显式缺失输入与配置准入。`Backends/LinuxKernelProcess.*` 使用独立的 x64／AArch64 O0／O2 原始调用程序，确认参数校验前返回 ENOSYS，且未指定及无关调用仍被拒绝。Android syscall 样例对照原始 SVC 与 Bionic `syscall`，分别保留原始返回值与 errno 效果。可用性变更须运行这些重点测试、完整 Linux 进程与进程公共接口测试，以及 Android syscall、原生入口和信号测试。

`CompletedQueryCache.*` 覆盖完整字节域答案、所有紧凑槽位、增长、上下文及所有者隔离、无效与不完整输入和精确存储边界。原生分支回归保持固定逻辑查询成本、精确预算和少一预算拒绝，即使完整答案省去了后端工作。


`BinaryLowIRRefinement.NativeTargetDomainsKeepIndependentProjections` / `FrameOffsets.FrameAndJointTargetProjectionsKeepIndependentSearches` 检查带分支变化和符号帧写入的重复原生目标链、固定逻辑开销、精确及少一次查询预算、无效目标限额、门预算耗尽和错误终点观察。交错的帧与相关目标投影还在谓词替换前后保持完整元组、观察顺序及不完整结果拒绝。
`FrameOffsets.Cached*` 覆盖根高位保持任意的地址平移、无符号回绕、和式形状变化、两种缓存模式、谓词隔离、零容量、查询／节点预算拒绝，以及空域与非唯一域的区别。首次请求保留完整求解证明；后续平移可在没有剩余查询预算时使用已经完成的证明。

## 已发布 Android GKI 内核契约

`AndroidTestExecution.def` 为 `FiniteRegistryRejectsBeforeSuccessAndCanBeReused` 设置 120 秒的整项 CTest 预算与 `RUN_SERIAL`。其中两次工作负载各自保留 30 秒的有限运行预算，串行约束避免容量压力用例相互争抢资源。六种 O0/O2 与重定位配置均采用此规则。

`LinuxPIDFD.*` 在加载前检查已发布分支解析、非法枚举、GKI 与缺失观测冲突，以及畸形、过量或矛盾的任务目录。`Backends/LinuxPIDFDProcess.*` 用独立 O0/O2 x64/AArch64 调用程序覆盖八个分支的标志、共用分配、限制、关闭复用、封闭目录、各版本非组首领错误及标量/向量顺序；缺失目标和非首领检查先于 FD 耗尽，并覆盖线程标志及仅隐式 self 的空目录。向量用例比较早期负长度与后续不可访问元数据，以及原始跨度越过用户上限、截断跨度却有效的缓冲区，覆盖 pidfd 和两个捕获流。Android 的 `ReleasedGKIProcessDescriptorsShareRawAndBionicOwnership`、`ReleasedGKIVectorImportRetainsRawAndBionicErrors` 和 `ReleasedGKICatalogueRetainsRawAndBionicLookupErrors` 在六种编译重定位配置中保留原始错误、Bionic errno、目录和耗尽规则。先运行这些定向测试，再运行完整 Linux 进程、Android 原生和公开进程套件。测试执行模型，不启动八个 GKI 内核。参见[已发布 GKI 契约](../android-gki-kernels.md)。

`ProcessCPUClocksRetainIdentityAndIdleSeparation`, `ProcessCPUClocksKeepMissingObservationBoundaries`, `LinuxClock.ProcessCPUObservationsShareAliasesAndRemainFixedWhileIdle`: `ProcessCPUClocksRetainIdentityAndIdleSeparation`、`ProcessCPUClocksKeepMissingObservationBoundaries` 与 `LinuxClock.ProcessCPUObservationsShareAliasesAndRemainFixedWhileIdle` 检查八种固定版本的 raw／Bionic 身份、PROF／VIRT／SCHED、低 32 位参数、目标校验先于指针故障、观察值缺失、别名、CPU 非负值及墙钟／CPU 空闲分离。`AndroidTimeTests.cpp` 检查输出和哨兵；协作式 syscall 样例检查当前非首领 TID 的进程组样本。

`ZeroTimeoutPollRetainsReadinessAndOrderedCopies` 覆盖八个 GKI 分支的 O0／O2 原始调用，检查存活／负数／已关闭描述符、重复计数、参数收窄、超时／掩码顺序、只读零 timespec、全部元数据先于就绪，以及后续故障保留较早 `revents`。`ZeroTimeoutPollKeepsUnobservedBoundaries` 保留内核、限额、掩码、等待和就绪状态的未知边界。Android 的 `ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` 在六种打包配置中复验共享表和 errno 所有权。

## 有界目录批量属性

bulk-attributes 检查完整组、名称/类型集合、未使用字节保护区、low32 FD、bitmap 字、原生错误、dup 共享进度、独立 open、缓存 EOF 和零 rewind。字面值与未知模式仅用于虚拟环境。模型还覆盖完整 stat、失效、NFD/255字节名称、输入/输出别名、传输/预算失败、移动/SWAP/删除/复用及显式授权。每个平台必需63个工作负载：ARM64 为189例，Intel 为126例；本地仅验证匹配的 ARM64 HVF。native5s、guest/Python5,000,000us/quantum1024、public10s 不变。

`MaterializedRuntimePreservesOwnedObjectsOnNativeWindows` 检查原程序的模型执行、恢复、节权限，以及原程序与恢复程序在 Windows 上的原生执行，覆盖私有堆扩容和释放、内部编码指针、FLS 回调重装、递归锁、LastError 及虚拟内存的保留、提交和保护。`MaterializationRequiresKnownSupportedState` 拒绝缺失版本输入及动态 TLS；`RuntimeRestorationHasTheSameCAPIAndCLIContract` 比较精确字节和报告。Linux 构建检查及 Wine 观察不能替代原生 Windows 生命周期证据。

`NeverDUnpackDriverTests` 覆盖 DriverEntry 恢复、静态／动态内核导入、遗留内核资源、入口 ABI／控制状态、畸形导出、调度、请求／卸载生命周期、C API／CLI 一致性及 PE 校验和。原生驱动清单要求对应 KVM／WHP 用例执行；Windows 还对照 ImageHlp。这不代表原生内核加载验收。见[脱壳](unpack.md)。

## 原生不透明状态检查

`X86PreservedState.*` 检查新鲜标量形式、精确寄存器别名、严格重置及字节/操作段/版本陈旧拒绝。`OriginalBinaryUndefinedIndependence.*Opaque*` 覆盖分支、内部调用、完整间接目标、精确配置及独立解码的元数据预算恰好够用/少一单位边界。`BinaryLowIR*.*Opaque*` 覆盖选择见证与任意未定义选择、多归纳源、后期秩/预算拒绝、真实入口标量保持，以及 LowIR 不变但后续源段字节改变时执行摘要必须变化。`NativeUndefinedIndependence.*Opaque*` 和 `NativeStackControl.*FreshMemoryCall*` 检查分组内部、切点前边界、陈旧记录及栈修改前的目标求值。重建受影响的元数据消费者，包括 `NeverDInterpreterLLVMRefinementTests`；分别报告 sanitizer、编译故障注入与普通测试结果。
