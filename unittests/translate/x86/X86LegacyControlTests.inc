//===- X86LegacyControlTests.inc - Protected control transitions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original widths, reserved bits, transactional PDPTE loads and next fetch.
//===----------------------------------------------------------------------===//

class X86LegacyControlTest : public X86LegacyExecutionTest {
protected:
  void reset(bool PAE = false, bool Paging = true) {
    resetLegacyState({.PhysicalAddressBits = 32,
                      .PSE = true,
                      .PAE = true,
                      .LongMode = true,
                      .SMEP = true,
                      .SMAP = true});
    S.Core.CPL = 0;
    S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
    S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
    S.EFlags = 0xed7;
    O.ControlRegisters = X86LegacyControlRegisterBinding{0x88};
    auto C = M->controls();
    C.CR0 = Paging ? 0x80010011 : 0x10011;
    C.CR4 = PAE ? 0x20 : 0;
    C.PDPTE = {0x11001, 0, 0, 0};
    if (PAE) {
      put(0x10000, 0x11001);
      put(0x11000, 0x12007);
      for (unsigned I = 0; I != 32; ++I)
        put(0x12000 + I * 8, I * 4096 | 7);
    }
    llvm::cantFail(M->bindControls(C));
  }

  void write(unsigned CR, uint32_t Value) {
    S.Core.GPR[6] = Value;
    ASSERT_TRUE(run(test::legacyControlBytes(CR, true)).InstructionCompleted);
  }
};

TEST_F(X86LegacyControlTest, FullWidthReadsIgnoreModAndLegacyOverrides) {
  for (bool Code32 : {false, true})
    for (unsigned CR : {0u, 2u, 3u, 4u})
      for (unsigned GPR = 0; GPR != 8; ++GPR)
        for (unsigned Mod = 0; Mod != 4; ++Mod)
          for (bool Overrides : {false, true}) {
            reset(true);
            S.Core.Segments[1].Attributes = Code32 ? 0xc09b : 0x809b;
            S.Core.GPR[GPR] = 0xdeadbeef;
            const uint32_t Value = CR == 0   ? 0x80010011
                                   : CR == 2 ? 0x12345678
                                   : CR == 3 ? 0x10000
                                             : 0x20;
            const auto Bytes =
                test::legacyControlBytes(CR, false, GPR, Mod, Overrides);
            ASSERT_TRUE(run(Bytes).InstructionCompleted);
            EXPECT_EQ(S.Core.GPR[GPR], Value);
            EXPECT_EQ(S.EIP, 0x1000 + Bytes.size());
            EXPECT_EQ(S.EFlags, 0xed7u);
          }
}

TEST_F(X86LegacyControlTest, WritesPublishOnlyTheirBankAndNormalizeCR0) {
  for (unsigned CR : {0u, 2u, 3u, 4u})
    for (unsigned GPR = 0; GPR != 8; ++GPR)
      for (unsigned Mod = 0; Mod != 4; ++Mod) {
        reset();
        const uint32_t Value = CR == 0   ? 0xdffaffff
                               : CR == 2 ? 0xdeadbeef
                               : CR == 3 ? 0x10fff
                                         : 0x88;
        S.Core.GPR[GPR] = Value;
        ASSERT_TRUE(run(test::legacyControlBytes(CR, true, GPR, Mod, true))
                        .InstructionCompleted);
        EXPECT_EQ(S.Core.GPR[GPR], Value);
        EXPECT_EQ(M->controls().CR0, CR == 0 ? 0xc000003fu : 0x80010011u);
        EXPECT_EQ(S.CR2, CR == 2 ? 0xdeadbeefu : 0x12345678u);
        EXPECT_EQ(M->controls().CR3, CR == 3 ? 0x10fffu : 0x10000u);
        EXPECT_EQ(M->controls().CR4, CR == 4 ? 0x88u : 0u);
        EXPECT_EQ(S.EFlags, 0xed7u);
      }
}

TEST_F(X86LegacyControlTest, ArchitecturalFaultsRetainControlsAndOriginalPC) {
  for (bool Write : {false, true})
    for (unsigned CR = 0; CR != 8; ++CR)
      for (unsigned CPL : {0u, 3u})
        for (bool Locked : {false, true}) {
          reset();
          if (CPL) {
            S.Core.CPL = 3;
            S.Core.Segments[1] = {0, UINT32_MAX, 27, 0xc0fb, false};
            S.Core.Segments[2] = {0, UINT32_MAX, 35, 0xc0f3, false};
          }
          const bool BadCR = CR == 1 || CR >= 5;
          if (!Locked && !BadCR && !CPL)
            continue;
          S.Core.GPR[6] = 0xffffffff;
          const auto R = run(test::legacyControlBytes(CR, Write, 6, 0, true,
                                                      Locked ? 0xf0 : 0));
          ASSERT_EQ(R.DeliveredVector, Locked || BadCR ? 6 : 13);
          EXPECT_FALSE(R.InstructionCompleted);
          EXPECT_EQ(M->controls().CR0, 0x80010011u);
          EXPECT_EQ(M->controls().CR3, 0x10000u);
          EXPECT_EQ(M->controls().CR4, 0u);
          EXPECT_EQ(S.CR2, 0x12345678u);
          EXPECT_EQ(get(S.Core.GPR[4] + (R.DeliveredVector == 13 ? 4 : 0)),
                    0x1000u);
        }
}

TEST_F(X86LegacyControlTest, InvalidControlCombinationsDeliverGP) {
  for (auto [CR, Value] : {std::pair{0u, 0x80000010u},
                           {0u, 0xa0010011u},
                           {4u, 0x80000000u},
                           {4u, 1u << 17},
                           {4u, 1u << 9}}) {
    reset();
    S.Core.GPR[6] = Value;
    const auto R = run(test::legacyControlBytes(CR, true));
    EXPECT_EQ(R.DeliveredVector, 13);
    EXPECT_EQ(M->controls().CR0, 0x80010011u);
    EXPECT_EQ(M->controls().CR4, 0u);
  }
  reset();
  O.ControlRegisters->OtherCR4Features |= 1u << 23;
  auto C = M->controls();
  C.CR0 &= ~(1u << 16);
  llvm::cantFail(M->bindControls(C));
  S.Core.GPR[6] = 1u << 23;
  EXPECT_EQ(run(test::legacyControlBytes(4, true)).DeliveredVector, 13);
  EXPECT_EQ(M->controls().CR4, 0u);
}

TEST_F(X86LegacyControlTest,
       InvalidLongModeActivationFaultsBeforeModeBoundary) {
  for (bool PAE : {false, true}) {
    reset(PAE, false);
    auto C = M->controls();
    C.EFER = 0x100;
    llvm::cantFail(M->bindControls(C));
    S.TR.Attributes = 0x83;
    S.Core.GPR[6] = 0x80010011;
    EXPECT_EQ(run(test::legacyControlBytes(0, true)).DeliveredVector, 13);
    EXPECT_EQ(M->controls().CR0, 0x10011u);
    EXPECT_EQ(M->controls().EFER, 0x100u);
  }
}

TEST_F(X86LegacyControlTest,
       PAEChangedControlsReloadButUnchangedControlsDoNot) {
  for (auto [CR, Value] : {std::pair{0u, 0xc0010011u},
                           {0u, 0xe0010011u},
                           {4u, 0x30u},
                           {4u, 0xa0u},
                           {4u, 0x100020u},
                           {3u, 0x10000u}}) {
    reset(true);
    put(0x10008,
        0xdeadbeefdeadbeeeULL); // Nonpresent reserved bits are ignored.
    ASSERT_NO_FATAL_FAILURE(write(CR, Value));
    EXPECT_EQ(M->controls().PDPTE[1], 0xdeadbeefdeadbeeeULL);
  }
  for (auto [CR, Value] : {std::pair{0u, 0x80010011u},
                           {0u, 0x80010019u},
                           {4u, 0x20u},
                           {4u, 0x28u},
                           {4u, 0x200020u},
                           {2u, 0x12345678u}}) {
    reset(true);
    put(0x10000, 3); // Would fault if an unrelated/unchanged control reloaded.
    ASSERT_NO_FATAL_FAILURE(write(CR, Value));
    EXPECT_EQ(M->controls().PDPTE[0], 0x11001u);
  }
}

TEST_F(X86LegacyControlTest, PAEEnableLoadsAllFourEntriesAtomically) {
  for (unsigned CR : {0u, 4u})
    for (unsigned Entry = 0; Entry != 4; ++Entry) {
      reset(true, false);
      auto C = M->controls();
      C.PDPTE = {0, 0, 0, 0};
      C.CR4 = CR == 4 ? 0 : 0x20;
      C.CR0 = CR == 4 ? 0x80010011 : 0x10011;
      // CR3's bits 11:5 select a different PDPT but are ignored by legacy
      // paging. Keep both translations valid before testing a late entry.
      if (CR == 4) {
        C.CR3 = 0x10020;
        put(0x10000, 0x11007, GuestScalarWidth::I32);
        for (unsigned I = 0; I != 32; ++I)
          put(0x11000 + I * 4, I * 4096 | 7, GuestScalarWidth::I32);
        put(0x10020, 0x14001);
        put(0x14000, 0x15007);
        for (unsigned I = 0; I != 32; ++I)
          put(0x15000 + I * 8, I * 4096 | 7);
      }
      llvm::cantFail(M->bindControls(C));
      const auto Prior = get(C.CR3 + Entry * 8, GuestScalarWidth::I64);
      put(C.CR3 + Entry * 8, 3);
      S.Core.GPR[6] = CR == 0 ? 0x80010011 : 0x20;
      const auto R = run(test::legacyControlBytes(CR, true));
      ASSERT_EQ(R.DeliveredVector, 13);
      EXPECT_EQ(M->controls().CR0, C.CR0);
      EXPECT_EQ(M->controls().CR4, C.CR4);
      EXPECT_EQ(M->controls().PDPTE, C.PDPTE);
      put(C.CR3 + Entry * 8, Prior);
      ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
      ASSERT_TRUE(run({0xcf}).InstructionCompleted);
      ASSERT_TRUE(run(test::legacyControlBytes(CR, true)).InstructionCompleted);
      EXPECT_EQ(M->controls().CR0, 0x80010011u);
      EXPECT_EQ(M->controls().CR4, 0x20u);
      EXPECT_EQ(M->controls().PDPTE[0], CR == 4 ? 0x14001u : 0x11001u);
    }
}

TEST_F(X86LegacyControlTest, PDPTEFaultRepairRetryAndBackingStop) {
  reset(true);
  put(0x13000, 0x11001);
  put(0x13018, 3);
  S.Core.GPR[6] = 0x13000;
  ASSERT_EQ(run(test::legacyControlBytes(3, true)).DeliveredVector, 13);
  EXPECT_EQ(M->controls().CR3, 0x10000u);
  put(0x13018, 0);
  ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
  ASSERT_TRUE(run({0xcf}).InstructionCompleted);
  ASSERT_TRUE(run(test::legacyControlBytes(3, true)).InstructionCompleted);
  EXPECT_EQ(M->controls().CR3, 0x13000u);
  S.Core.GPR[6] = 0x20000;
  const auto PC = S.EIP;
  const auto Bank = M->controls().PDPTE;
  auto R = run(test::legacyControlBytes(3, true));
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, PC);
  EXPECT_EQ(M->controls().CR3, 0x13000u);
  EXPECT_EQ(M->controls().PDPTE, Bank);
}

TEST_F(X86LegacyControlTest, CR3CommitChangesNextInstructionFetch) {
  for (bool PAE : {false, true}) {
    reset(PAE);
    if (PAE) {
      put(0x13000, 0x14001);
      put(0x14000, 0x15007);
      put(0x15008, 0x9007);
    } else {
      put(0x13000, 0x14007, GuestScalarWidth::I32);
      put(0x14004, 0x9007, GuestScalarWidth::I32);
    }
    put(0x9003, 0x12345678b8ULL);
    ASSERT_NO_FATAL_FAILURE(write(3, 0x13000));
    const std::vector<uint8_t> Bytes{0xb8, 0x78, 0x56, 0x34, 0x12};
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_TRUE(R->InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  }
}

TEST_F(X86LegacyControlTest, ControlsCommitBeforeDebugTrapOrDeliveryStop) {
  for (unsigned CR : {0u, 2u, 3u, 4u}) {
    reset();
    S.EFlags = 0x302;
    S.Debug.emplace();
    O.Debug = X86LegacyDebugProfile::InstructionStep;
    const uint32_t Value = CR == 0   ? 0x80000011
                           : CR == 2 ? 0xabcdef01
                           : CR == 3 ? 0x10018
                                     : 0x88;
    S.Core.GPR[6] = Value;
    const auto R = run(test::legacyControlBytes(CR, true));
    ASSERT_EQ(R.DeliveredVector, 1);
    EXPECT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x1003u);
    EXPECT_EQ(M->controls().CR0, CR == 0 ? Value : 0x80010011u);
    EXPECT_EQ(S.CR2, CR == 2 ? Value : 0x12345678u);
    EXPECT_EQ(M->controls().CR3, CR == 3 ? Value : 0x10000u);
    EXPECT_EQ(M->controls().CR4, CR == 4 ? Value : 0u);
  }
}

TEST_F(X86LegacyControlTest,
       MissingBindingAndMalformedGrammarRefuseBeforeFetch) {
  reset();
  O.ControlRegisters.reset();
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x0f, 0x20, 0xc0},
        {0x0f, 0x20},
        {0x0f, 0x20, 0x04, 0},
        {0x0f, 0x22, 0xc0, 0x90},
        {0x66, 0x66, 0x0f, 0x20, 0xc0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x11004), 0x1007u);
  }
}

TEST_F(X86LegacyControlTest, PagingCapabilitiesHaveOneOwner) {
  for (uint32_t Invalid : {0x10u, 0x20u, 0x1000u, 0x100000u, 0x200000u,
                           0x400000u, 0x1000000u, 0x80000000u}) {
    reset();
    O.ControlRegisters->OtherCR4Features = Invalid;
    auto R = executeX86LegacyInstruction(S, *M, {0x0f, 0x20, 0xc0}, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x11004), 0x1007u);
  }
  resetLegacyState({.PhysicalAddressBits = 32});
  auto C = M->controls();
  C.CR0 |= 0x10;
  llvm::cantFail(M->bindControls(C));
  S.Core.CPL = 0;
  S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
  S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
  S.EFlags = 2;
  O.ControlRegisters = X86LegacyControlRegisterBinding{};
  S.Core.GPR[6] = 0x20;
  EXPECT_EQ(run(test::legacyControlBytes(4, true)).DeliveredVector, 13);
  EXPECT_EQ(M->controls().CR4, 0u);
}

TEST_F(X86LegacyControlTest, UnboundModesAndMemoryDomainsRefuseWithoutCommit) {
  for (auto [CR, Value] : {std::pair{0u, 0x10u},
                           {0u, 0x80010011u},
                           {4u, 1u << 23},
                           {4u, 1u << 27},
                           {4u, 1u << 28}}) {
    reset(true, false);
    O.ControlRegisters->OtherCR4Features = (1u << 23) | (1u << 27) | (1u << 28);
    auto C = M->controls();
    if (CR == 0 && (Value & 0x80000000))
      C.EFER = 0x100;
    llvm::cantFail(M->bindControls(C));
    S.Core.GPR[6] = Value;
    const auto Bytes = test::legacyControlBytes(CR, true);
    for (unsigned I = 0; I != Bytes.size(); ++I)
      put(S.EIP + I, Bytes[I], GuestScalarWidth::I8);
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(M->controls().CR0, C.CR0);
    EXPECT_EQ(M->controls().CR4, C.CR4);
  }
}
