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

add_neverd_unittest(NeverDSemanticTests
  X64_IntegerSemanticTests.cpp
  X64_ControlSemanticTests.cpp
  X64_FPSemanticTests.cpp
  AArch64_SemanticTests.cpp
  ARM32_SemanticTests.cpp
  PipelineSemanticTests.cpp
  IRStageTests.cpp
  IRExecRoundTripTests.cpp
  PreviouslyUnsupportedTests.cpp
  X64_RoundTripSemanticTests.cpp
  AArch64_RoundTripSemanticTests.cpp
  ARM32_RoundTripSemanticTests.cpp
  ControlFlowRoundTripTests.cpp
  CLIEndToEndTests.cpp
  X64_AutoRoundTripTests.cpp
  AArch64_AutoRoundTripTests.cpp
  ARM32_AutoRoundTripTests.cpp
  X64_CExprRoundTripTests.cpp
  X64_SubregStressTests.cpp
  X64_FPRoundTripTests.cpp
  X64_AdvancedRoundTripTests.cpp
  AArch64_CExprRoundTripTests.cpp
  AArch64_FPRoundTripTests.cpp
  ARM32_CExprRoundTripTests.cpp
  ARM32_FPRoundTripTests.cpp
  X64_AtomicMemRoundTripTests.cpp
  X64_ExtendedCExprTests.cpp
  X64_StringPatternTests.cpp
  X64_RegressionTests.cpp
  X64_BMIExtTests.cpp
  X64_FPAdvancedTests.cpp
  X64_MassiveTests.cpp
  AArch64_ExtendedCExprTests.cpp
  ARM32_ExtendedCExprTests.cpp
  AArch64_BitManipTests.cpp
  AArch64_MassiveTests.cpp
  AArch64_AdvancedRoundTripTests.cpp
  ARM32_BitManipTests.cpp
  ARM32_MassiveTests.cpp
  ARM32_AdvancedRoundTripTests.cpp
  AllPlatform_EdgeCaseTests.cpp
  AllPlatform_ControlFlowTests.cpp
  AllPlatform_AlgorithmTests.cpp
  AllPlatform_MathTests.cpp
  X64_Word32Tests.cpp
  X64_SIMDRoundTripTests.cpp
  AArch64_NEONRoundTripTests.cpp
  ARM32_NEONRoundTripTests.cpp
  X64_StringAtomicRTTests.cpp
  AllPlatform_AdvancedPatternTests.cpp
  X64_SSEConversionRTTests.cpp
  X64_SatArithShuffleRTTests.cpp
  AArch64_AdvNEONRTTests.cpp
  ARM32_AdvancedNEONRTTests.cpp
  X64_FPCompareRTTests.cpp
  X64_DivMulEdgeRTTests.cpp
  X64_SIMDPackedRTTests.cpp
  AArch64_FlagChainRTTests.cpp
  ARM32_DivFlagRTTests.cpp
  X64_PackedFPRTTests.cpp
  AArch64_NEONPackedRTTests.cpp
  ARM32_NEONPackedRTTests.cpp
  X64_ShuffleAtomicRTTests.cpp
  AArch64_MiscPatternRTTests.cpp
  ARM32_MiscPatternRTTests.cpp
  X64_MemStringRTTests.cpp
  AllPlatform_AlgorithmRTTests.cpp
  X64_SetccCmovRTTests.cpp
  X64_Word16OpRTTests.cpp
  AllPlatform_StressRTTests.cpp
  X64_FMASIMDExtRTTests.cpp
  AArch64_NEONExtRTTests.cpp
  X64_SIMDLaneRTTests.cpp
  X64_SIMDPerLaneRTTests.cpp
  AArch64_NEONPerLaneRTTests.cpp
  X64_AVXRoundTripTests.cpp
  ARM32_NEONPerLaneRTTests.cpp
  AArch64_CoreNEONAdvRTTests.cpp
  X64_LegacyBitRTTests.cpp
  X64_FPURoundTripTests.cpp
  ARM32_NEONAdvRTTests.cpp
  X64_StringControlRTTests.cpp
  KnownBugTests.cpp
  EdgeCaseNEONTests.cpp
  X86_SIMDEdgeCaseTests.cpp
  X64_SSE4AdvRTTests.cpp
  AArch64_NEONAdvOpsRTTests.cpp
  ARM32_NEONVFPAdvRTTests.cpp
  X64_SIMDShufflePermRTTests.cpp
  AArch64_NEONShuffleExtRTTests.cpp
  ARM32_NEONExtendedRTTests.cpp
  X64_AVXPerLaneRTTests.cpp
  X64_BlendCmpRTTests.cpp
  AArch64_NEONAdvOps2RTTests.cpp
  ARM32_NEONOps2RTTests.cpp
  X64_x87FPUInlineRTTests.cpp
  AArch64_FPConvRoundRTTests.cpp
  ARM32_DSPMulSatRTTests.cpp
  X64_SSEVectorCExprRTTests.cpp
  X64_SSEPackedAdvRTTests.cpp
  AArch64_NEONWidenNarrowRTTests.cpp
  AArch64_NEONPairwiseAbsdRTTests.cpp
  AllPlatform_CondLoopRTTests.cpp
  ARM32_NEONVecOpsRTTests.cpp
  X64_SSEConvBroadcastRTTests.cpp
  AArch64_NEONCryptoMiscRTTests.cpp
  ARM32_NEONMiscRTTests.cpp
  X64_SSE42StringRTTests.cpp
  AArch64_AtomicLSERTTests.cpp
  ARM32_CoreExtRTTests.cpp
  X64_SSEPackedFPMiscRTTests.cpp
  X64_SSESqrtRecipRTTests.cpp
  X64_SSEExtractInsertRTTests.cpp
  AArch64_NEONArithRTTests.cpp
  ARM32_NEONArithSatRTTests.cpp
  X64_SSEMovConvRTTests.cpp
  X64_SSEShufBlendRTTests.cpp
  AArch64_NEONMemShufRTTests.cpp
  ARM32_CoreMemFPRTTests.cpp
  X64_SSELogicMovUnpackRTTests.cpp
  AArch64_NEONCoreExtRTTests.cpp
  ARM32_NEONExtOpsRTTests.cpp
  X64_AVXPackedRTTests.cpp
  AArch64_NEONFPAdvRTTests.cpp
  ARM32_NEONShiftAdvRTTests.cpp
  X64_SSEScalarChainRTTests.cpp
  X64_SSEPackedChainRTTests.cpp
  AArch64_FPScalarChainRTTests.cpp
  AArch64_NEONPackedChainRTTests.cpp
  ARM32_FPScalarChainRTTests.cpp
  X64_SSEPackedIntMiscRTTests.cpp
  AArch64_NEONLaneChainRTTests.cpp
  ARM32_VFPNEONChainRTTests.cpp
  X64_IntCExprExtendedRTTests.cpp
  AArch64_IntCExprExtRTTests.cpp
  ARM32_IntCExprExtRTTests.cpp
  AllPlatform_ReturnWidthRTTests.cpp
  AllPlatform_FlagStressRTTests.cpp
  AllPlatform_ComplexAlgoRTTests.cpp
  AllPlatform_VectorAlgoRTTests.cpp
  AllPlatform_VectorAlgo2RTTests.cpp
  AllPlatform_VectorAlgo3RTTests.cpp
  AllPlatform_VectorAlgo4RTTests.cpp
  AllPlatform_VectorAlgo5RTTests.cpp
  AllPlatform_VectorAlgo6RTTests.cpp
  AllPlatform_VectorAlgo7RTTests.cpp
  AllPlatform_VectorAlgo8RTTests.cpp
  AllPlatform_VectorAlgo9RTTests.cpp
  AllPlatform_VectorAlgo10RTTests.cpp
  AllPlatform_VectorAlgo11RTTests.cpp
  AllPlatform_VectorAlgo12RTTests.cpp
  AllPlatform_VectorAlgo13RTTests.cpp
  AllPlatform_VectorAlgo14RTTests.cpp
  AllPlatform_VectorAlgo15RTTests.cpp
  AllPlatform_VectorAlgo16RTTests.cpp
  AllPlatform_SwitchTableRTTests.cpp
  AllPlatform_BitAlgo11RTTests.cpp
  AllPlatform_BitAlgo12RTTests.cpp
  AllPlatform_BitAlgo13RTTests.cpp
  AllPlatform_CarryChainRTTests.cpp
  AllPlatform_DivConstRTTests.cpp
  AllPlatform_FloatAlgoRTTests.cpp
  AllPlatform_MemAlgo14RTTests.cpp
  ARM32_SatArithRTTests.cpp
  X64_SSESatPackRTTests.cpp
  ARM32_NEONSatShiftRTTests.cpp
  AArch64_CryptoRTTests.cpp
  ARM32_CryptoRTTests.cpp
  AllPlatform_HashAlgoRTTests.cpp
  AArch64_NEONSatAccWidenRTTests.cpp
  ARM32_NEONHalvingNarrowRTTests.cpp
  AllPlatform_FixedPointRTTests.cpp
  AllPlatform_StringAlgoRTTests.cpp
  AllPlatform_NumericAlgoRTTests.cpp
  AllPlatform_PolyMulRTTests.cpp
  AllPlatform_ImageAlgoRTTests.cpp
  AllPlatform_DotProductRTTests.cpp
  AllPlatform_PermuteAlgoRTTests.cpp
  AllPlatform_RoundingModeRTTests.cpp
  AllPlatform_FPMinMaxRTTests.cpp
  AllPlatform_VectorReduceRTTests.cpp
  AllPlatform_ComplexArithRTTests.cpp
  ARM32_ParallelSIMDRTTests.cpp
  AllPlatform_FPCompareNaNRTTests.cpp
  X64_RepStringRTTests.cpp
  X64_PartialFlagRTTests.cpp
  X64_HighByteRTTests.cpp
  X64_BitTestRTTests.cpp
  ARM32_ShiftCountRTTests.cpp
  AArch64_DivEdgeRTTests.cpp
  X64_MemForwardRTTests.cpp
  AArch64_SHA512SM34RTTests.cpp
  X64_RotateCountRTTests.cpp
  AArch64_ShiftCountRTTests.cpp
  AArch64_FP16ArithRTTests.cpp
  AArch64_FP16ConvRTTests.cpp
  AArch64_FP16FusedRTTests.cpp
  AArch64_FMulxRTTests.cpp
  AArch64_FP16RecipRTTests.cpp
  AArch64_FP16WidenFMARTTests.cpp
  AArch64_FRIntTSRTTests.cpp
  AArch64_FjcvtzsRTTests.cpp
  AArch64_FcvtxnRTTests.cpp
  X64_ScasCmpsRTTests.cpp
  AArch64_SetfFlagsRTTests.cpp
  AArch64_XAFlagRTTests.cpp
  AArch64_RmifRTTests.cpp
  X64_AuxCarryRTTests.cpp
  X64_PushfPopfRTTests.cpp
  X64_IncDecOverflowRTTests.cpp
  X64_PcmpStrRTTests.cpp
  X64_SimdNarrowOperandRTTests.cpp
  ARM32_SMMulWordRTTests.cpp
  ARM32_ExtendRotRTTests.cpp
  X64_FlagEdgeProbeRTTests.cpp
  AArch64_CarryBitfieldRTTests.cpp
  ARM32_BitfieldCarryRTTests.cpp
  AllPlatform_SignedFlagFoldRTTests.cpp
  AllPlatform_FlagFoldEdge2RTTests.cpp
  X64_BMI2DivEdgeRTTests.cpp
  X64_LoopPartialRegRTTests.cpp
  X64_Div128RTTests.cpp
  X64_ShiftFlagsRTTests.cpp
  ARM32_ShiftCarryRTTests.cpp
  ARM32_LogicShiftCarryRTTests.cpp
  AArch64_LogicFlagsRTTests.cpp
  X64_StringDirectionRTTests.cpp
  AllPlatform_DivZeroRTTests.cpp
  X64_DoubleShiftFlagsRTTests.cpp
  X64_XaddFlagsRTTests.cpp
  AArch64_CoreNEON2RTTests.cpp
  X64_SSEExtBlendRcpRTTests.cpp
  AllPlatform_RoundShiftRTTests.cpp
  AllPlatform_FPToIntSatRTTests.cpp
  AArch64_ExtendRegFlagsRTTests.cpp
  AllPlatform_RevBitfieldRTTests.cpp
  AllPlatform_StackMixedWidthRTTests.cpp
  AArch64_AtomicMinMaxRTTests.cpp
  AArch64_AtomicStoreOpRTTests.cpp
  AArch64_AtomicCASPRTTests.cpp
  ARM32_AcqRelWidthRTTests.cpp
  ARM32_PopPushSingleRTTests.cpp
  AArch64_LdnpStnpRTTests.cpp
  AArch64_OrderedWidthRTTests.cpp
  AArch64_IndexExtendRTTests.cpp
  ARM32_ShiftedIndexRTTests.cpp
  X64_BitScanZeroRTTests.cpp
  ARM32_DualMulAccumRTTests.cpp
  AArch64_CondSelectAliasRTTests.cpp
  AArch64_BitfieldClearRTTests.cpp
  AArch64_AtomicSwapAliasRTTests.cpp
  AArch64_AtomicCASRTTests.cpp
  X64_AtomicMemStoreRTTests.cpp
  X64_SimdExtractMemRTTests.cpp
  X64_SimdHighLowMoveRTTests.cpp
  X64_ShiftMemDstRTTests.cpp
  AArch64_NEONSatDblMulhRTTests.cpp
  X64_PartialRegMulDivRTTests.cpp
  X64_HorizAddLaneRTTests.cpp
  ARM32_ParallelSatHalveRTTests.cpp
  AArch64_MulHighLongRTTests.cpp
  X64_ADXCarryChainRTTests.cpp
  AArch64_CondCompareRTTests.cpp
  X64_VPermilInLaneRTTests.cpp
  X64_BitTestMemOffsetRTTests.cpp
  AArch64_MoveWideRTTests.cpp
  X64_SetccRegPreserveRTTests.cpp
  AArch64_BitfieldMoveRTTests.cpp
  X64_PackedMulHighRoundRTTests.cpp
  X64_PackedWideMul32RTTests.cpp
  ARM32_RRXCarryRTTests.cpp
  ARM32_ShiftAmountSatRTTests.cpp
  X64_PackedMulAddWordRTTests.cpp
  AArch64_ExtrPairRTTests.cpp
  AArch64_ClsCountRTTests.cpp
  X64_PackedCmpUnordRTTests.cpp
  X64_CvtScalarUpperRTTests.cpp
  ARM32_LdmStmModeRTTests.cpp
  ARM32_LdrdStrdModeRTTests.cpp
  X64_MovbeEndianRTTests.cpp
  X64_PackedSignAbsRTTests.cpp
  AArch64_AtomicClearRTTests.cpp
  AArch64_NEONSatDblMulAccRTTests.cpp
  X64_PackedMulHighRTTests.cpp
  X64_VexVectorStoreRTTests.cpp
  AArch64_ScalarShiftRegRTTests.cpp
  AArch64_FccmpRTTests.cpp
  X64_FcomiRTTests.cpp
  AllPlatform_KernelAlgoRTTests.cpp
  AllPlatform_StateAlgoRTTests.cpp
  AllPlatform_DataAlgoRTTests.cpp
  X64_BMI2FlagShiftRTTests.cpp
  AArch64_NegCarryRTTests.cpp
  ARM32_UmaalDoubleAccRTTests.cpp
  X64_BitCountFlagsRTTests.cpp
  X64_XlatbTableRTTests.cpp
  X64_LoopJcxzRTTests.cpp
  X64_EnterNestingRTTests.cpp
  AArch64_LdpswSextRTTests.cpp
  X64_BlsFlagsRTTests.cpp
  AArch64_CfinvRTTests.cpp
  X64_MulFlagsRTTests.cpp
  X64_CarryConsumeRTTests.cpp
  X64_RclVarCountRTTests.cpp
  AllPlatform_RodataHoistRTTests.cpp
  X64_FlagMaskBranchlessRTTests.cpp
  AArch64_FlagMaskBranchlessRTTests.cpp
  ARM32_FlagMaskBranchlessRTTests.cpp
  AllPlatform_RodataHoist2RTTests.cpp
  AllPlatform_RodataHoist3RTTests.cpp
  X86_32_IntRoundTripTests.cpp
  X86_32_MemRoundTripTests.cpp
  X86_32_PicRodataRoundTripTests.cpp
  X86_32_FpWideRoundTripTests.cpp
  X86_32_BcdRoundTripTests.cpp
  AllPlatform_LongLong64RTTests.cpp
  X86_32_MultiArgRTTests.cpp
  AllPlatform_ShiftedRegOperandRTTests.cpp
  X86_X87Float32RTTests.cpp
  X86_X87CompareRTTests.cpp
  X86_X87CMovRTTests.cpp
  X86_X87FlagFoldRTTests.cpp
  X86_X87ConstLoadRTTests.cpp
  X86_X87StackArithRTTests.cpp
  Int128MathRTTests.cpp
  AllPlatform_OverflowBuiltinRTTests.cpp
  AllPlatform_BitfieldStructRTTests.cpp
  AllPlatform_FPIntSelectRTTests.cpp
  AllPlatform_FPUnsignedCvtRTTests.cpp
  AllPlatform_ScalarMixRTTests.cpp
  X64_RepStringBackwardRTTests.cpp
  ARM32_Crc32RTTests.cpp
  AllPlatform_ConstPoolRTTests.cpp
  AllPlatform_VectorAlgo17RTTests.cpp
  AllPlatform_VectorAlgo18RTTests.cpp
  AArch64_NEONShiftNarrowProbeRTTests.cpp
  X64_SSEIntProbeRTTests.cpp
  ARM32_NEONShiftTableProbeRTTests.cpp
  AllPlatform_OptStressRTTests.cpp
  X86_X87TranscendentalRTTests.cpp
  ARM32_NEONScalarWidenMACRTTests.cpp
  AArch64_NEONByElemWidenRTTests.cpp
  AllPlatform_VectorAlgo19RTTests.cpp
  AllPlatform_VectorAlgo20RTTests.cpp
  X86_32_AlgoRTTests.cpp
  X86_32_SSEVectorRTTests.cpp
  X86_SimdMovdSpillZeroExtRTTests.cpp
  AllPlatform_VectorAlgo21RTTests.cpp
  AllPlatform_VectorAlgo22RTTests.cpp
  AllPlatform_VectorAlgo23RTTests.cpp
  AllPlatform_VectorAlgo24RTTests.cpp
  X64_VectorAlgo24AVXRTTests.cpp
  AArch64_NarrowHighHalfRTTests.cpp
  X86_DoubleShiftZeroRTTests.cpp
  AllPlatform_VectorAlgo25RTTests.cpp
  AllPlatform_VectorAlgo26RTTests.cpp
  AllPlatform4_KernelRTTests.cpp
  ARM_PostIndexAddrRTTests.cpp
  AllPlatform_PtrWalkRTTests.cpp
  ARM_PairRegAddrRTTests.cpp
  AllPlatform_VectorAlgo27RTTests.cpp
  AllPlatform_VectorAlgo28RTTests.cpp
  AllPlatform_ExclusivePairRTTests.cpp
  X86_PartialRegAliasRTTests.cpp
  X86_32_PartialRegAliasRTTests.cpp
  X86_FlagPreserveRTTests.cpp
  AllPlatform_ConstFoldWidthRTTests.cpp
  AllPlatform_FPEdgeValueRTTests.cpp
  AllPlatform_WideHash64RTTests.cpp
  AllPlatform_MultiWordAbsRTTests.cpp
  AllPlatform_Cmp64RTTests.cpp
  AllPlatform_FPCompareHashRTTests.cpp
  AllPlatform_FPSelectFlagRTTests.cpp
  AllPlatform_CondCompareChainRTTests.cpp
  X86_32_PartialRegMulDivRTTests.cpp
  AllPlatform_RodataInductionRTTests.cpp
  AllPlatform_ConstTableWalkRTTests.cpp
  AllPlatform_VectorAlgo29RTTests.cpp
  AllPlatform_RareInsnRTTests.cpp
  AllPlatform_SwitchTableRTTests.cpp
  AllPlatform_SwitchDispatchRTTests.cpp
  AllPlatform_OptStress2RTTests.cpp
  AllPlatform_BitManip2RTTests.cpp
  AllPlatform_Wide64On32RTTests.cpp
  AllPlatform_CondExecRTTests.cpp
  AllPlatform_MemStructRTTests.cpp
  AllPlatform_ComputedGotoRTTests.cpp
  AllPlatform_RecursionCallRTTests.cpp
  AllPlatform_DynStackCFGRTTests.cpp
  AllPlatform_OptStress3RTTests.cpp
  AllPlatform_FuncPtrCallRTTests.cpp
  AllPlatform_SwitchModuloRTTests.cpp
  AllPlatform_OptStress4RTTests.cpp
  AllPlatform_SwitchVarietyRTTests.cpp
  AllPlatform_VLAFrameRTTests.cpp
  AllPlatform_PtrMemRTTests.cpp
  AllPlatform_DirectCallAbiRTTests.cpp
  AllPlatform_OptStress5RTTests.cpp
  AllPlatform_OptStress6RTTests.cpp
  AllPlatform_DynAllocaRTTests.cpp
  AllPlatform_CallArgVarietyRTTests.cpp
  AllPlatform_LoopCarriedSubwordRTTests.cpp
  AllPlatform_FPConvEdgeRTTests.cpp
  AllPlatform_ConstTableIdxRTTests.cpp
  AllPlatform_PackedConstInitRTTests.cpp
  AllPlatform_Wide64SelectRTTests.cpp
  AllPlatform_NestedCallRTTests.cpp
  AllPlatform_OptStress7RTTests.cpp
  AllPlatform_TailCallVarietyRTTests.cpp
  AllPlatform_FPCallAbiRTTests.cpp
  AllPlatform_OptStress8RTTests.cpp
  AllPlatform_OptStress9RTTests.cpp
  AllPlatform_Wide64CallAbiRTTests.cpp
  AllPlatform_Wide64OptRTTests.cpp
  AllPlatform_StackArgWidthRTTests.cpp
  AllPlatform_OptStress10RTTests.cpp
  X64_AVX2ReduceRTTests.cpp
  AllPlatform_SubRegMixRTTests.cpp
  AllPlatform_CFGStressRTTests.cpp
  AllPlatform_OptStress11RTTests.cpp
  AllPlatform_StructAbiRTTests.cpp
  AllPlatform_OptStress12RTTests.cpp
  AllPlatform_VariadicAbiRTTests.cpp
  AllPlatform_OptStress13RTTests.cpp
  AllPlatform_OptStress14RTTests.cpp
  AllPlatform_OptStress15RTTests.cpp
  AllPlatform_OptStress16RTTests.cpp
  AllPlatform_OptStress17RTTests.cpp
  AllPlatform_OptStress18RTTests.cpp
  AllPlatform_ManyArgAbiRTTests.cpp
  AllPlatform_OptStress19RTTests.cpp
  AllPlatform_OptStress20RTTests.cpp
  X86_32_FPReturnAbiRTTests.cpp
  AllPlatform_OptStress21RTTests.cpp
  AllPlatform_OptStress22RTTests.cpp
  AllPlatform_OptStress23RTTests.cpp
  AllPlatform_OptStress24RTTests.cpp
  AllPlatform_OptStress25RTTests.cpp
  AllPlatform_OptStress26RTTests.cpp
  AllPlatform_OptStress27RTTests.cpp
  AllPlatform_OptStress28RTTests.cpp
  AllPlatform_OptStress29RTTests.cpp
  AllPlatform_OptStress30RTTests.cpp
  AllPlatform_OptStress31RTTests.cpp
  AllPlatform_OptStress32RTTests.cpp
  AllPlatform_OptStress33RTTests.cpp
  AllPlatform_OptStress34RTTests.cpp
  AllPlatform_OptStress35RTTests.cpp
  AllPlatform_OptStress36RTTests.cpp
  AllPlatform_OptStress37RTTests.cpp
  AllPlatform_OptStress38RTTests.cpp
  AllPlatform_OptStress39RTTests.cpp
  AllPlatform_Wide64MemRTTests.cpp
  AllPlatform_Wide64MulRTTests.cpp
  AllPlatform_VectorAlgo30RTTests.cpp
  X86_X87LongDoubleRTTests.cpp
  X86_X87LongDoubleConvRTTests.cpp
  X86_X87StackBranchRTTests.cpp
  X86_X87StackChainRTTests.cpp
  X86_X87SwitchRTTests.cpp
  X86_X87SwitchTailRTTests.cpp
  AllPlatform_OptStress40RTTests.cpp
  AllPlatform_BitFieldRTTests.cpp
  X64_AVX2Width256RTTests.cpp
  X64_AVX2VarShiftPermRTTests.cpp
  X64_AVX2MagicDivRTTests.cpp
  X64_AVX2CrossLaneRTTests.cpp
  X64_AVX2GatherRTTests.cpp
  AllPlatform_OptStress41RTTests.cpp
  AllPlatform_OptStress42RTTests.cpp
  AllPlatform_OptStress43RTTests.cpp
  AllPlatform_OptStress44RTTests.cpp
  X64_BMI2DepositRTTests.cpp
  AllPlatform_OptStress45RTTests.cpp
  AllPlatform_OptStress46RTTests.cpp
  AllPlatform_OptStress47RTTests.cpp
  AllPlatform_OptStress48RTTests.cpp
  AllPlatform_OptStress49RTTests.cpp
  AllPlatform_PtrTableIdxRTTests.cpp
  AllPlatform_PtrTableIdx2RTTests.cpp
  AllPlatform_OptStress50RTTests.cpp
  AllPlatform_OptStress51RTTests.cpp
  AllPlatform_OptStress52RTTests.cpp
  AllPlatform_OptStress53RTTests.cpp
  AllPlatform_OptStress54RTTests.cpp
  AllPlatform_OptStress55RTTests.cpp
  AllPlatform_OptStress56RTTests.cpp
  AllPlatform_OptStress57RTTests.cpp
  AllPlatform_OptStress58RTTests.cpp
  AllPlatform_OptStress59RTTests.cpp
  AllPlatform_OptStress60RTTests.cpp
  AllPlatform_OptStress61RTTests.cpp
  AllPlatform_OptStress62RTTests.cpp
  AllPlatform_OptStress63RTTests.cpp
  AllPlatform_OptStress64RTTests.cpp
  AllPlatform_OptStress65RTTests.cpp
  AllPlatform_OptStress66RTTests.cpp
  AllPlatform_OptStress67RTTests.cpp
  AllPlatform_OptStress68RTTests.cpp
  AllPlatform_OptStress69RTTests.cpp
  AllPlatform_OptStress70RTTests.cpp
  AllPlatform_OptStress71RTTests.cpp
  AllPlatform_OptStress72RTTests.cpp
  AllPlatform_OptStress73RTTests.cpp
  AllPlatform_OptStress74RTTests.cpp
  AllPlatform_OptStress75RTTests.cpp
  AllPlatform_OptStress76RTTests.cpp
  AllPlatform_OptStress77RTTests.cpp
  AllPlatform_OptStress78RTTests.cpp
  AllPlatform_OptStress79RTTests.cpp
  X86_FpI64CvtRTTests.cpp
  X86_X87Int64CvtRTTests.cpp
  AllPlatform_OptStress80RTTests.cpp
  AllPlatform_OptStress81RTTests.cpp
  AllPlatform_OptStress82RTTests.cpp
  AllPlatform_OptStress83RTTests.cpp
  AllPlatform_OptStress84RTTests.cpp
  AllPlatform_OptStress85RTTests.cpp
  AllPlatform_OptStress86RTTests.cpp
  AllPlatform_OptStress87RTTests.cpp
  AllPlatform_OptStress88RTTests.cpp
  AllPlatform_OptStress89RTTests.cpp
  AllPlatform_OptStress90RTTests.cpp
  AllPlatform_OptStress91RTTests.cpp
  AllPlatform_OptStress92RTTests.cpp
  AllPlatform_OptStress93RTTests.cpp
  AllPlatform_OptStress94RTTests.cpp
  AllPlatform_OptStress95RTTests.cpp
  AllPlatform_OptStress96RTTests.cpp
  AllPlatform_OptStress97RTTests.cpp
  AllPlatform_OptStress98RTTests.cpp
  AllPlatform_OptStress99RTTests.cpp
  AllPlatform_OptStress100RTTests.cpp
  AllPlatform_OptStress101RTTests.cpp
  AllPlatform_OptStress102RTTests.cpp
  AllPlatform_OptStress103RTTests.cpp
  AllPlatform_OptStress104RTTests.cpp
  AllPlatform_OptStress105RTTests.cpp
  AllPlatform_OptStress106RTTests.cpp
  AllPlatform_OptStress107RTTests.cpp
  AllPlatform_OptStress108RTTests.cpp
  AllPlatform_OptStress109RTTests.cpp
  AllPlatform_OptStress110RTTests.cpp
  AllPlatform_OptStress111RTTests.cpp
  AllPlatform_OptStress112RTTests.cpp
  AllPlatform_OptStress113RTTests.cpp
  AllPlatform_OptStress114RTTests.cpp
  AllPlatform_OptStress115RTTests.cpp
  AllPlatform_OptStress116RTTests.cpp
  AllPlatform_OptStress117RTTests.cpp
  AllPlatform_OptStress118RTTests.cpp
  AllPlatform_OptStress119RTTests.cpp
  AllPlatform_OptStress120RTTests.cpp
  AllPlatform_OptStress121RTTests.cpp
  AllPlatform_OptStress122RTTests.cpp
  AllPlatform_OptStress123RTTests.cpp
  AllPlatform_OptStress124RTTests.cpp
  AllPlatform_OptStress125RTTests.cpp
  AllPlatform_OptStress126RTTests.cpp
  AllPlatform_OptStress127RTTests.cpp
  AllPlatform_OptStress128RTTests.cpp
  AllPlatform_OptStress129RTTests.cpp
  AllPlatform_OptStress130RTTests.cpp
  AllPlatform_OptStress131RTTests.cpp
  AllPlatform_OptStress132RTTests.cpp
  AllPlatform_OptStress133RTTests.cpp
  AllPlatform_OptStress134RTTests.cpp
  AllPlatform_OptStress135RTTests.cpp
  AllPlatform_OptStress136RTTests.cpp
  AllPlatform_OptStress137RTTests.cpp
  AllPlatform_OptStress138RTTests.cpp
  AllPlatform_OptStress139RTTests.cpp
  AllPlatform_OptStress140RTTests.cpp
  AllPlatform_OptStress141RTTests.cpp
  AllPlatform_OptStress142RTTests.cpp
  AllPlatform_OptStress143RTTests.cpp
  AllPlatform_OptStress144RTTests.cpp
  AllPlatform_OptStress145RTTests.cpp
  AllPlatform_OptStress146RTTests.cpp
  AllPlatform_OptStress147RTTests.cpp
  AllPlatform_OptStress148RTTests.cpp
  AllPlatform_OptStress149RTTests.cpp
  AllPlatform_OptStress150RTTests.cpp
  AllPlatform_OptStress151RTTests.cpp
  AllPlatform_OptStress152RTTests.cpp
  AllPlatform_OptStress153RTTests.cpp
  AllPlatform_OptStress154RTTests.cpp
  AllPlatform_OptStress155RTTests.cpp
  AllPlatform_OptStress156RTTests.cpp
  AllPlatform_OptStress157RTTests.cpp
  AllPlatform_OptStress158RTTests.cpp
  AllPlatform_OptStress159RTTests.cpp
  AllPlatform_OptStress160RTTests.cpp
  AllPlatform_OptStress161RTTests.cpp
  AllPlatform_OptStress162RTTests.cpp
  AllPlatform_OptStress163RTTests.cpp
  AllPlatform_OptStress164RTTests.cpp
  AllPlatform_OptStress165RTTests.cpp
  AllPlatform_OptStress166RTTests.cpp
  AllPlatform_OptStress167RTTests.cpp
  AllPlatform_OptStress168RTTests.cpp
  AllPlatform_OptStress169RTTests.cpp
  AllPlatform_OptStress170RTTests.cpp
  AllPlatform_OptStress171RTTests.cpp
  AllPlatform_OptStress172RTTests.cpp
  AllPlatform_OptStress173RTTests.cpp
  AllPlatform_OptStress174RTTests.cpp
  AllPlatform_OptStress175RTTests.cpp
  AllPlatform_OptStress176RTTests.cpp
  AllPlatform_OptStress177RTTests.cpp
  AllPlatform_OptStress178RTTests.cpp
  AllPlatform_OptStress179RTTests.cpp
  AllPlatform_OptStress180RTTests.cpp
  AllPlatform_OptStress181RTTests.cpp
  AllPlatform_OptStress182RTTests.cpp
  AllPlatform_OptStress183RTTests.cpp
  AllPlatform_OptStress184RTTests.cpp
  AllPlatform_OptStress185RTTests.cpp
  AllPlatform_OptStress186RTTests.cpp
  AllPlatform_OptStress187RTTests.cpp
  AllPlatform_OptStress188RTTests.cpp
  AllPlatform_OptStress189RTTests.cpp
  AllPlatform_OptStress190RTTests.cpp
  AllPlatform_OptStress191RTTests.cpp
  AllPlatform_OptStress192RTTests.cpp
  AllPlatform_OptStress193RTTests.cpp
  AllPlatform_OptStress194RTTests.cpp
  AllPlatform_OptStress195RTTests.cpp
  AllPlatform_OptStress196RTTests.cpp
  AllPlatform_OptStress197RTTests.cpp
  AllPlatform_OptStress198RTTests.cpp
  AllPlatform_OptStress199RTTests.cpp
  AllPlatform_OptStress200RTTests.cpp
  AllPlatform_OptStress201RTTests.cpp
  AllPlatform_OptStress202RTTests.cpp
  AllPlatform_OptStress203RTTests.cpp
  AllPlatform_OptStress204RTTests.cpp
  AllPlatform_OptStress205RTTests.cpp
  AllPlatform_OptStress206RTTests.cpp
  AllPlatform_OptStress207RTTests.cpp
  AllPlatform_OptStress208RTTests.cpp
  AllPlatform_OptStress209RTTests.cpp
  AllPlatform_OptStress210RTTests.cpp
  AllPlatform_MemBarrierLiveRegRTTests.cpp
  AArch64_NEONSubRegAliasRTTests.cpp
  ARM32_NEONSubRegAliasRTTests.cpp
  AllPlatform_SelectBaseTableRTTests.cpp
  X86_ParityFlagRTTests.cpp
  X86_32_StackParamUpdateRTTests.cpp
  AllPlatform_StackArgEscapeRTTests.cpp
  AllPlatform_MixedWidthArgAbiRTTests.cpp
  AllPlatform_OptStress211RTTests.cpp
  AllPlatform_OptStress212RTTests.cpp
  AllPlatform_OptStress213RTTests.cpp
  AllPlatform_OptStress214RTTests.cpp
  AllPlatform_OptStress215RTTests.cpp
  AllPlatform_OptStress216RTTests.cpp
  AllPlatform_OptStress217RTTests.cpp
  AllPlatform_OptStress218RTTests.cpp
  AArch64_CarryPreserveRTTests.cpp
  ARM32_CarryPreserveRTTests.cpp
  AllPlatform_OptStress219RTTests.cpp
  X64_BsrPreserveParamRTTests.cpp
  X86_32_BsrParamRegRTTests.cpp
  AArch64_GPRSubRegAliasRTTests.cpp
  AllPlatform_OptStress220RTTests.cpp
  AllPlatform_OptStress221RTTests.cpp
  AllPlatform_OptStress222RTTests.cpp
  AllPlatform_OptStress223RTTests.cpp
  AllPlatform_OptStress224RTTests.cpp
  AllPlatform_OptStress225RTTests.cpp
  AllPlatform_OptStress226RTTests.cpp
  AllPlatform_OptStress227RTTests.cpp
  AllPlatform_OptStress228RTTests.cpp
  AllPlatform_OptStress229RTTests.cpp
  AllPlatform_OptStress230RTTests.cpp
  AllPlatform_OptStress231RTTests.cpp
  AllPlatform_OptStress232RTTests.cpp
  AllPlatform_OptStress233RTTests.cpp
  AllPlatform_OptStress234RTTests.cpp
  AllPlatform_OptStress235RTTests.cpp
  AllPlatform_OptStress236RTTests.cpp
  AllPlatform_OptStress237RTTests.cpp
  AllPlatform_OptStress238RTTests.cpp
  AllPlatform_OptStress239RTTests.cpp
  AllPlatform_OptStress240RTTests.cpp
  AllPlatform_OptStress241RTTests.cpp
  AllPlatform_OptStress242RTTests.cpp
  AllPlatform_OptStress243RTTests.cpp
  AllPlatform_OptStress244RTTests.cpp
  AllPlatform_OptStress245RTTests.cpp
  AllPlatform_OptStress246RTTests.cpp
  AllPlatform_OptStress247RTTests.cpp
  AllPlatform_OptStress248RTTests.cpp
  AllPlatform_OptStress249RTTests.cpp
  AllPlatform_OptStress250RTTests.cpp
  AllPlatform_OptStress251RTTests.cpp
  AllPlatform_OptStress252RTTests.cpp
  AllPlatform_OptStress253RTTests.cpp
  AllPlatform_OptStress254RTTests.cpp
  AllPlatform_OptStress255RTTests.cpp
  AllPlatform_OptStress256RTTests.cpp
  AllPlatform_OptStress257RTTests.cpp
  AllPlatform_OptStress258RTTests.cpp
  AllPlatform_OptStress259RTTests.cpp
  AllPlatform_OptStress260RTTests.cpp
  AllPlatform_OptStress261RTTests.cpp
  AllPlatform_OptStress262RTTests.cpp
  AllPlatform_OptStress263RTTests.cpp
  AllPlatform_OptStress264RTTests.cpp
  AllPlatform_OptStress265RTTests.cpp
  AllPlatform_OptStress266RTTests.cpp
  AllPlatform_OptStress267RTTests.cpp
  AllPlatform_OptStress268RTTests.cpp
  AllPlatform_OptStress269RTTests.cpp
  AllPlatform_OptStress270RTTests.cpp
  AllPlatform_OptStress271RTTests.cpp
  AllPlatform_OptStress272RTTests.cpp
  AllPlatform_OptStress273RTTests.cpp
  AllPlatform_OptStress274RTTests.cpp
  AllPlatform_OptStress275RTTests.cpp
  AllPlatform_OptStress276RTTests.cpp
  AllPlatform_OptStress277RTTests.cpp
  AllPlatform_OptStress278RTTests.cpp
  AllPlatform_OptStress279RTTests.cpp
  AllPlatform_OptStress280RTTests.cpp
  AllPlatform_OptStress281RTTests.cpp
  AllPlatform_OptStress282RTTests.cpp
  AllPlatform_OptStress283RTTests.cpp
  AllPlatform_OptStress284RTTests.cpp
  AllPlatform_OptStress285RTTests.cpp
  AllPlatform_OptStress286RTTests.cpp
  AllPlatform_OptStress287RTTests.cpp
  AllPlatform_OptStress288RTTests.cpp
  AllPlatform_OptStress289RTTests.cpp
  AllPlatform_OptStress290RTTests.cpp
  AllPlatform_OptStress291RTTests.cpp
  AllPlatform_OptStress292RTTests.cpp
  AllPlatform_OptStress293RTTests.cpp
  AllPlatform_OptStress294RTTests.cpp
  AllPlatform_OptStress295RTTests.cpp
  AllPlatform_OptStress296RTTests.cpp
  AllPlatform_OptStress297RTTests.cpp
  AllPlatform_OptStress298RTTests.cpp
  AllPlatform_OptStress299RTTests.cpp
  AllPlatform_OptStress300RTTests.cpp
  AllPlatform_VectorAlgo31RTTests.cpp
  AllPlatform_VectorAlgo32RTTests.cpp
  AllPlatform_VectorAlgo33RTTests.cpp
  AllPlatform_VectorAlgo34RTTests.cpp
  AllPlatform_VectorAlgo35RTTests.cpp
  AllPlatform_VectorAlgo36RTTests.cpp
  AllPlatform_VectorAlgo37RTTests.cpp
  AllPlatform_VectorAlgo38RTTests.cpp
  AllPlatform_VectorAlgo39RTTests.cpp
  AllPlatform_VectorAlgo40RTTests.cpp
  AllPlatform_VectorAlgo41RTTests.cpp
  AllPlatform_VectorAlgo42RTTests.cpp
  AllPlatform_VectorAlgo43RTTests.cpp
  AllPlatform_VectorAlgo44RTTests.cpp
  AllPlatform_VectorAlgo45RTTests.cpp
  AllPlatform_VectorAlgo46RTTests.cpp
  AllPlatform_VectorAlgo47RTTests.cpp
  AllPlatform_VectorAlgo48RTTests.cpp
  AllPlatform_VectorAlgo49RTTests.cpp
  AllPlatform_VectorAlgo50RTTests.cpp
  AllPlatform_VectorAlgo51RTTests.cpp
  AllPlatform_VectorAlgo52RTTests.cpp
  AllPlatform_VectorAlgo53RTTests.cpp
  AllPlatform_FPRoundConvRTTests.cpp
  AllPlatform_FPRoundConvDblRTTests.cpp
  AllPlatform_FMARoundingRTTests.cpp
  AArch64_AtomicLseOptRTTests.cpp
  AllPlatform_OptStress301RTTests.cpp
  AllPlatform_OptStress302RTTests.cpp
  AllPlatform_OptStress303RTTests.cpp
  AllPlatform_OptStress304RTTests.cpp
  AllPlatform_OptStress305RTTests.cpp
  AllPlatform_OptStress306RTTests.cpp
  AllPlatform_OptStress307RTTests.cpp
  AllPlatform_OptStress308RTTests.cpp
  AllPlatform_OptStress309RTTests.cpp
  AllPlatform_OptStress310RTTests.cpp
  AllPlatform_OptStress311RTTests.cpp
  AllPlatform_OptStress312RTTests.cpp
  AllPlatform_OptStress313RTTests.cpp
  AllPlatform_OptStress314RTTests.cpp
  AllPlatform_OptStress315RTTests.cpp
  AllPlatform_OptStress316RTTests.cpp
  AllPlatform_OptStress317RTTests.cpp
  AllPlatform_OptStress318RTTests.cpp
  AllPlatform_OptStress319RTTests.cpp
  AllPlatform_OptStress320RTTests.cpp
  AllPlatform_OptStress321RTTests.cpp
  AllPlatform_OptStress322RTTests.cpp
  AllPlatform_OptStress323RTTests.cpp
  AllPlatform_OptStress324RTTests.cpp
  AllPlatform_OptStress325RTTests.cpp
  AllPlatform_OptStress326RTTests.cpp
  AllPlatform_OptStress327RTTests.cpp
  AllPlatform_OptStress328RTTests.cpp
  AllPlatform_OptStress329RTTests.cpp
  AllPlatform_OptStress330RTTests.cpp
  AllPlatform_OptStress331RTTests.cpp
  AllPlatform_OptStress332RTTests.cpp
  AllPlatform_OptStress333RTTests.cpp
  AllPlatform_OptStress334RTTests.cpp
  AllPlatform_OptStress335RTTests.cpp
  AllPlatform_OptStress336RTTests.cpp
  AllPlatform_OptStress337RTTests.cpp
  AllPlatform_OptStress338RTTests.cpp
  AllPlatform_OptStress339RTTests.cpp
  AllPlatform_BigGlobalRTTests.cpp
  AllPlatform_MemBuiltinLiveRTTests.cpp
  RewriteCodegenRTTests.cpp
)

target_compile_definitions(NeverDSemanticTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDSemanticTests PRIVATE
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

# LibCNames' C++ helpers are intentionally not exported from neverd_shared on
# Windows.  SemanticRoundTripFixture calls them directly, so provide their
# static definitions to the test executable.
if(WIN32)
  target_link_libraries(NeverDSemanticTests PRIVATE NeverDLibC)
endif()

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
# per-test ctest timeout so slow codegen never flakes as a false Timeout
# (the Unicorn emulation itself is already bounded by a deterministic 200M
# instruction budget, so only codegen wall-clock is load-sensitive).
add_neverd_unittest(NeverDPatchFullTests
  PatchFullSubstRTTests.cpp
  TIMEOUT 1800
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

# --- Switch / jump-table transformation probing suite ---
# Standalone small TU (compiles in seconds, links against the prebuilt
# neverd_shared) so switch/indirect-branch transformation + backend bugs can be
# hunted incrementally without recompiling the 800-TU NeverDSemanticTests binary
# or the 22k-line PatchFull TU.  Differential lift->recompile roundtrip across
# the 4 ISAs; run just this binary via `ctest -L NeverDSwitchXformTests`.
add_neverd_unittest(NeverDSwitchXformTests
  AllPlatform_SwitchXformRTTests.cpp
  TIMEOUT 600
)

target_compile_definitions(NeverDSwitchXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDSwitchXformTests PRIVATE
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
  AllPlatform_IndCallXformRTTests.cpp
  TIMEOUT 600
)

target_compile_definitions(NeverDIndCallXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDIndCallXformTests PRIVATE
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
  AllPlatform_CFGLoopXformRTTests.cpp
  TIMEOUT 600
)

target_compile_definitions(NeverDCFGLoopXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDCFGLoopXformTests PRIVATE
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
  AllPlatform_TwoTableXformRTTests.cpp
  TIMEOUT 600
)

target_compile_definitions(NeverDTwoTableXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDTwoTableXformTests PRIVATE
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
  X64_AvxUpperXformRTTests.cpp
  TIMEOUT 600
)

target_compile_definitions(NeverDAvxUpperXformTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(NeverDAvxUpperXformTests PRIVATE
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
add_executable(PatchFullRepro EXCLUDE_FROM_ALL PatchFullRepro.cpp)
target_include_directories(PatchFullRepro PRIVATE
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
