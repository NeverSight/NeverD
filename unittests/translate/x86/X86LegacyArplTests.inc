//===- X86LegacyArplTests.inc - Protected selector adjustment ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Word-only RPL updates, conditional write checks and original fault retry.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, ArplPreservesRegisterBitsAndOtherFlags) {
  for (unsigned Destination = 0; Destination != 8; ++Destination)
    for (unsigned Source = 0; Source != 8; ++Source)
      for (unsigned DestRPL = 0; DestRPL != 4; ++DestRPL)
        for (unsigned SourceRPL = 0; SourceRPL != 4; ++SourceRPL)
          for (uint32_t Flags : {0x10002u, 0x10ed7u}) {
            SetUp();
            for (unsigned I = 0; I != 8; ++I)
              S.Core.GPR[I] = 0xabcd0000u | (I << 4) | DestRPL;
            S.Core.GPR[Source] = 0xfedc8000u | (Source << 4) | SourceRPL;
            S.EFlags = Flags;
            auto Expected = S.Core.GPR;
            const bool Adjust =
                (Expected[Destination] & 3) < (Expected[Source] & 3);
            if (Adjust)
              Expected[Destination] =
                  (Expected[Destination] & ~3u) | (Expected[Source] & 3);
            ASSERT_TRUE(run({0x66, 0x67, 0x64, 0x63,
                             uint8_t(0xc0 | Source << 3 | Destination)})
                            .InstructionCompleted);
            EXPECT_EQ(S.Core.GPR, Expected);
            EXPECT_EQ(S.EFlags, (Flags & ~0x10040u) | (Adjust ? 64 : 0));
          }
}

TEST_F(X86LegacyExecutionTest, ArplChecksWritePermissionOnlyWhenRPLChanges) {
  for (bool SegmentReadOnly : {false, true})
    for (bool PageReadOnly : {false, true})
      for (bool Adjust : {false, true}) {
        SetUp();
        S.Core.GPR[3] = 0xc100;
        S.Core.GPR[1] = 0xfedc0003;
        S.EFlags = Adjust ? 0x10e97 : 0x10ed7;
        if (SegmentReadOnly)
          S.Core.Segments[3].Attributes &= ~2u;
        put(0x11030, PageReadOnly ? 0xc005 : 0xc007, GuestScalarWidth::I32);
        const uint32_t Before = 0x55aaabc0u | (Adjust ? 1 : 3);
        put(0xc100, Before, GuestScalarWidth::I32);
        const auto R = run({0x66, 0x63, 0x0b});
        if (Adjust && (SegmentReadOnly || PageReadOnly)) {
          EXPECT_EQ(R.DeliveredVector, SegmentReadOnly ? 13 : 14);
          EXPECT_FALSE(R.InstructionCompleted);
          EXPECT_EQ(get(S.Core.GPR[4]), SegmentReadOnly ? 0u : 7u);
          EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
          EXPECT_EQ(get(S.Core.GPR[4] + 12), 0x10e97u);
          EXPECT_EQ(get(0xc100), Before);
        } else {
          ASSERT_TRUE(R.InstructionCompleted);
          EXPECT_EQ(get(0xc100), 0x55aaabc3u);
          EXPECT_EQ(S.EFlags, 0xe97u | (Adjust ? 64 : 0));
          EXPECT_EQ(get(0x11030) & 0x60, Adjust ? 0x60u : 0x20u);
        }
        EXPECT_EQ(S.Core.GPR[1], 0xfedc0003u);
      }
}

TEST_F(X86LegacyExecutionTest, ArplReadsBeforeCheckingAConditionalStore) {
  S.Core.GPR[3] = 0xc100;
  S.Core.GPR[1] = 3;
  S.Core.Segments[3].Attributes &= ~2u;
  put(0xc100, 0xabc0, GuestScalarWidth::I16);
  put(0x11030, 0xc006, GuestScalarWidth::I32);
  auto R = run({0x63, 0x0b});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0xc100u);
  EXPECT_EQ(get(S.Core.GPR[4]), 4u); // Original read #PF, not write #GP.
  EXPECT_EQ(get(S.Core.GPR[4] + 12), 0x10202u);
  EXPECT_EQ(get(0xc100, GuestScalarWidth::I16), 0xabc0u);
}

TEST_F(X86LegacyExecutionTest, ArplUsesOnlyItsWordDespiteOperandSize) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (bool Override : {false, true}) {
        SetUp();
        if (!Code32)
          S.Core.Segments[1].Attributes &= ~0x4000u;
        S.Core.GPR[3] = 0xcffe;
        S.Core.GPR[1] = 0xffff0003;
        put(0xcffc, 0xabc0cafe, GuestScalarWidth::I32);
        put(0xd000, 0xdead, GuestScalarWidth::I16);
        put(0x11034, 0xd006, GuestScalarWidth::I32);
        std::vector<uint8_t> Bytes;
        if (Override)
          Bytes.push_back(0x66);
        if (Address32 != Code32)
          Bytes.push_back(0x67);
        Bytes.insert(Bytes.end(), {0x63, uint8_t(Address32 ? 0x0b : 0x0f)});
        ASSERT_TRUE(run(Bytes).InstructionCompleted);
        EXPECT_EQ(get(0xcffc), 0xabc3cafeu);
        EXPECT_EQ(get(0xd000, GuestScalarWidth::I16), 0xdeadu);
        EXPECT_EQ(get(0x11034) & 0x60, 0u);
      }
}

TEST_F(X86LegacyExecutionTest, ArplRestartsAfterOriginalPageRepair) {
  for (bool Adjust : {false, true}) {
    SetUp();
    S.Core.GPR[3] = 0xc100;
    S.Core.GPR[1] = 3;
    put(0xc100, Adjust ? 0xabc0 : 0xabc3, GuestScalarWidth::I16);
    put(0x11030, 0xc006, GuestScalarWidth::I32);
    ASSERT_EQ(run({0x63, 0x0b}).DeliveredVector, 14);
    EXPECT_EQ(get(S.Core.GPR[4]), 4u);
    ASSERT_TRUE(run({0xc7, 0x05, 0x30, 0x10, 1, 0, 7, 0xc0, 0, 0})
                    .InstructionCompleted);
    ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
    ASSERT_TRUE(run({0xcf}).InstructionCompleted);
    ASSERT_EQ(S.EIP, 0x1000u);
    ASSERT_TRUE(run({0x63, 0x0b}).InstructionCompleted);
    EXPECT_EQ(get(0xc100, GuestScalarWidth::I16), 0xabc3u);
    EXPECT_EQ(S.EFlags, 0x202u | (Adjust ? 64 : 0));
  }
}

TEST_F(X86LegacyExecutionTest, ArplAdmissionPrecedesInstructionFetch) {
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x63},
                                         {0x63, 0xc0, 0},
                                         {0xf0, 0x63, 0xc1},
                                         {0xf3, 0x63, 0xc1},
                                         {0x66, 0x66, 0x63, 0xc1}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
  O.Scalars.reset();
  auto R = executeX86LegacyInstruction(S, *M, {0x63, 0xc0}, O);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(0x11004) & 0x20, 0u);
}
