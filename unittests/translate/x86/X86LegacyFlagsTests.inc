//===- X86LegacyFlagsTests.inc - Protected stack flags -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Privilege masks, independent stack widths, fault rollback and event edges.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, PoppedFlagsUseOriginalPrivilegeAndOperandSize) {
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (unsigned IOPL = 0; IOPL != 4; ++IOPL)
      for (bool Word : {false, true})
        for (uint32_t Before : {2u, 0x3d4ed7u})
          for (unsigned Bit = 0; Bit != 34; ++Bit) {
            SetUp();
            S.Core.CPL = CPL;
            for (auto &C : S.Core.Segments) {
              C.Selector = (C.Selector & ~3u) | CPL;
              C.Attributes = (C.Attributes & ~0x60u) | (CPL << 5);
            }
            S.EFlags = (Before & ~0x3000u) | (IOPL << 12);
            const uint32_t Popped = Bit == 32   ? 0
                                    : Bit == 33 ? UINT32_MAX
                                                : 1u << Bit;
            put(0xd000, Popped, GuestScalarWidth::I32);
            const auto Registers = S.Core.GPR;
            uint32_t Expected = S.EFlags & ~0x10000u;
            for (unsigned I = 0; I != 32; ++I) {
              const bool Status = I == 0 || I == 2 || I == 4 || I == 6 ||
                                  I == 7 || I == 8 || I == 10 || I == 11 ||
                                  I == 14;
              if (Status || (I == 9 && CPL <= IOPL) ||
                  ((I == 12 || I == 13) && CPL == 0) ||
                  ((I == 18 || I == 21) && !Word))
                Expected = (Expected & ~(1u << I)) | (Popped & (1u << I));
            }
            const std::vector<uint8_t> Bytes =
                Word ? std::vector<uint8_t>{0x66, 0x9d}
                     : std::vector<uint8_t>{0x9d};
            ASSERT_TRUE(run(Bytes).InstructionCompleted);
            EXPECT_EQ(S.EFlags, Expected);
            EXPECT_EQ(M->controls().RFlags, Expected);
            EXPECT_EQ(S.Core.GPR[4], 0xd000u + (Word ? 2 : 4));
            for (unsigned I = 0; I != Registers.size(); ++I)
              if (I != 4)
                EXPECT_EQ(S.Core.GPR[I], Registers[I]);
          }
}

TEST_F(X86LegacyExecutionTest, PushFlagsClearsOnlySnapshotRFAndKeepsHighESP) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u}) {
        SetUp();
        S.Core.Segments[1].Attributes ^= Code32 ? 0 : 0x4000;
        S.Core.Segments[2].Attributes ^= Stack32 ? 0 : 0x4000;
        S.Core.Segments[2].Base = 0x4000;
        S.Core.GPR[4] = Stack32 ? 0xd000 : 0xabcd0000;
        S.EFlags = 0x3d7ed7;
        const uint32_t Before = S.EFlags;
        const uint32_t Address =
            0x4000 + (Stack32 ? 0xd000 - Width : 0x10000 - Width);
        put(Address - 2, UINT64_MAX);
        std::vector<uint8_t> Bytes{0x67, 0x65};
        if (Code32 != (Width == 4))
          Bytes.push_back(0x66);
        Bytes.push_back(0x9c);
        ASSERT_TRUE(run(Bytes).InstructionCompleted);
        EXPECT_EQ(S.Core.GPR[4],
                  Stack32 ? 0xd000 - Width : 0xabcd0000u | (0x10000 - Width));
        EXPECT_EQ(get(Address, Width == 2 ? GuestScalarWidth::I16
                                          : GuestScalarWidth::I32),
                  (Before & 0xfcffff) & (Width == 2 ? UINT16_MAX : UINT32_MAX));
        EXPECT_EQ(get(Address - 2, GuestScalarWidth::I16), UINT16_MAX);
        EXPECT_EQ(get(Address + Width, GuestScalarWidth::I16), UINT16_MAX);
        EXPECT_EQ(S.EFlags, Before & ~0x10000u);
      }
}

TEST_F(X86LegacyExecutionTest, WordStackPopPreservesUpperESPAndFlags) {
  S.Core.Segments[2].Attributes &= ~0x4000u;
  S.Core.GPR[4] = 0xabcdfffe;
  S.EFlags = 0x3d3ed7;
  put(0xfffe, 0, GuestScalarWidth::I16);
  ASSERT_TRUE(run({0x67, 0x64, 0x66, 0x9d}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd0000u);
  EXPECT_EQ(S.EFlags, 0x3c3002u);
}

TEST_F(X86LegacyExecutionTest, PVIChangesNeitherSnapshotIFNorPoppedVIF) {
  auto Controls = M->controls();
  Controls.CR4 |= 2;
  llvm::cantFail(M->bindControls(Controls));
  S.EFlags = 0x100202; // VIP set, VIF clear, IOPL zero.
  ASSERT_TRUE(run({0x9c}).InstructionCompleted);
  EXPECT_EQ(get(0xcffc), 0x100202u);
  put(0xcffc, 0x80000, GuestScalarWidth::I32); // VIF cannot be set by POPFD.
  ASSERT_TRUE(run({0x9d}).InstructionCompleted);
  EXPECT_EQ(S.EFlags, 0x100202u); // IF cannot be cleared at CPL3/IOPL0.
}

TEST_F(X86LegacyExecutionTest, PopFlagsEnablesIRQWithoutCreatingSTIShadow) {
  S.EFlags = 0x3002;
  S.SSBlocked = S.STIBlocked = true;
  S.NMIBlocked = true;
  put(0xd000, 0x3202, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x9d}).InstructionCompleted);
  EXPECT_FALSE(S.SSBlocked);
  EXPECT_FALSE(S.STIBlocked);
  EXPECT_TRUE(S.NMIBlocked);
  auto R = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 64, X86LegacyEventKind::Interrupt}, O));
  EXPECT_EQ(R.DeliveredVector, 64);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1001u);
  EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x3202u);
}

TEST_F(X86LegacyExecutionTest, PopFlagsSetsTFWithoutAnImmediateTrap) {
  put(0xd000, 0x102, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x9d}).InstructionCompleted);
  EXPECT_EQ(S.EFlags, 0x302u); // Unprivileged IF remains set.
  auto R = executeX86LegacyInstruction(S, *M, {0x90}, O);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("debug-event producer"),
            std::string::npos);
  EXPECT_EQ(S.EIP, 0x1001u);
}

TEST_F(X86LegacyExecutionTest, FlagsStackPageFaultRestartsBeforePopOrPush) {
  for (uint8_t Opcode : {0x9c, 0x9d}) {
    SetUp();
    put(0x5804, 0xf000, GuestScalarWidth::I32);
    S.Core.GPR[4] = 0xcffe;
    if (Opcode == 0x9c)
      S.Core.GPR[4] += 4;
    put(0xcffe, 0x1357, GuestScalarWidth::I16);
    put(0xd000, 0x2468, GuestScalarWidth::I16);
    put(0x11034, 0xd006, GuestScalarWidth::I32);
    const uint32_t OldSP = S.Core.GPR[4];
    auto R = run({Opcode});
    ASSERT_EQ(R.DeliveredVector, 14);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(S.CR2, 0xd000u);
    EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
    EXPECT_EQ(get(S.Core.GPR[4] + 12), 0x10202u);
    EXPECT_EQ(get(S.Core.GPR[4] + 16), OldSP);
    EXPECT_EQ(get(0xcffe, GuestScalarWidth::I16), 0x1357u);
    EXPECT_EQ(get(0xd000, GuestScalarWidth::I16), 0x2468u);
    ASSERT_TRUE(run({0xc7, 0x05, 0x34, 0x10, 1, 0, 7, 0xd0, 0, 0})
                    .InstructionCompleted);
    ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
    ASSERT_TRUE(run({0xcf}).InstructionCompleted);
    R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {Opcode}, O));
    ASSERT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[4], Opcode == 0x9c ? OldSP - 4 : OldSP + 4);
  }
}

TEST_F(X86LegacyExecutionTest, FlagsAdmissionAndFetchPrecedeStackEffects) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0xf0, 0x9c}, {0xf3, 0x9d}, {0x66, 0x66, 0x9d}, {0x9c, 0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
  O.Scalars.reset();
  for (uint8_t Opcode : {0x9c, 0x9d}) {
    auto R = executeX86LegacyInstruction(S, *M, {Opcode}, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.Core.GPR[4], 0xd000u);
  }
  O.Scalars = X86LegacyScalarProfile::OrderedRAM;
  S.EIP = 0x1fff;
  put(0x11008, 0x2006, GuestScalarWidth::I32);
  auto R = run({0x66, 0x9c});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0x2000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 16), 0xd000u);
}
