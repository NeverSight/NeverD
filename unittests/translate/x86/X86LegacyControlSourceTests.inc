//===- X86LegacyControlSourceTests.inc - Compiled MOV-CR continuations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Generated C sees live paging changes, fault retries and committed traps.
//===----------------------------------------------------------------------===//

TEST_P(X86LegacySourceTest, ControlRegisterWidthsAndReadAfterWrite) {
  for (bool Code32 : {false, true})
    for (unsigned CR : {0u, 2u, 3u, 4u}) {
      controlState(false, Code32);
      S.Core.GPR[6] = CR == 0   ? 0x80050009
                      : CR == 2 ? 0xdeadbeef
                      : CR == 3 ? 0x10018
                                : 0x88;
      S.Core.GPR[0] = 0x12345678;
      auto Write = test::legacyControlBytes(CR, true, 6, 0, true);
      auto Read = test::legacyControlBytes(CR, false, 0, 1, true);
      const unsigned Bits = Code32 ? 32 : 16;
      Driver Run = nullptr;
      ASSERT_NO_FATAL_FAILURE(
          compile({{0x1000, Bits, Write},
                   {uint32_t(0x1000 + Write.size()), Bits, Read}},
                  Run));
      X86LegacySourceExecution Context(S, *M, O);
      uint64_t Budget = 2;
      ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(S.Core.GPR[0], CR == 0 ? 0x80050019u : S.Core.GPR[6]);
      EXPECT_EQ(S.EIP, 0x1000 + Write.size() + Read.size());
      EXPECT_EQ(S.EFlags, 0xed7u);
    }
}

TEST_P(X86LegacySourceTest, ControlRegisterCR3SelectsNewInstructionBytes) {
  for (bool PAE : {false, true}) {
    controlState(PAE);
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compile({{0x1000, 32, {0x0f, 0x22, 0xde}},
                 {0x1003, 32, {0xb8, 0x78, 0x56, 0x34, 0x12}}},
                Run));
    if (PAE) {
      put(0x13000, 0x14001);
      put(0x14000, 0x15007);
      put(0x15008, 0x9007);
    } else {
      put(0x13000, 0x14007, GuestScalarWidth::I32);
      put(0x14004, 0x9007, GuestScalarWidth::I32);
    }
    put(0x1003, 0xccccccccccULL);
    put(0x9003, 0x12345678b8ULL);
    S.Core.GPR[6] = 0x13000;
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 2;
    ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
    EXPECT_EQ(S.EIP, 0x1008u);
    EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
    EXPECT_EQ(M->controls().CR3, 0x13000u);
  }
}

TEST_P(X86LegacySourceTest, ControlRegisterPDPTEFaultRepairAndRetry) {
  controlState(true);
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 32, {0x0f, 0x22, 0xde}},
               {0x1003, 32, {0x0f, 0x20, 0xd8}},
               {0x2000, 32, {0xc7, 5, 0x18, 0x30, 1, 0, 0, 0, 0, 0}},
               {0x200a, 32, {0x83, 0xc4, 4}},
               {0x200d, 32, {0xcf}}},
              Run));
  S.Core.GPR[6] = 0x13000;
  put(0x13000, 0x11001);
  put(0x13018, 3);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().DeliveredVector, 13);
  EXPECT_FALSE(Context.lastResult().InstructionCompleted);
  EXPECT_EQ(M->controls().CR3, 0x10000u);
  EXPECT_EQ(S.CR2, 0x12345678u);
  Budget = 5;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.EIP, 0x1006u);
  EXPECT_EQ(S.Core.GPR[0], 0x13000u);
  EXPECT_EQ(M->controls().CR3, 0x13000u);
}

TEST_P(X86LegacySourceTest, ControlRegisterCommitPrecedesDebugEntry) {
  controlState();
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0x22, 0xde}}}, Run));
  // The new CR3 remaps the handler's IDT page. A stale bank would enter 2000.
  put(0x13000, 0x14007, GuestScalarWidth::I32);
  for (unsigned I = 0; I != 32; ++I)
    put(0x14000 + I * 4, I * 4096 | 7, GuestScalarWidth::I32);
  put(0x14018, 0x40007, GuestScalarWidth::I32);
  put(0x9008, 0x3000 | (uint64_t{8} << 16) | (uint64_t{0x8e} << 40));
  S.Core.GPR[6] = 0x13000;
  S.EFlags = 0x302;
  S.Debug.emplace();
  O.Debug = X86LegacyDebugProfile::InstructionStep;
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
  EXPECT_TRUE(Context.lastResult().InstructionCompleted);
  ASSERT_TRUE(S.PendingEvent);
  EXPECT_EQ(S.EIP, 0x1003u);
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(M->controls().CR3, 0x13000u);
  put(0x14018, 0x9007, GuestScalarWidth::I32);
  ASSERT_EQ(Context.resume(), 1u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().DeliveredVector, 1);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1003u);
  EXPECT_EQ(M->controls().CR3, 0x13000u);
}

TEST_P(X86LegacySourceTest, ControlRegisterBackingStopKeepsBudgetAndState) {
  controlState(true);
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0x22, 0xde}}}, Run));
  S.Core.GPR[6] = 0x20000;
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 2;
  ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
  EXPECT_EQ(Budget, 1u);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(M->controls().CR3, 0x10000u);
  EXPECT_EQ(M->controls().PDPTE[0], 0x11001u);
  S.Core.GPR[6] = 0x10000;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.EIP, 0x1003u);
}

TEST_P(X86LegacySourceTest, ControlRegisterBindingRefusesBeforeFetch) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0x20, 0xc0}}}, Run));
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  EXPECT_EQ(Run(&Context, &Budget), 256u);
  EXPECT_EQ(Budget, 1u);
  EXPECT_EQ(get(0x11004), 0x1007u);
  EXPECT_EQ(S.EIP, 0x1000u);
}
