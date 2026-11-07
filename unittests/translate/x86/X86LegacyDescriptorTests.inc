//===- X86LegacyDescriptorTests.inc - Protected descriptor queries -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Visibility, read-only tables, exact selector width and fault publication.
//===----------------------------------------------------------------------===//

class X86LegacyDescriptorTest : public X86LegacyExecutionTest {
protected:
  void SetUp() override {
    X86LegacyExecutionTest::SetUp();
    O.DescriptorQueries = X86LegacyDescriptorProfile::TableImage;
  }
};

TEST_F(X86LegacyDescriptorTest, TypesPrivilegesPresenceAndOperandWidths) {
  constexpr bool LimitTypes[16]{false, true,  true,  true, false, false,
                                false, false, false, true, false, true};
  constexpr bool RightsTypes[16]{false, true,  true, true,  true, true, false,
                                 false, false, true, false, true, true};
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned Access = 0; Access != 256; ++Access)
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (unsigned RPL = 0; RPL != 4; ++RPL)
          for (bool Word : {false, true}) {
            S.EIP = 0x1000;
            S.EFlags = 0xed7;
            S.Core.CPL = CPL;
            S.Core.Segments[1].Selector = 24 | CPL;
            S.Core.Segments[1].Attributes = 0xc09b | (CPL << 5);
            S.Core.GPR[0] = 0x89abcdef;
            S.Core.GPR[6] = 0xabcd0038 | RPL;
            const uint64_t Descriptor =
                0x00dabc4567891234ULL | (uint64_t(Access) << 40);
            // Clear the original access byte before installing this row.
            const uint64_t D = (Descriptor & ~(uint64_t{255} << 40)) |
                               (uint64_t(Access) << 40);
            put(0x7038, D);
            const bool Segment = Access & 16;
            const unsigned Type = Access & 15, DPL = (Access >> 5) & 3;
            const bool Visible =
                (Segment && (Type & 12) == 12 || CPL <= DPL && RPL <= DPL) &&
                (Form == 0   ? Segment || RightsTypes[Type]
                 : Form == 1 ? Segment || LimitTypes[Type]
                 : Form == 2 ? Segment && (!(Type & 8) || (Type & 2))
                             : Segment && (Type & 10) == 2);
            ASSERT_TRUE(run(test::legacyDescriptorBytes(Form, true, !Word))
                            .InstructionCompleted);
            const uint32_t Value =
                Form == 0 ? 0x00da0000 | Access << 8 : 0xa1234fff;
            EXPECT_EQ(S.Core.GPR[0],
                      Visible && Form < 2
                          ? Word ? 0x89ab0000 | uint16_t(Value) : Value
                          : 0x89abcdef);
            EXPECT_EQ(S.EFlags, Visible ? 0xed7u : 0xe97u);
            EXPECT_EQ(get(0x7038, GuestScalarWidth::I64), D);
          }
}

TEST_F(X86LegacyDescriptorTest,
       InvalidSelectorsDoNotReadTablesOrChangeDestination) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned Case = 0; Case != 4; ++Case) {
      SetUp();
      S.Core.GDTRLimit = 0xff;
      S.Core.GPR[6] = Case == 0 ? 3 : Case == 1 ? 0x103b : 0x103f;
      S.Core.GPR[0] = 0x12345678;
      S.Core.LDTR = {0x8000, Case == 3 ? 0x103eu : 0x103fu, 48, 0x82,
                     Case == 2};
      S.EFlags = 0xed7;
      put(0x11020, 0, GuestScalarWidth::I32);
      ASSERT_TRUE(run(test::legacyDescriptorBytes(Form)).InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
      EXPECT_EQ(S.EFlags, 0xe97u);
      EXPECT_EQ(S.CR2, 0x12345678u);
      EXPECT_EQ(get(0x11020), 0u);
    }
}

TEST_F(X86LegacyDescriptorTest,
       LDTZeroIndexNonpresentReadOnlyAndLimitGranularity) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (bool Granular : {false, true}) {
      SetUp();
      S.Core.LDTR = {0x8000, 7, 48, 0x82, false};
      S.Core.GPR[6] = 7;
      put(0x8000, (Granular ? 0x00aa720000001234ULL : 0x002a720000001234ULL));
      put(0x11020, 0x8001, GuestScalarWidth::I32);
      ASSERT_TRUE(run(test::legacyDescriptorBytes(Form)).InstructionCompleted);
      EXPECT_TRUE(S.EFlags & 64);
      if (Form < 2)
        EXPECT_EQ(S.Core.GPR[0], Form == 0  ? Granular ? 0xaa7200u : 0x2a7200u
                                 : Granular ? 0xa1234fffu
                                            : 0xa1234u);
      EXPECT_EQ(get(0x11020), 0x8021u); // Supervisor read only; no dirty bit.
      EXPECT_EQ(get(0x8005, GuestScalarWidth::I8), 0x72u); // No accessed write.
    }
}

TEST_F(X86LegacyDescriptorTest, DescriptorPageFaultPrecedesVisibilityFailure) {
  for (unsigned Form = 0; Form != 4; ++Form) {
    SetUp();
    S.Core.GDTRLimit = 0x103f;
    S.Core.GPR[6] = 0x103b;
    S.Core.GPR[0] = 0x12345678;
    put(0x11020, 0, GuestScalarWidth::I32);
    const auto R = run(test::legacyDescriptorBytes(Form));
    EXPECT_EQ(R.DeliveredVector, 14);
    EXPECT_EQ(S.CR2, 0x8038u);
    EXPECT_EQ(get(S.Core.GPR[4]), 0u); // Implicit supervisor descriptor read.
    EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
    EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
  }
}

TEST_F(X86LegacyDescriptorTest, SelectorIsAWordAndBackingStopIsRestartable) {
  for (unsigned Form = 0; Form != 4; ++Form) {
    SetUp();
    S.Core.GPR[3] = 0x9ffe;
    put(0x9ffe, 35, GuestScalarWidth::I16);
    put(0x11028, 0, GuestScalarWidth::I32);
    ASSERT_TRUE(run(test::legacyDescriptorBytes(Form, true, true, true, 0, -1))
                    .InstructionCompleted);
    S.EIP = 0x1000;
    S.Core.GDTRLimit = 0x103f;
    S.Core.GPR[6] = 0x103b;
    S.Core.GPR[0] = 0x12345678;
    S.EFlags = 0xed7;
    put(0x11020, 0x40007, GuestScalarWidth::I32);
    const auto Bytes = test::legacyDescriptorBytes(Form);
    const auto R = run(Bytes);
    EXPECT_TRUE(R.BackingFault);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
    EXPECT_EQ(S.EFlags, 0xed7u);
    EXPECT_EQ(S.EIP, 0x1000u);
    put(0x11020, 0x8007, GuestScalarWidth::I32);
    put(0x8038, 0x00cff2000000ffffULL);
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
    EXPECT_TRUE(S.EFlags & 64);
  }
}

TEST_F(X86LegacyDescriptorTest, DescriptorLinearAddressWrapsBetweenDwords) {
  for (unsigned Form = 0; Form != 4; ++Form) {
    SetUp();
    S.Core.LDTR = {0xfffffffc, 7, 48, 0x82, false};
    S.Core.GPR[6] = 7;
    put(0x10ffc, 0x12007, GuestScalarWidth::I32);
    put(0x12ffc, 0x13007, GuestScalarWidth::I32);
    put(0x13ffc, 0x00001234, GuestScalarWidth::I32);
    put(0, 0x00da7200, GuestScalarWidth::I32);
    ASSERT_TRUE(run(test::legacyDescriptorBytes(Form)).InstructionCompleted);
    EXPECT_TRUE(S.EFlags & 64);
    if (Form < 2)
      EXPECT_EQ(S.Core.GPR[0], Form == 0 ? 0xda7200u : 0xa1234fffu);
    EXPECT_EQ(get(0x12ffc), 0x13027u);
    EXPECT_EQ(get(0x11000), 0x27u);
  }
}

TEST_F(X86LegacyDescriptorTest,
       MissingProfileAndMalformedBytesRefuseBeforeFetch) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned Case = 0; Case != 7; ++Case) {
      SetUp();
      auto Bytes = test::legacyDescriptorBytes(Form);
      if (Case == 0)
        O.DescriptorQueries.reset();
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
