//===- X86LegacyFarTests.inc - Protected far-transfer contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Call-gate round trips, fault restart and explicit admission boundaries.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, FarAdmissionAndMalformedFormsPrecedeFetch) {
  const std::vector<uint8_t> Valid{0x9a, 0, 0x40, 0, 0, 27, 0};
  for (bool Missing : {false, true}) {
    O.FarTransfers =
        Missing ? std::nullopt
                : std::optional(static_cast<X86LegacyFarTransferProfile>(255));
    auto R = executeX86LegacyInstruction(S, *M, Valid, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x10000), 0x11007u);
  }
  O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0xff, 0xd8},
                                         {0xff, 0xe8},
                                         {0xea, 0, 0x40},
                                         {0x9a, 0, 0x40, 0, 0, 27, 0, 0},
                                         {0x66, 0x66, 0xea, 0, 0x40, 27, 0},
                                         {0xf2, 0xea, 0, 0x40, 0, 0, 27, 0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  EXPECT_EQ(get(0x10000), 0x11007u);
  EXPECT_EQ(llvm::cantFail(executeX86LegacyInstruction(S, *M, Valid, O)).Status,
            X86LegacyExecutionStatus::CodeChanged);
  EXPECT_EQ(run({0xf0, 0xff, 0x18}).DeliveredVector, 6);
}

TEST_F(X86LegacyExecutionTest, FarGateCopiesArgumentsAndReturnsToOuterStack) {
  O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
  for (bool Gate32 : {false, true})
    for (bool Operand32 : {false, true})
      for (unsigned Parameters : {0u, 1u, 31u}) {
        SetUp();
        const unsigned Width = Gate32 ? 4 : 2;
        const auto W = Gate32 ? GuestScalarWidth::I32 : GuestScalarWidth::I16;
        put(0x7030, 0x4000 | uint64_t{8} << 16 | uint64_t(Parameters) << 32 |
                        uint64_t(Gate32 ? 0xec : 0xe4) << 40);
        for (unsigned I = 0; I != Parameters; ++I)
          put(0xd000 + I * Width, 0x87650000 | I * 0x123, W);
        std::vector<uint8_t> Call;
        if (!Operand32)
          Call.push_back(0x66);
        Call.insert(Call.end(), {0x9a, 0xef, 0xbe});
        if (Operand32)
          Call.insert(Call.end(), {0xad, 0xde});
        Call.insert(Call.end(), {51, 0});
        S.SSBlocked = S.STIBlocked = S.NMIBlocked = true;
        const auto R = run(Call);
        ASSERT_TRUE(R.InstructionCompleted);
        EXPECT_EQ(S.Core.CPL, 0);
        EXPECT_EQ(S.Core.Segments[1].Selector, 8);
        EXPECT_EQ(S.Core.Segments[2].Selector, 16);
        EXPECT_EQ(S.EIP, 0x4000u);
        EXPECT_EQ(S.EFlags, 0x202u);
        EXPECT_FALSE(S.SSBlocked || S.STIBlocked);
        EXPECT_TRUE(S.NMIBlocked);
        const uint32_t SP = 0xe000 - (Parameters + 4) * Width;
        EXPECT_EQ(S.Core.GPR[4], SP);
        EXPECT_EQ(get(SP, W), 0x1000 + Call.size());
        EXPECT_EQ(get(SP + Width, W), 27u);
        for (unsigned I = 0; I != Parameters; ++I)
          EXPECT_EQ(get(SP + (I + 2) * Width, W), get(0xd000 + I * Width, W));
        EXPECT_EQ(get(SP + (Parameters + 2) * Width, W), 0xd000u);
        EXPECT_EQ(get(SP + (Parameters + 3) * Width, W), 35u);
        std::vector<uint8_t> Ret;
        if (!Gate32)
          Ret.push_back(0x66);
        Ret.insert(Ret.end(), {0xca, uint8_t(Parameters * Width), 0});
        ASSERT_TRUE(run(Ret).InstructionCompleted);
        EXPECT_EQ(S.Core.CPL, 3);
        EXPECT_EQ(S.Core.GPR[4], 0xd000 + Parameters * Width);
        EXPECT_EQ(S.EIP, 0x1000 + Call.size());
        EXPECT_EQ(S.Core.Segments[1].Selector, 27);
        EXPECT_EQ(S.Core.Segments[2].Selector, 35);
        EXPECT_TRUE(S.NMIBlocked);
      }
}

TEST_F(X86LegacyExecutionTest,
       FarTaskTransferRefusesWithoutPublishingRegisters) {
  O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
  for (unsigned Type : {1u, 3u, 5u, 9u, 11u}) {
    SetUp();
    put(0x7030, uint64_t(0xe0 | Type) << 40);
    const std::vector<uint8_t> Bytes{0xea, 0, 0, 0, 0, 51, 0};
    for (unsigned I = 0; I != Bytes.size(); ++I)
      put(0x1000 + I, Bytes[I], GuestScalarWidth::I8);
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("task-switch"),
              std::string::npos);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.Core.GPR[4], 0xd000u);
    EXPECT_EQ(S.Core.Segments[1].Selector, 27);
  }
}

TEST_F(X86LegacyExecutionTest,
       FarPointerPageFaultRepairsAndRetriesThroughIret) {
  O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
  S.Core.GPR[3] = 0xeffc;
  put(0xeffc, 0x4000, GuestScalarWidth::I32);
  put(0xf000, 27, GuestScalarWidth::I16);
  put(0x1103c, 0xf006, GuestScalarWidth::I32);
  auto R = run({0xff, 0x1b});
  ASSERT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0xf000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
  EXPECT_EQ(get(0xcffc), 0u);
  put(0x1103c, 0xf007, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
  ASSERT_TRUE(run({0xcf}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[4], 0xd000u);
  ASSERT_TRUE(run({0xff, 0x1b}).InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x4000u);
  EXPECT_EQ(S.Core.GPR[4], 0xcff8u);
  EXPECT_EQ(get(0xcff8), 0x1002u);
}

TEST_F(X86LegacyExecutionTest, FarFaultDeliveryBackingStopRetainsPartialFrame) {
  O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
  S.Core.GPR[4] = 0xe004;
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  put(0x11018, 0x30007, GuestScalarWidth::I32);
  auto R = run({0x9a, 0, 0x40, 0, 0, 27, 0});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingEvent);
  EXPECT_EQ(S.PendingEvent->Event.Vector, 14);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(S.Core.GPR[4], 0xe004u);
  EXPECT_EQ(get(0xe000), 27u);
  put(0x11018, 0x6007, GuestScalarWidth::I32);
  put(0x11034, 0xd007, GuestScalarWidth::I32);
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_FALSE(S.PendingEvent);
  EXPECT_EQ(get(0xe000), 27u);
  ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
  ASSERT_TRUE(run({0xcf}).InstructionCompleted);
  ASSERT_TRUE(run({0x9a, 0, 0x40, 0, 0, 27, 0}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[4], 0xdffcu);
  EXPECT_EQ(get(0xdffc), 0x1007u);
}

TEST_F(X86LegacyExecutionTest, FarParameterFaultRetainsOuterFrameBeforeRetry) {
  O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
  for (bool Wide : {false, true}) {
    SetUp();
    const unsigned Width = Wide ? 4 : 2;
    const auto W = Wide ? GuestScalarWidth::I32 : GuestScalarWidth::I16;
    put(0x7038, 0x4000 | uint64_t{48} << 16 | uint64_t{3} << 32 |
                    uint64_t(Wide ? 0xec : 0xe4) << 40);
    put(0x7030, 0x00cfba000000ffffULL);
    put(0x7040, 0x00cfb2000000ffffULL);
    put(0x580c, 0xa800, GuestScalarWidth::I32);
    put(0x5810, 65, GuestScalarWidth::I16);
    S.Core.GPR[4] = 0xc000 - Width;
    for (unsigned I = 0; I != 3; ++I)
      put(S.Core.GPR[4] + I * Width, 0xaabb0000 | (I + 1) * 0x123, W);
    put(0x11030, 0xc006, GuestScalarWidth::I32);
    auto R = run({0x9a, 0, 0, 0, 0, 59, 0});
    ASSERT_EQ(R.DeliveredVector, 14);
    EXPECT_EQ(S.CR2, 0xc000 + Width);
    EXPECT_EQ(get(0xa800 - Width, W), 35u);
    EXPECT_EQ(get(0xa800 - 2 * Width, W), 0xc000 - Width);
    EXPECT_EQ(get(0xa800 - 3 * Width, W), 0u);
    EXPECT_EQ(get(S.Core.GPR[4]), 4u);
    put(0x11030, 0xc007, GuestScalarWidth::I32);
    ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
    ASSERT_TRUE(run({0xcf}).InstructionCompleted);
    ASSERT_TRUE(run({0x9a, 0, 0, 0, 0, 59, 0}).InstructionCompleted);
    EXPECT_EQ(S.Core.CPL, 1);
    for (unsigned I = 0; I != 3; ++I)
      EXPECT_EQ(get(0xa800 - (5 - I) * Width, W),
                (0xaabb0000 | (I + 1) * 0x123) &
                    (Wide ? UINT32_MAX : UINT16_MAX));
  }
}
