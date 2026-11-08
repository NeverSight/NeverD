//===- X86LegacySystemSelectorTests.inc - Protected LDTR/TR contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Fixed expectations for cache publication, busy writes and restart state.
//===----------------------------------------------------------------------===//

class X86LegacySystemSelectorTest : public X86LegacyExecutionTest {
protected:
  void SetUp() override {
    X86LegacyExecutionTest::SetUp();
    O.SystemSelectors = X86LegacySystemSelectorProfile::ExclusiveTable;
  }

  void kernel() {
    S.Core.CPL = 0;
    S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
    S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
    S.EFlags = 0xed7;
  }
};

TEST_F(X86LegacySystemSelectorTest, StoresRespectWidthAndNeverReadTables) {
  for (unsigned Form : {0u, 1u})
    for (bool Operand32 : {false, true})
      for (int Register : {-1, 0, 1, 2, 3, 4, 5, 6, 7}) {
        SetUp();
        S.EFlags = 0xed7;
        S.Core.LDTR = {0x8000, 7, 0xabcf, 0x82, true};
        S.TR.Selector = 0x123b;
        S.Core.GPR[3] = 0x9ffe;
        put(0x9ffc, 0xdeadbeef, GuestScalarWidth::I32);
        put(0x11028, 0, GuestScalarWidth::I32);
        if (Register >= 0)
          S.Core.GPR[Register] = 0x89abcdef;
        put(0x1101c, 0, GuestScalarWidth::I32); // GDT is inaccessible.
        const auto R = run(test::legacySystemSelectorBytes(
            Form, true, Operand32, true, Register));
        ASSERT_TRUE(R.InstructionCompleted);
        const uint32_t Selector = Form == 0 ? 0xabcf : 0x123b;
        if (Register < 0)
          EXPECT_EQ(get(0x9ffc), (Selector << 16) | 0xbeef);
        else
          EXPECT_EQ(S.Core.GPR[Register],
                    Operand32 ? Selector : 0x89ab0000 | Selector);
        EXPECT_EQ(S.EFlags, 0xed7u);
      }
}

TEST_F(X86LegacySystemSelectorTest, LoadsPreserveFlagsAndUnrelatedCaches) {
  for (unsigned Form : {2u, 3u})
    for (unsigned Type : {1u, 2u, 9u})
      for (unsigned RPL = 0; RPL != 4; ++RPL) {
        SetUp();
        kernel();
        S.Core.GPR[6] = 0xabcd0038 | RPL;
        const uint64_t D =
            0x0090004567891234ULL | (uint64_t(Type | 0xe0) << 40);
        put(0x7038, D);
        const auto OldTR = S.TR;
        const auto OldLDT = S.Core.LDTR;
        const auto R = run(test::legacySystemSelectorBytes(Form));
        const bool Valid = Form == 2 ? Type == 2 : Type == 1 || Type == 9;
        ASSERT_EQ(R.InstructionCompleted, Valid);
        if (!Valid) {
          EXPECT_EQ(R.DeliveredVector, 13);
          EXPECT_EQ(get(S.Core.GPR[4]), 0x38u);
          EXPECT_EQ(S.TR.Selector, OldTR.Selector);
          EXPECT_EQ(S.Core.LDTR.Selector, OldLDT.Selector);
        } else {
          const auto &C = Form == 2 ? S.Core.LDTR : S.TR;
          EXPECT_EQ(C.Selector, 0x38 | RPL);
          EXPECT_EQ(C.Base, 0x456789u);
          EXPECT_EQ(C.Limit, 0x1234fffu);
          EXPECT_EQ(C.Attributes, 0x9000 | Type | 0xe0 | (Form == 3 ? 2 : 0));
          EXPECT_EQ(S.EFlags, 0xed7u);
          EXPECT_EQ(S.Core.GPR[6], 0xabcd0038 | RPL);
        }
        EXPECT_EQ(get(0x7038, GuestScalarWidth::I64),
                  D | (Valid && Form == 3 ? uint64_t{1} << 41 : 0));
      }
}

TEST_F(X86LegacySystemSelectorTest, NullLocalBoundsAndPresenceFaults) {
  for (unsigned Form : {2u, 3u})
    for (unsigned Case = 0; Case != 7; ++Case) {
      SetUp();
      kernel();
      S.Core.LDTR = {0x8000, 0x1234, 48, 0x82, false};
      S.Core.GPR[6] = Case < 4 ? Case : Case == 4 ? 0x103f : 0x103b;
      S.Core.GDTRLimit = Case == 5 ? 0x103e : 0x103f;
      put(0x8038, Form == 2 ? 0x0000020058000067ULL : 0x0000090058000067ULL);
      const auto R = run(test::legacySystemSelectorBytes(Form));
      if (Form == 2 && Case < 4) {
        ASSERT_TRUE(R.InstructionCompleted);
        EXPECT_EQ(S.Core.LDTR.Selector, Case);
        EXPECT_TRUE(S.Core.LDTR.Unusable);
        EXPECT_EQ(S.Core.LDTR.Base, 0x8000u); // Hidden fields remain opaque.
      } else {
        ASSERT_EQ(R.DeliveredVector, Case == 6 ? 11 : 13);
        EXPECT_EQ(get(S.Core.GPR[4]), Case < 4    ? 0u
                                      : Case == 4 ? 0x103cu
                                                  : 0x1038u);
        EXPECT_EQ(S.Core.LDTR.Selector, 48);
        EXPECT_EQ(S.TR.Selector, 40);
      }
    }
}

TEST_F(X86LegacySystemSelectorTest, BusyWriteFaultDoesNotLoadTRAndCanRetry) {
  kernel();
  S.Core.GPR[6] = 0x103b;
  S.Core.GDTRLimit = 0x103f;
  put(0x8038, 0x0000890058000067ULL);
  put(0x11020, 0x8001, GuestScalarWidth::I32);
  const auto Bytes = test::legacySystemSelectorBytes(3);
  auto R = run(Bytes);
  ASSERT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0x8038u);
  EXPECT_EQ(get(S.Core.GPR[4]), 3u); // Supervisor write protection.
  EXPECT_EQ(S.TR.Selector, 40);
  EXPECT_EQ(get(0x8038, GuestScalarWidth::I64), 0x0000890058000067ULL);
  put(0x11020, 0x8003, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
  ASSERT_TRUE(run({0xcf}).InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x1000u);
  ASSERT_TRUE(run(Bytes).InstructionCompleted);
  EXPECT_EQ(S.TR.Selector, 0x103b);
  EXPECT_EQ(S.TR.Base, 0x5800u);
  EXPECT_EQ(S.TR.Attributes, 0x8b);
  EXPECT_EQ(get(0x8038, GuestScalarWidth::I64), 0x00008b0058000067ULL);
  EXPECT_EQ(get(0x11020), 0x8063u);
}

TEST_F(X86LegacySystemSelectorTest, WordSourceAndBackingStopPreserveState) {
  for (unsigned Form : {2u, 3u}) {
    SetUp();
    kernel();
    S.Core.GPR[3] = 0x9ffe;
    put(0x9ffe, 0x103b, GuestScalarWidth::I16);
    put(0x11028, 0, GuestScalarWidth::I32);
    put(0x11020, 0x40007, GuestScalarWidth::I32);
    S.Core.GDTRLimit = 0x103f;
    const auto Bytes =
        test::legacySystemSelectorBytes(Form, true, true, true, -1);
    const auto R = run(Bytes);
    ASSERT_TRUE(R.BackingFault);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.EFlags, 0xed7u);
    EXPECT_EQ(S.TR.Selector, 40);
    EXPECT_TRUE(S.Core.LDTR.Unusable);
    put(0x11020, 0x8007, GuestScalarWidth::I32);
    put(0x8038, Form == 2 ? 0x0000820058000067ULL : 0x0000890058000067ULL);
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
    EXPECT_EQ(Form == 2 ? S.Core.LDTR.Selector : S.TR.Selector, 0x103b);
  }
}

TEST_F(X86LegacySystemSelectorTest, PrivilegeAndUMIPPrecedeOperandAccess) {
  for (unsigned Form = 0; Form != 4; ++Form) {
    SetUp();
    auto Controls = M->controls();
    Controls.CR4 |= 1u << 11;
    ASSERT_FALSE(bool(M->bindControls(Controls)));
    S.Core.GPR[3] = 0x9000;
    put(0x11024, 0, GuestScalarWidth::I32);
    ASSERT_EQ(run(test::legacySystemSelectorBytes(Form, true, true, true, -1))
                  .DeliveredVector,
              13);
    EXPECT_EQ(get(S.Core.GPR[4]), 0u);
    EXPECT_EQ(S.CR2, 0x12345678u);
  }
}

TEST_F(X86LegacySystemSelectorTest,
       MissingProfileAndMalformedBytesBeforeFetch) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned Case = 0; Case != 7; ++Case) {
      SetUp();
      auto Bytes = test::legacySystemSelectorBytes(Form);
      if (!Case)
        O.SystemSelectors.reset();
      else if (Case < 4)
        Bytes.insert(Bytes.begin(), Case == 1 ? 0xf0 : Case == 2 ? 0xf2 : 0xf3);
      else if (Case == 4)
        Bytes.pop_back();
      else if (Case == 5)
        Bytes.push_back(0);
      else
        Bytes.insert(Bytes.begin(), {0x66, 0x66});
      auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
      ASSERT_FALSE(bool(R));
      llvm::consumeError(R.takeError());
      EXPECT_EQ(get(0x11004), 0x1007u);
    }
}

TEST_F(X86LegacySystemSelectorTest,
       LoadedFlagNibbleSurvivesFollowingInstruction) {
  for (unsigned Form : {2u, 3u})
    for (unsigned Flags = 0; Flags != 16; ++Flags) {
      SetUp();
      kernel();
      S.Core.GPR[6] = 0x3b;
      put(0x7038, (Form == 2 ? 0x0000820058000067ULL : 0x0000890058000067ULL) |
                      (uint64_t(Flags) << 52));
      ASSERT_TRUE(
          run(test::legacySystemSelectorBytes(Form)).InstructionCompleted);
      EXPECT_EQ((Form == 2 ? S.Core.LDTR.Attributes : S.TR.Attributes) >> 12,
                Flags);
      ASSERT_TRUE(run({0x90}).InstructionCompleted);
    }
}

TEST_F(X86LegacySystemSelectorTest, DescriptorReadPrecedesTypeAndBusyWrite) {
  for (unsigned Form : {2u, 3u})
    for (bool Readable : {false, true}) {
      SetUp();
      kernel();
      S.Core.GPR[6] = 0x103b;
      S.Core.GDTRLimit = 0x103f;
      put(0x8038, 0x0000030058000067ULL); // Wrong type and not present.
      put(0x11020, Readable ? 0x8001 : 0x8000, GuestScalarWidth::I32);
      const auto R = run(test::legacySystemSelectorBytes(Form));
      ASSERT_EQ(R.DeliveredVector, Readable ? 13 : 14);
      EXPECT_EQ(get(S.Core.GPR[4]), Readable ? 0x1038u : 0u);
      EXPECT_EQ(S.TR.Selector, 40);
      EXPECT_TRUE(S.Core.LDTR.Unusable);
      EXPECT_EQ(get(0x8038, GuestScalarWidth::I64), 0x0000030058000067ULL);
    }
}

TEST_F(X86LegacySystemSelectorTest, BusyCacheIncludesAliasedPageWalkDirtyBit) {
  kernel();
  S.Core.GPR[6] = 0x103b;
  S.Core.GDTRBase = 0x1000c;
  S.Core.GDTRLimit = 0x103f;
  // The descriptor's low dword also maps the table page itself.
  put(0x11044, 0x0000890000011007ULL);
  ASSERT_TRUE(run(test::legacySystemSelectorBytes(3)).InstructionCompleted);
  EXPECT_EQ(get(0x11044, GuestScalarWidth::I64), 0x00008b0000011067ULL);
  EXPECT_EQ(S.TR.Base, 1u);
  EXPECT_EQ(S.TR.Limit, 0x1067u);
  EXPECT_EQ(S.TR.Attributes, 0x8b);
}
