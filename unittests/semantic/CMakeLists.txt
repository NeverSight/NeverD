# --- Unicorn (from third_party submodule) ---
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(UNICORN_ARCH "x86;arm;aarch64" CACHE STRING "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/unicorn
                 ${CMAKE_BINARY_DIR}/third_party/unicorn
                 EXCLUDE_FROM_ALL)

# Unicorn's public platform header includes windows.h on MSVC.  Keep the
# Windows min/max macros out of every semantic-test translation unit because
# they corrupt LLVM headers included afterwards (for example
# std::numeric_limits<T>::max()).
if(WIN32)
  add_compile_definitions(NOMINMAX)
endif()

# This binary's large inventory can take several minutes to enumerate on
# hosted Windows runners, so widen its still-finite PRE_TEST discovery bound.
add_neverd_unittest(NeverDSemanticTests
  aarch64/AArch64_AdvNEONRTTests.cpp
  aarch64/AArch64_AdvancedRoundTripTests.cpp
  aarch64/AArch64_AtomicCASPRTTests.cpp
  aarch64/AArch64_AtomicCASRTTests.cpp
  aarch64/AArch64_AtomicClearRTTests.cpp
  aarch64/AArch64_AtomicLSERTTests.cpp
  aarch64/AArch64_AtomicLseOptRTTests.cpp
  aarch64/AArch64_AtomicMinMaxRTTests.cpp
  aarch64/AArch64_AtomicStoreOpRTTests.cpp
  aarch64/AArch64_AtomicSwapAliasRTTests.cpp
  aarch64/AArch64_AutoRoundTripTests.cpp
  aarch64/AArch64_BitManipTests.cpp
  aarch64/AArch64_BitfieldClearRTTests.cpp
  aarch64/AArch64_BitfieldMoveRTTests.cpp
  aarch64/AArch64_CExprRoundTripTests.cpp
  aarch64/AArch64_CarryBitfieldRTTests.cpp
  aarch64/AArch64_CarryPreserveRTTests.cpp
  aarch64/AArch64_CfinvRTTests.cpp
  aarch64/AArch64_ClsCountRTTests.cpp
  aarch64/AArch64_CompactJumpTableRTTests.cpp
  aarch64/AArch64_CondCompareRTTests.cpp
  aarch64/AArch64_CondSelectAliasRTTests.cpp
  aarch64/AArch64_CoreNEON2RTTests.cpp
  aarch64/AArch64_CoreNEONAdvRTTests.cpp
  aarch64/AArch64_CryptoRTTests.cpp
  aarch64/AArch64_DivEdgeRTTests.cpp
  aarch64/AArch64_ExtendRegFlagsRTTests.cpp
  aarch64/AArch64_ExtendedCExprTests.cpp
  aarch64/AArch64_ExtrPairRTTests.cpp
  aarch64/AArch64_FMulxRTTests.cpp
  aarch64/AArch64_FP16ArithRTTests.cpp
  aarch64/AArch64_FP16ConvRTTests.cpp
  aarch64/AArch64_FP16FusedRTTests.cpp
  aarch64/AArch64_FP16PairwiseRTTests.cpp
  aarch64/AArch64_FP16RecipRTTests.cpp
  aarch64/AArch64_FP16ReduceRTTests.cpp
  aarch64/AArch64_FP16WidenFMARTTests.cpp
  aarch64/AArch64_FPConvRoundRTTests.cpp
  aarch64/AArch64_FPSRRTTests.cpp
  aarch64/AArch64_FPRoundTripTests.cpp
  aarch64/AArch64_FPScalarChainRTTests.cpp
  aarch64/AArch64_FRIntTSRTTests.cpp
  aarch64/AArch64_FccmpRTTests.cpp
  aarch64/AArch64_FcvtxnRTTests.cpp
  aarch64/AArch64_FjcvtzsRTTests.cpp
  aarch64/AArch64_FlagChainRTTests.cpp
  aarch64/AArch64_FlagMaskBranchlessRTTests.cpp
  aarch64/AArch64_GPRSubRegAliasRTTests.cpp
  aarch64/AArch64_IndexExtendRTTests.cpp
  aarch64/AArch64_IntCExprExtRTTests.cpp
  aarch64/AArch64_LdnpStnpRTTests.cpp
  aarch64/AArch64_LdpswSextRTTests.cpp
  aarch64/AArch64_LogicFlagsRTTests.cpp
  aarch64/AArch64_MassiveTests.cpp
  aarch64/AArch64_MiscPatternRTTests.cpp
  aarch64/AArch64_MoveWideRTTests.cpp
  aarch64/AArch64_MulHighLongRTTests.cpp
  aarch64/AArch64_NEONAdvOps2RTTests.cpp
  aarch64/AArch64_NEONAdvOpsRTTests.cpp
  aarch64/AArch64_NEONAdvOpsSatRTTests.cpp
  aarch64/AArch64_NEONArithRTTests.cpp
  aarch64/AArch64_NEONByElemWidenRTTests.cpp
  aarch64/AArch64_NEONCoreExtRTTests.cpp
  aarch64/AArch64_NEONCryptoMiscRTTests.cpp
  aarch64/AArch64_NEONExtRTTests.cpp
  aarch64/AArch64_NEONFPAdvRTTests.cpp
  aarch64/AArch64_NEONLaneChainRTTests.cpp
  aarch64/AArch64_NEONMemShufRTTests.cpp
  aarch64/AArch64_NEONPackedChainRTTests.cpp
  aarch64/AArch64_NEONPackedRTTests.cpp
  aarch64/AArch64_NEONPairwiseAbsdRTTests.cpp
  aarch64/AArch64_NEONPerLaneRTTests.cpp
  aarch64/AArch64_NEONRoundTripTests.cpp
  aarch64/AArch64_NEONSatAccWidenRTTests.cpp
  aarch64/AArch64_NEONSatDblMulAccRTTests.cpp
  aarch64/AArch64_NEONSatDblMulhRTTests.cpp
  aarch64/AArch64_NEONShiftNarrowProbeRTTests.cpp
  aarch64/AArch64_NEONShuffleExtRTTests.cpp
  aarch64/AArch64_NEONSubRegAliasRTTests.cpp
  aarch64/AArch64_NEONWidenNarrowRTTests.cpp
  aarch64/AArch64_NarrowHighHalfRTTests.cpp
  aarch64/AArch64_NegCarryRTTests.cpp
  aarch64/AArch64_OrderedWidthRTTests.cpp
  aarch64/AArch64_RmifRTTests.cpp
  aarch64/AArch64_RoundTripSemanticTests.cpp
  aarch64/AArch64_SHA512SM34RTTests.cpp
  aarch64/AArch64_ScalarShiftRegRTTests.cpp
  aarch64/AArch64_SemanticTests.cpp
  aarch64/AArch64_SetfFlagsRTTests.cpp
  aarch64/AArch64_ShiftCountRTTests.cpp
  aarch64/AArch64_XAFlagRTTests.cpp
  allplatform/AllPlatform4_KernelRTTests.cpp
  allplatform/AllPlatform_AdvancedPatternTests.cpp
  allplatform/AllPlatform_AlgorithmRTTests.cpp
  allplatform/AllPlatform_AlgorithmTests.cpp
  allplatform/AllPlatform_ComplexAlgoRTTests.cpp
  allplatform/AllPlatform_DataAlgoRTTests.cpp
  allplatform/AllPlatform_EdgeCaseTests.cpp
  allplatform/AllPlatform_FixedPointRTTests.cpp
  allplatform/AllPlatform_HashAlgoRTTests.cpp
  allplatform/AllPlatform_ImageAlgoRTTests.cpp
  allplatform/AllPlatform_KernelAlgoRTTests.cpp
  allplatform/AllPlatform_MathTests.cpp
  allplatform/AllPlatform_NumericAlgoRTTests.cpp
  allplatform/AllPlatform_PolyMulRTTests.cpp
  allplatform/AllPlatform_RareInsnRTTests.cpp
  allplatform/AllPlatform_StateAlgoRTTests.cpp
  allplatform/AllPlatform_StressRTTests.cpp
  allplatform/AllPlatform_StringAlgoRTTests.cpp
  allplatform/abi/AllPlatform_CallArgVarietyRTTests.cpp
  allplatform/abi/AllPlatform_DirectCallAbiRTTests.cpp
  allplatform/abi/AllPlatform_FuncPtrCallRTTests.cpp
  allplatform/abi/AllPlatform_ManyArgAbiRTTests.cpp
  allplatform/abi/AllPlatform_MixedWidthArgAbiRTTests.cpp
  allplatform/abi/AllPlatform_NestedCallRTTests.cpp
  allplatform/abi/AllPlatform_RecursionCallRTTests.cpp
  allplatform/abi/AllPlatform_ReturnWidthRTTests.cpp
  allplatform/abi/AllPlatform_StackArgEscapeRTTests.cpp
  allplatform/abi/AllPlatform_StackArgWidthRTTests.cpp
  allplatform/abi/AllPlatform_StackMixedWidthRTTests.cpp
  allplatform/abi/AllPlatform_StructAbiRTTests.cpp
  allplatform/abi/AllPlatform_TailCallVarietyRTTests.cpp
  allplatform/abi/AllPlatform_VariadicAbiRTTests.cpp
  allplatform/controlflow/AllPlatform_CFGStressRTTests.cpp
  allplatform/controlflow/AllPlatform_ComputedGotoRTTests.cpp
  allplatform/controlflow/AllPlatform_CondExecRTTests.cpp
  allplatform/controlflow/AllPlatform_CondLoopRTTests.cpp
  allplatform/controlflow/AllPlatform_ControlFlowTests.cpp
  allplatform/controlflow/AllPlatform_DynStackCFGRTTests.cpp
  allplatform/controlflow/AllPlatform_SwitchDispatchRTTests.cpp
  allplatform/controlflow/AllPlatform_SwitchModuloRTTests.cpp
  allplatform/controlflow/AllPlatform_SwitchTableRTTests.cpp
  allplatform/controlflow/AllPlatform_SwitchVarietyRTTests.cpp
  allplatform/fp/AllPlatform_FMARoundingRTTests.cpp
  allplatform/fp/AllPlatform_FPCallAbiRTTests.cpp
  allplatform/fp/AllPlatform_FPCompareHashRTTests.cpp
  allplatform/fp/AllPlatform_FPCompareNaNRTTests.cpp
  allplatform/fp/AllPlatform_FPConvEdgeRTTests.cpp
  allplatform/fp/AllPlatform_FPEdgeValueRTTests.cpp
  allplatform/fp/AllPlatform_FPIntSelectRTTests.cpp
  allplatform/fp/AllPlatform_FPMinMaxRTTests.cpp
  allplatform/fp/AllPlatform_FPRoundConvDblRTTests.cpp
  allplatform/fp/AllPlatform_FPRoundConvRTTests.cpp
  allplatform/fp/AllPlatform_FPSelectFlagRTTests.cpp
  allplatform/fp/AllPlatform_FPToIntSatRTTests.cpp
  allplatform/fp/AllPlatform_FPUnsignedCvtRTTests.cpp
  allplatform/fp/AllPlatform_FloatAlgoRTTests.cpp
  allplatform/fp/AllPlatform_RoundingModeRTTests.cpp
  allplatform/memory/AllPlatform_DynAllocaRTTests.cpp
  allplatform/memory/AllPlatform_ExclusivePairRTTests.cpp
  allplatform/memory/AllPlatform_MemAlgo14RTTests.cpp
  allplatform/memory/AllPlatform_MemBarrierLiveRegRTTests.cpp
  allplatform/memory/AllPlatform_MemBuiltinLiveRTTests.cpp
  allplatform/memory/AllPlatform_MemStructRTTests.cpp
  allplatform/memory/AllPlatform_PtrMemRTTests.cpp
  allplatform/memory/AllPlatform_PtrWalkRTTests.cpp
  allplatform/memory/AllPlatform_VLAFrameRTTests.cpp
  allplatform/optstress/AllPlatform_OptStress100RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress101RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress102RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress103RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress104RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress105RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress106RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress107RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress108RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress109RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress10RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress110RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress111RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress112RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress113RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress114RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress115RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress116RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress117RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress118RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress119RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress11RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress120RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress121RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress122RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress123RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress124RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress125RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress126RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress127RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress128RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress129RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress12RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress130RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress131RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress132RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress133RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress134RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress135RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress136RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress137RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress138RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress139RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress13RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress140RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress141RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress142RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress143RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress144RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress145RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress146RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress147RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress148RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress149RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress14RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress150RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress151RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress152RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress153RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress154RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress155RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress156RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress157RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress158RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress159RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress15RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress160RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress161RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress162RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress163RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress164RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress165RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress166RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress167RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress168RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress169RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress16RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress170RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress171RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress172RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress173RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress174RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress175RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress176RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress177RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress178RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress179RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress17RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress180RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress181RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress182RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress183RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress184RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress185RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress186RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress187RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress188RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress189RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress18RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress190RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress191RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress192RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress193RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress194RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress195RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress196RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress197RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress198RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress199RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress19RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress200RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress201RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress202RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress203RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress204RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress205RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress206RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress207RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress208RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress209RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress20RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress210RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress211RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress212RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress213RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress214RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress215RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress216RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress217RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress218RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress219RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress21RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress220RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress221RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress222RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress223RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress224RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress225RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress226RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress227RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress228RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress229RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress22RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress230RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress231RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress232RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress233RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress234RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress235RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress236RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress237RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress238RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress239RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress23RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress240RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress241RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress242RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress243RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress244RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress245RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress246RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress247RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress248RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress249RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress24RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress250RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress251RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress252RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress253RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress254RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress255RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress256RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress257RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress258RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress259RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress25RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress260RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress261RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress262RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress263RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress264RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress265RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress266RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress267RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress268RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress269RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress26RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress270RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress271RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress272RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress273RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress274RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress275RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress276RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress277RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress278RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress279RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress27RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress280RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress281RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress282RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress283RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress284RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress285RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress286RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress287RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress288RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress289RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress28RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress290RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress291RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress292RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress293RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress294RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress295RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress296RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress297RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress298RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress299RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress29RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress2RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress300RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress301RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress302RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress303RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress304RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress305RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress306RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress307RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress308RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress309RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress30RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress310RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress311RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress312RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress313RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress314RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress315RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress316RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress317RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress318RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress319RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress31RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress320RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress321RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress322RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress323RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress324RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress325RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress326RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress327RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress328RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress329RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress32RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress330RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress331RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress332RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress333RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress334RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress335RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress336RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress337RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress338RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress339RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress33RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress34RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress35RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress36RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress37RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress38RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress39RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress3RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress40RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress41RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress42RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress43RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress44RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress45RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress46RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress47RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress48RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress49RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress4RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress50RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress51RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress52RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress53RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress54RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress55RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress56RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress57RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress58RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress59RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress5RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress60RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress61RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress62RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress63RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress64RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress65RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress66RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress67RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress68RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress69RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress6RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress70RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress71RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress72RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress73RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress74RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress75RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress76RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress77RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress78RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress79RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress7RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress80RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress81RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress82RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress83RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress84RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress85RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress86RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress87RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress88RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress89RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress8RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress90RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress91RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress92RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress93RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress94RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress95RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress96RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress97RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress98RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress99RTTests.cpp
  allplatform/optstress/AllPlatform_OptStress9RTTests.cpp
  allplatform/optstress/AllPlatform_OptStressRTTests.cpp
  allplatform/rodata/AllPlatform_BigGlobalRTTests.cpp
  allplatform/rodata/AllPlatform_ConstPoolRTTests.cpp
  allplatform/rodata/AllPlatform_ConstTableIdxRTTests.cpp
  allplatform/rodata/AllPlatform_ConstTableWalkRTTests.cpp
  allplatform/rodata/AllPlatform_PtrTableIdx2RTTests.cpp
  allplatform/rodata/AllPlatform_PtrTableIdxRTTests.cpp
  allplatform/rodata/AllPlatform_RodataHoist2RTTests.cpp
  allplatform/rodata/AllPlatform_RodataHoist3RTTests.cpp
  allplatform/rodata/AllPlatform_RodataHoistRTTests.cpp
  allplatform/rodata/AllPlatform_RodataInductionRTTests.cpp
  allplatform/rodata/AllPlatform_SelectBaseTableRTTests.cpp
  allplatform/scalar/AllPlatform_BitAlgo11RTTests.cpp
  allplatform/scalar/AllPlatform_BitAlgo12RTTests.cpp
  allplatform/scalar/AllPlatform_BitAlgo13RTTests.cpp
  allplatform/scalar/AllPlatform_BitFieldRTTests.cpp
  allplatform/scalar/AllPlatform_BitManip2RTTests.cpp
  allplatform/scalar/AllPlatform_BitfieldStructRTTests.cpp
  allplatform/scalar/AllPlatform_CondCompareChainRTTests.cpp
  allplatform/scalar/AllPlatform_ConstFoldWidthRTTests.cpp
  allplatform/scalar/AllPlatform_DivConstRTTests.cpp
  allplatform/scalar/AllPlatform_DivZeroRTTests.cpp
  allplatform/scalar/AllPlatform_FlagFoldEdge2RTTests.cpp
  allplatform/scalar/AllPlatform_FlagStressRTTests.cpp
  allplatform/scalar/AllPlatform_LoopCarriedSubwordRTTests.cpp
  allplatform/scalar/AllPlatform_OverflowBuiltinRTTests.cpp
  allplatform/scalar/AllPlatform_RevBitfieldRTTests.cpp
  allplatform/scalar/AllPlatform_ScalarMixRTTests.cpp
  allplatform/scalar/AllPlatform_ShiftedRegOperandRTTests.cpp
  allplatform/scalar/AllPlatform_SignedFlagFoldRTTests.cpp
  allplatform/scalar/AllPlatform_SubRegMixRTTests.cpp
  allplatform/vector/AllPlatform_ComplexArithRTTests.cpp
  allplatform/vector/AllPlatform_DotProductRTTests.cpp
  allplatform/vector/AllPlatform_PackedConstInitRTTests.cpp
  allplatform/vector/AllPlatform_PermuteAlgoRTTests.cpp
  allplatform/vector/AllPlatform_RoundShiftRTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo10RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo11RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo12RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo13RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo14RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo15RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo16RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo17RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo18RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo19RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo20RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo21RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo22RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo23RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo24RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo25RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo26RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo27RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo28RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo29RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo2RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo30RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo31RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo32RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo33RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo34RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo35RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo36RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo37RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo38RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo39RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo3RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo40RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo41RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo42RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo43RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo44RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo45RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo46RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo47RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo48RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo49RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo4RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo50RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo51RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo52RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo53RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo5RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo6RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo7RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo8RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgo9RTTests.cpp
  allplatform/vector/AllPlatform_VectorAlgoRTTests.cpp
  allplatform/vector/AllPlatform_VectorReduceRTTests.cpp
  allplatform/wide64/AllPlatform_CarryChainRTTests.cpp
  allplatform/wide64/AllPlatform_Cmp64RTTests.cpp
  allplatform/wide64/AllPlatform_LongLong64RTTests.cpp
  allplatform/wide64/AllPlatform_MultiWordAbsRTTests.cpp
  allplatform/wide64/AllPlatform_Wide64CallAbiRTTests.cpp
  allplatform/wide64/AllPlatform_Wide64MemRTTests.cpp
  allplatform/wide64/AllPlatform_Wide64MulRTTests.cpp
  allplatform/wide64/AllPlatform_Wide64On32RTTests.cpp
  allplatform/wide64/AllPlatform_Wide64OptRTTests.cpp
  allplatform/wide64/AllPlatform_Wide64SelectRTTests.cpp
  allplatform/wide64/AllPlatform_WideHash64RTTests.cpp
  arm32/ARM32_AcqRelWidthRTTests.cpp
  arm32/ARM32_AdvancedNEONRTTests.cpp
  arm32/ARM32_AdvancedRoundTripTests.cpp
  arm32/ARM32_AutoRoundTripTests.cpp
  arm32/ARM32_BitManipTests.cpp
  arm32/ARM32_BitfieldCarryRTTests.cpp
  arm32/ARM32_CExprRoundTripTests.cpp
  arm32/ARM32_CarryPreserveRTTests.cpp
  arm32/ARM32_CoreExtRTTests.cpp
  arm32/ARM32_CoreMemFPRTTests.cpp
  arm32/ARM32_Crc32RTTests.cpp
  arm32/ARM32_CryptoRTTests.cpp
  arm32/ARM32_DSPMulSatRTTests.cpp
  arm32/ARM32_DivFlagRTTests.cpp
  arm32/ARM32_DualMulAccumRTTests.cpp
  arm32/ARM32_ExtendRotRTTests.cpp
  arm32/ARM32_ExtendedCExprTests.cpp
  arm32/ARM32_FPRoundTripTests.cpp
  arm32/ARM32_FPScalarChainRTTests.cpp
  arm32/ARM32_FlagMaskBranchlessRTTests.cpp
  arm32/ARM32_IntCExprExtRTTests.cpp
  arm32/ARM32_LdmStmModeRTTests.cpp
  arm32/ARM32_LdrdStrdModeRTTests.cpp
  arm32/ARM32_LogicShiftCarryRTTests.cpp
  arm32/ARM32_MassiveTests.cpp
  arm32/ARM32_MiscPatternRTTests.cpp
  arm32/ARM32_NEONAdvRTTests.cpp
  arm32/ARM32_NEONArithSatRTTests.cpp
  arm32/ARM32_NEONExtOpsRTTests.cpp
  arm32/ARM32_NEONExtendedRTTests.cpp
  arm32/ARM32_NEONHalvingNarrowRTTests.cpp
  arm32/ARM32_NEONMiscRTTests.cpp
  arm32/ARM32_NEONOps2RTTests.cpp
  arm32/ARM32_NEONPackedRTTests.cpp
  arm32/ARM32_NEONPerLaneRTTests.cpp
  arm32/ARM32_NEONRoundTripTests.cpp
  arm32/ARM32_NEONSatShiftRTTests.cpp
  arm32/ARM32_NEONScalarWidenMACRTTests.cpp
  arm32/ARM32_NEONShiftAdvRTTests.cpp
  arm32/ARM32_NEONShiftTableProbeRTTests.cpp
  arm32/ARM32_NEONSubRegAliasRTTests.cpp
  arm32/ARM32_NEONVFPAdvRTTests.cpp
  arm32/ARM32_NEONVecOpsRTTests.cpp
  arm32/ARM32_ParallelSIMDRTTests.cpp
  arm32/ARM32_ParallelSatHalveRTTests.cpp
  arm32/ARM32_PopPushSingleRTTests.cpp
  arm32/ARM32_RRXCarryRTTests.cpp
  arm32/ARM32_RoundTripSemanticTests.cpp
  arm32/ARM32_SMMulWordRTTests.cpp
  arm32/ARM32_SatArithRTTests.cpp
  arm32/ARM32_SemanticTests.cpp
  arm32/ARM32_ShiftAmountSatRTTests.cpp
  arm32/ARM32_ShiftCarryRTTests.cpp
  arm32/ARM32_ShiftCountRTTests.cpp
  arm32/ARM32_ShiftedIndexRTTests.cpp
  arm32/ARM32_UmaalDoubleAccRTTests.cpp
  arm32/ARM32_VFPNEONChainRTTests.cpp
  arm32/ARM_PairRegAddrRTTests.cpp
  arm32/ARM_PostIndexAddrRTTests.cpp
  common/CLIEndToEndTests.cpp
  common/ControlFlowRoundTripTests.cpp
  common/EdgeCaseNEONTests.cpp
  common/IRExecRoundTripTests.cpp
  common/IRStageTests.cpp
  common/Int128MathRTTests.cpp
  common/KnownBugTests.cpp
  common/PipelineSemanticTests.cpp
  common/PreviouslyUnsupportedTests.cpp
  common/RewriteCodegenAArch64RTTests.cpp
  common/RewriteCodegenAArch64RelocRTTests.cpp
  common/RewriteCodegenARM32RTTests.cpp
  common/RewriteCodegenARM32RelocRTTests.cpp
  common/RewriteCodegenCOFFAArch64RTTests.cpp
  common/RewriteCodegenCOFFAArch64RelocRTTests.cpp
  common/RewriteCodegenCOFFARM32RTTests.cpp
  common/RewriteCodegenCOFFARM32RelocRTTests.cpp
  common/RewriteCodegenCOFFI386RTTests.cpp
  common/RewriteCodegenCOFFI386RelocRTTests.cpp
  common/RewriteCodegenCOFFX64RTTests.cpp
  common/RewriteCodegenCOFFX64RelocRTTests.cpp
  common/RewriteCodegenHarness.cpp
  common/RewriteCodegenI386RTTests.cpp
  common/RewriteCodegenI386RelocRTTests.cpp
  common/RewriteCodegenInplaceRTTests.cpp
  common/RewriteCodegenMIRRTTests.cpp
  common/RewriteCodegenMachOAArch64RTTests.cpp
  common/RewriteCodegenMachOAArch64RelocRTTests.cpp
  common/RewriteCodegenMachOX64RTTests.cpp
  common/RewriteCodegenMachOX64RelocRTTests.cpp
  common/RewriteCodegenX64RTTests.cpp
  common/RewriteCodegenX64RelocRTTests.cpp
  x86/X86_DoubleShiftZeroRTTests.cpp
  x86/X86_FlagPreserveRTTests.cpp
  x86/X86_FpI64CvtRTTests.cpp
  x86/X86_ParityFlagRTTests.cpp
  x86/X86_PartialRegAliasRTTests.cpp
  x86/X86_SIMDEdgeCaseTests.cpp
  x86/X86_SimdMovdSpillZeroExtRTTests.cpp
  x86/X86_X87CMovRTTests.cpp
  x86/X86_X87CompareRTTests.cpp
  x86/X86_X87ConstLoadRTTests.cpp
  x86/X86_X87FlagFoldRTTests.cpp
  x86/X86_X87Float32RTTests.cpp
  x86/X86_X87Int64CvtRTTests.cpp
  x86/X86_X87LongDoubleConvRTTests.cpp
  x86/X86_X87LongDoubleRTTests.cpp
  x86/X86_X87StackArithRTTests.cpp
  x86/X86_X87StackBranchRTTests.cpp
  x86/X86_X87StackChainRTTests.cpp
  x86/X86_X87SwitchRTTests.cpp
  x86/X86_X87SwitchTailRTTests.cpp
  x86/X86_X87TranscendentalRTTests.cpp
  x86_32/X86_32_AlgoRTTests.cpp
  x86_32/X86_32_BcdRoundTripTests.cpp
  x86_32/X86_32_BsrParamRegRTTests.cpp
  x86_32/X86_32_FPReturnAbiRTTests.cpp
  x86_32/X86_32_FpWideRoundTripTests.cpp
  x86_32/X86_32_IntRoundTripTests.cpp
  x86_32/X86_32_MemRoundTripTests.cpp
  x86_32/X86_32_MultiArgRTTests.cpp
  x86_32/X86_32_PartialRegAliasRTTests.cpp
  x86_32/X86_32_PartialRegMulDivRTTests.cpp
  x86_32/X86_32_PicRodataRoundTripTests.cpp
  x86_32/X86_32_SSEVectorRTTests.cpp
  x86_32/X86_32_StackParamUpdateRTTests.cpp
  x86_64/X64_ADXCarryChainRTTests.cpp
  x86_64/X64_AVX2CrossLaneRTTests.cpp
  x86_64/X64_AVX2GatherRTTests.cpp
  x86_64/X64_AVX2MagicDivRTTests.cpp
  x86_64/X64_AVX2ReduceRTTests.cpp
  x86_64/X64_AVX2VarShiftPermRTTests.cpp
  x86_64/X64_AVX2Width256RTTests.cpp
  x86_64/X64_AVXPackedRTTests.cpp
  x86_64/X64_AVXPerLaneRTTests.cpp
  x86_64/X64_AVXRoundTripTests.cpp
  x86_64/X64_AdvancedRoundTripTests.cpp
  x86_64/X64_AtomicMemRoundTripTests.cpp
  x86_64/X64_AtomicMemStoreRTTests.cpp
  x86_64/X64_AutoRoundTripTests.cpp
  x86_64/X64_AuxCarryRTTests.cpp
  x86_64/X64_BMI2DepositRTTests.cpp
  x86_64/X64_BMI2DivEdgeRTTests.cpp
  x86_64/X64_BMI2FlagShiftRTTests.cpp
  x86_64/X64_BMIExtTests.cpp
  x86_64/X64_BitCountFlagsRTTests.cpp
  x86_64/X64_BitScanZeroRTTests.cpp
  x86_64/X64_BitTestMemOffsetRTTests.cpp
  x86_64/X64_BitTestRTTests.cpp
  x86_64/X64_BlendCmpRTTests.cpp
  x86_64/X64_BlsFlagsRTTests.cpp
  x86_64/X64_BsrPreserveParamRTTests.cpp
  x86_64/X64_CExprRoundTripTests.cpp
  x86_64/X64_CarryConsumeRTTests.cpp
  x86_64/X64_ControlSemanticTests.cpp
  x86_64/X64_CvtScalarUpperRTTests.cpp
  x86_64/X64_Div128RTTests.cpp
  x86_64/X64_DivMulEdgeRTTests.cpp
  x86_64/X64_DoubleShiftFlagsRTTests.cpp
  x86_64/X64_EnterNestingRTTests.cpp
  x86_64/X64_ExtendedCExprTests.cpp
  x86_64/X64_FMASIMDExtRTTests.cpp
  x86_64/X64_FPAdvancedTests.cpp
  x86_64/X64_FPCompareRTTests.cpp
  x86_64/X64_FPRoundTripTests.cpp
  x86_64/X64_FPSemanticSSEExtTests.cpp
  x86_64/X64_FPSemanticTests.cpp
  x86_64/X64_FPURoundTripTests.cpp
  x86_64/X64_FcomiRTTests.cpp
  x86_64/X64_FlagEdgeProbeRTTests.cpp
  x86_64/X64_FlagMaskBranchlessRTTests.cpp
  x86_64/X64_HighByteRTTests.cpp
  x86_64/X64_HorizAddLaneRTTests.cpp
  x86_64/X64_IncDecOverflowRTTests.cpp
  x86_64/X64_IntCExprExtendedRTTests.cpp
  x86_64/X64_IntegerSemanticTests.cpp
  x86_64/X64_LegacyBitRTTests.cpp
  x86_64/X64_LoopJcxzRTTests.cpp
  x86_64/X64_LoopPartialRegRTTests.cpp
  x86_64/X64_MassiveTests.cpp
  x86_64/X64_MemForwardRTTests.cpp
  x86_64/X64_MemStringRTTests.cpp
  x86_64/X64_MovbeEndianRTTests.cpp
  x86_64/X64_MulFlagsRTTests.cpp
  x86_64/X64_PackedCmpUnordRTTests.cpp
  x86_64/X64_PackedFPRTTests.cpp
  x86_64/X64_PackedMulAddWordRTTests.cpp
  x86_64/X64_PackedMulHighRTTests.cpp
  x86_64/X64_PackedMulHighRoundRTTests.cpp
  x86_64/X64_PackedSignAbsRTTests.cpp
  x86_64/X64_PackedWideMul32RTTests.cpp
  x86_64/X64_PartialFlagRTTests.cpp
  x86_64/X64_PartialRegMulDivRTTests.cpp
  x86_64/X64_PcmpStrRTTests.cpp
  x86_64/X64_PushfPopfRTTests.cpp
  x86_64/X64_RclVarCountRTTests.cpp
  x86_64/X64_RegressionTests.cpp
  x86_64/X64_RepCmpScasRTTests.cpp
  x86_64/X64_RepStringBackwardRTTests.cpp
  x86_64/X64_RepStringRTTests.cpp
  x86_64/X64_RotateCountRTTests.cpp
  x86_64/X64_RoundTripSemanticTests.cpp
  x86_64/X64_SIMDLaneRTTests.cpp
  x86_64/X64_SIMDPackedRTTests.cpp
  x86_64/X64_SIMDPerLaneRTTests.cpp
  x86_64/X64_SIMDRoundTripTests.cpp
  x86_64/X64_SIMDShufflePermRTTests.cpp
  x86_64/X64_SSE42StringRTTests.cpp
  x86_64/X64_SSE4AdvRTTests.cpp
  x86_64/X64_SSEConvBroadcastRTTests.cpp
  x86_64/X64_SSEConversionRTTests.cpp
  x86_64/X64_SSEExtBlendRcpRTTests.cpp
  x86_64/X64_SSEExtractInsertRTTests.cpp
  x86_64/X64_SSEIntProbeRTTests.cpp
  x86_64/X64_SSELogicMovUnpackRTTests.cpp
  x86_64/X64_SSEMovConvRTTests.cpp
  x86_64/X64_SSEPackedAdvRTTests.cpp
  x86_64/X64_SSEPackedChainRTTests.cpp
  x86_64/X64_SSEPackedFPMiscRTTests.cpp
  x86_64/X64_SSEPackedIntMiscRTTests.cpp
  x86_64/X64_SSESatPackRTTests.cpp
  x86_64/X64_SSEScalarChainRTTests.cpp
  x86_64/X64_SSEShufBlendRTTests.cpp
  x86_64/X64_SSESqrtRecipRTTests.cpp
  x86_64/X64_SSEVectorCExprRTTests.cpp
  x86_64/X64_SatArithShuffleRTTests.cpp
  x86_64/X64_ScasCmpsRTTests.cpp
  x86_64/X64_SetccCmovRTTests.cpp
  x86_64/X64_SetccRegPreserveRTTests.cpp
  x86_64/X64_ShiftFlagsRTTests.cpp
  x86_64/X64_ShiftMemDstRTTests.cpp
  x86_64/X64_ShuffleAtomicRTTests.cpp
  x86_64/X64_SimdExtractMemRTTests.cpp
  x86_64/X64_SimdHighLowMoveRTTests.cpp
  x86_64/X64_SimdNarrowOperandRTTests.cpp
  x86_64/X64_StringAtomicRTTests.cpp
  x86_64/X64_StringControlRTTests.cpp
  x86_64/X64_StringDirectionRTTests.cpp
  x86_64/X64_StringPatternTests.cpp
  x86_64/X64_SubregStressTests.cpp
  x86_64/X64_VPermilInLaneRTTests.cpp
  x86_64/X64_VectorAlgo24AVXRTTests.cpp
  x86_64/X64_VexVectorStoreRTTests.cpp
  x86_64/X64_Word16OpRTTests.cpp
  x86_64/X64_Word32Tests.cpp
  x86_64/X64_XaddFlagsRTTests.cpp
  x86_64/X64_XlatbTableRTTests.cpp
  x86_64/X64_x87FPUInlineRTTests.cpp

  DISCOVERY_TIMEOUT 600
)

target_compile_definitions(NeverDSemanticTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDSemanticTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

# LLVM MC libraries for assembly + NeverD SDK for roundtrip verification
target_link_libraries(NeverDSemanticTests PRIVATE
  unicorn
  neverd_shared
  NeverDCodeGen
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDSemanticTests neverd)
endif()

# --- Patch (instruction-substitution) equivalence suite ---
# Separate executable so `check-neverd-patch-full` can run just these cases
# (ctest -L NeverDPatchFullTests).  Verifies the --subst / neverd_patch_full
# transform preserves semantics across 4 ISAs x 3 object formats via Unicorn.
# A handful of fully-obfuscated ARM forms (FarBr / FarLoop / VecWide512 run
# through all 11 L1 passes) compile a single very large function and can take
# several minutes under `ctest -j` on a busy machine; give this suite a larger
# per-test ctest timeout so slow codegen never flakes as a false Timeout.  Its
# 22,960-case list also needs a wider, independently bounded discovery timeout
# (the Unicorn emulation itself is already bounded by a deterministic 200M
# instruction budget, so only codegen wall-clock is load-sensitive).
add_neverd_unittest(NeverDPatchFullTests
  probe/patchfull/PatchFullSubstRTTests.cpp

  TIMEOUT 1800
  DISCOVERY_TIMEOUT 600
)

# This generated 36k-line test harness is prohibitively expensive to optimize
# and does not contain the code under test.  Keep NeverD and LLVM in Release,
# but compile the harness itself without optimization so all CI hosts can reach
# the actual test run in a reasonable time.
if(MSVC)
  target_compile_options(NeverDPatchFullTests PRIVATE /Od /bigobj)
else()
  target_compile_options(NeverDPatchFullTests PRIVATE -O0)
endif()

target_compile_definitions(NeverDPatchFullTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDPatchFullTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDPatchFullTests PRIVATE
  unicorn
  neverd_shared
  NeverDCodeGen
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

# Pipeline's C++ entry points are intentionally not part of the Windows DLL
# ABI.  This test calls them directly, so provide their static definitions on
# Windows instead of relying on neverd_shared to export C++ internals.
if(WIN32)
  target_link_libraries(NeverDPatchFullTests PRIVATE NeverDPipeline)
endif()

if(TARGET neverd)
  add_dependencies(NeverDPatchFullTests neverd)
endif()

# --- Computed-goto / local label-table probing suite ---
# Keep these structural soundness checks in every CI profile.  In particular,
# a runtime roundtrip can appear to pass when an unresolved indirect branch is
# emitted as a call into an interior block of the original image; the fixture
# now requires the local-table cases to produce a real LLVM switch.
add_neverd_unittest(NeverDComputedGotoTests
  allplatform/controlflow/AllPlatform_ComputedGotoRTTests.cpp

  TIMEOUT 600
)

target_compile_definitions(NeverDComputedGotoTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  NEVERD_COMPUTED_GOTO_STRUCTURAL_ONLY=1
)

target_include_directories(NeverDComputedGotoTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDComputedGotoTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDComputedGotoTests neverd)
endif()

# --- Switch / jump-table transformation probing suite ---
# Standalone small TU (compiles in seconds, links against the prebuilt
# neverd_shared) so switch/indirect-branch transformation + backend bugs can be
# hunted incrementally without recompiling the 800-TU NeverDSemanticTests binary
# or the 22k-line PatchFull TU.  Differential lift->recompile roundtrip across
# the 4 ISAs; run just this binary via `ctest -L NeverDSwitchXformTests`.
add_neverd_unittest(NeverDSwitchXformTests
  aarch64/AArch64_CompactJumpTableRTTests.cpp
  probe/switch/AllPlatform_SwitchXformRTTests.cpp
  probe/switch/AllPlatform_SwitchXformSparseRTTests.cpp
  probe/switch/AllPlatform_SwitchXformTableRTTests.cpp

  TIMEOUT 600
)

target_compile_definitions(NeverDSwitchXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDSwitchXformTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDSwitchXformTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDSwitchXformTests neverd)
endif()

# --- Indirect-call / function-pointer transformation probing suite ---
# Standalone small TU (same rationale as NeverDSwitchXformTests): hunt
# INDIR_CALL / function-pointer-table recovery + code-pointer relocation bugs
# incrementally without recompiling the 800-TU NeverDSemanticTests binary.
# Differential lift->recompile roundtrip across the 4 ISAs; run just this
# binary via `ctest -L NeverDIndCallXformTests`.
add_neverd_unittest(NeverDIndCallXformTests
  probe/indcall/AllPlatform_IndCallXformRTTests.cpp

  TIMEOUT 600
)

target_compile_definitions(NeverDIndCallXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDIndCallXformTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDIndCallXformTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDIndCallXformTests neverd)
endif()

# --- CFG / loop structuring transformation probing suite ---
# Standalone small TU (same rationale as NeverDSwitchXformTests): hunt bugs in
# lift->Med->High control-flow reconstruction (CFStructurer / HighLoopRecovery /
# MedToHigh PHI-copy insertion) and the backend re-lowering of the recovered
# structured form, incrementally without recompiling the 800-TU
# NeverDSemanticTests binary.  Differential lift->recompile roundtrip across the
# 4 ISAs at O0/O2; run just this binary via `ctest -L NeverDCFGLoopXformTests`.
add_neverd_unittest(NeverDCFGLoopXformTests
  probe/cfgloop/AllPlatform_CFGLoopXformRTTests.cpp

  TIMEOUT 600
)

target_compile_definitions(NeverDCFGLoopXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDCFGLoopXformTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDCFGLoopXformTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDCFGLoopXformTests neverd)
endif()

# --- Adjacent PIC jump-table probing suite ---
# Standalone small TU (same rationale as NeverDSwitchXformTests): hunt the
# adjacent-unguarded-PIC-table over-read (§15.2) in the jump-table resolver —
# two or more guard-eliminated PIC tables laid out back-to-back in rodata whose
# RelCodeReloc runs merge, so the first table's run count over-reads into the
# next.  Differential lift->recompile roundtrip across the 4 ISAs at O0/O2; run
# just this binary via `ctest -L NeverDTwoTableXformTests`.
add_neverd_unittest(NeverDTwoTableXformTests
  probe/twotable/AllPlatform_TwoTableXformRTTests.cpp

  TIMEOUT 600
)

target_compile_definitions(NeverDTwoTableXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDTwoTableXformTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDTwoTableXformTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDTwoTableXformTests neverd)
endif()

# --- AVX YMM upper-half (VZEROUPPER/VZEROALL/VEX.128) probing suite ---
# Standalone small TU (same rationale as NeverDSwitchXformTests): hunt bugs in
# the AVX upper-128 zeroing model (VEX.128 writes / VZEROUPPER / VZEROALL)
# incrementally without recompiling the 800-TU NeverDSemanticTests binary.
# Differential lift->recompile roundtrip; run via `ctest -L NeverDAvxUpperXformTests`.
add_neverd_unittest(NeverDAvxUpperXformTests
  probe/avxupper/X64_AvxUpperXformRTTests.cpp

  TIMEOUT 600
)

target_compile_definitions(NeverDAvxUpperXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDAvxUpperXformTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDAvxUpperXformTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDAvxUpperXformTests neverd)
endif()

# --- Throwaway fast repro for obfuscation/codegen debugging ---
# Tiny TU (compiles in seconds) mirroring the PatchFullSubstRTTests harness so a
# bug can be isolated without the ~33min rebuild of the 22k-line suite.  Not a
# ctest test; run manually: ./bin/PatchFullRepro <isa> <passes>.
add_executable(PatchFullRepro EXCLUDE_FROM_ALL probe/repro/PatchFullRepro.cpp)
target_include_directories(PatchFullRepro PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)
target_link_libraries(PatchFullRepro PRIVATE
  unicorn
  neverd_shared
  NeverDCodeGen
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(WIN32)
  target_link_libraries(PatchFullRepro PRIVATE NeverDPipeline)
endif()

# Same TU is also listed in NeverDSemanticTests so Linux CI
# (linux-semantic) picks the cases up.  This one-TU binary is only a
# local fast path: rebuild and run without relinking the 800-TU suite.
add_neverd_unittest(NeverDFP16ReduceTests
  aarch64/AArch64_FP16ReduceRTTests.cpp
  TIMEOUT 120
)

target_compile_definitions(NeverDFP16ReduceTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDFP16ReduceTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDFP16ReduceTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDFP16ReduceTests neverd)
endif()

# Focused target for FP16 pairwise and element-wise min/max regressions.  The
# TU also remains part of NeverDSemanticTests for CI ownership.
add_neverd_unittest(NeverDFP16PairwiseTests
  aarch64/AArch64_FP16PairwiseRTTests.cpp
  TIMEOUT 120
)

target_compile_definitions(NeverDFP16PairwiseTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDFP16PairwiseTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDFP16PairwiseTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDFP16PairwiseTests neverd)
endif()

# Same TU remains part of NeverDSemanticTests for CI ownership.  This focused
# target keeps local FP16 conversion regressions fast to rebuild and run.
add_neverd_unittest(NeverDFP16ConvTests
  aarch64/AArch64_FP16ConvRTTests.cpp
  TIMEOUT 120
)

target_compile_definitions(NeverDFP16ConvTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDFP16ConvTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDFP16ConvTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDFP16ConvTests neverd)
endif()

# Focused target for one-rounding FMA regressions.  The TU also remains in the
# aggregate semantic suite used by Linux CI.
add_neverd_unittest(NeverDFMARoundingTests
  allplatform/fp/AllPlatform_FMARoundingRTTests.cpp
  TIMEOUT 120
)

target_compile_definitions(NeverDFMARoundingTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDFMARoundingTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDFMARoundingTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDFMARoundingTests neverd)
endif()

# Focused target for AArch64 FPSR/QC state regressions.  The TU also remains in
# the aggregate semantic suite used by Linux CI.
add_neverd_unittest(NeverDFPSRTests
  aarch64/AArch64_FPSRRTTests.cpp
  TIMEOUT 120
)

target_compile_definitions(NeverDFPSRTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDFPSRTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(NeverDFPSRTests PRIVATE
  unicorn
  neverd_shared
  NeverDPassMIR
  LLVMMC
  LLVMMCParser
  LLVMObject
  LLVMX86AsmParser LLVMX86Desc LLVMX86Info
  LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
  LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
  LLVMSupport
)

if(TARGET neverd)
  add_dependencies(NeverDFPSRTests neverd)
endif()

# LibCNames' C++ helpers are intentionally not exported from neverd_shared on
# Windows.  Every executable that compiles SemanticRoundTripFixture calls them
# directly, so provide their static definitions to all fixture users.  Keep
# this after the focused targets above so the list cannot omit later targets.
if(WIN32)
  foreach(_neverd_semantic_fixture_target IN ITEMS
      NeverDSemanticTests
      NeverDSwitchXformTests
      NeverDIndCallXformTests
      NeverDCFGLoopXformTests
      NeverDTwoTableXformTests
      NeverDAvxUpperXformTests
      NeverDFP16ReduceTests
      NeverDFP16PairwiseTests
      NeverDFP16ConvTests
      NeverDFMARoundingTests
      NeverDFPSRTests)
    target_link_libraries(${_neverd_semantic_fixture_target} PRIVATE NeverDLibC)
  endforeach()
endif()
