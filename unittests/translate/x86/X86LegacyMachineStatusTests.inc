//===- X86LegacyMachineStatusTests.inc - Protected CR0 contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Widths, sticky PE, fault atomicity and control publication before traps.
//===----------------------------------------------------------------------===//

class X86LegacyMachineStatusTest : public X86LegacyExecutionTest {
protected:
  void kernel() {
    S.Core.CPL = 0;
    S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
    S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
    S.EFlags = 0xed7;
  }

  void controls(uint32_t CR0, bool UMIP = false) {
    auto C = M->controls();
    C.CR0 = CR0;
    C.CR4 = UMIP ? 1u << 11 : 0;
    llvm::cantFail(M->bindControls(C));
  }
};

TEST_F(X86LegacyMachineStatusTest, StoreWidthsPreserveRegistersAndMemory) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true})
      for (int Register : {-1, 0, 1, 2, 3, 4, 5, 6, 7}) {
        SetUp();
        S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
        S.EFlags = 0xed7;
        controls(0xe005003f);
        O.MachineStatus = X86LegacyMachineStatusProfile::FullCR0;
        S.Core.GPR[3] = 0x9ffe;
        if (Register >= 0)
          S.Core.GPR[Register] = 0x12345678;
        put(0x9ffc, 0xdeadbeef, GuestScalarWidth::I32);
        put(0x11028, 0, GuestScalarWidth::I32);
        ASSERT_TRUE(run(test::legacyMachineStatusBytes(0, Code32, Operand32,
                                                       true, Register))
                        .InstructionCompleted);
        if (Register < 0)
          EXPECT_EQ(get(0x9ffc), 0x003fbeefu);
        else
          EXPECT_EQ(S.Core.GPR[Register],
                    Operand32 ? 0xe005003fu : 0x1234003fu);
        EXPECT_EQ(S.EFlags, 0xed7u);
        EXPECT_EQ(M->controls().CR0, 0xe005003fu);
      }
}

TEST_F(X86LegacyMachineStatusTest, LoadChangesOnlyMP_EM_TSAndCannotClearPE) {
  for (unsigned Initial = 0; Initial != 8; ++Initial)
    for (unsigned Input = 0; Input != 16; ++Input)
      for (bool Operand32 : {false, true}) {
        SetUp();
        kernel();
        controls(0xe0050031 | (Initial << 1));
        S.Core.GPR[6] = 0xfffffff0 | Input;
        ASSERT_TRUE(run(test::legacyMachineStatusBytes(1, true, Operand32))
                        .InstructionCompleted);
        EXPECT_EQ(M->controls().CR0, 0xe0050031u | (Input & 14));
        EXPECT_EQ(S.Core.GPR[6], 0xfffffff0u | Input);
        EXPECT_EQ(S.EFlags, 0xed7u);
      }
}

TEST_F(X86LegacyMachineStatusTest, LoadReadsOnlyAWordAndClearTouchesOnlyTS) {
  kernel();
  controls(0x80050011);
  S.Core.GPR[3] = 0x9ffe;
  put(0x9ffe, 0xfffe, GuestScalarWidth::I16);
  put(0x11028, 0, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x0f, 1, 0x33}).InstructionCompleted);
  EXPECT_EQ(M->controls().CR0, 0x8005001fu);
  ASSERT_TRUE(run({0x0f, 6}).InstructionCompleted);
  EXPECT_EQ(M->controls().CR0, 0x80050017u);
  ASSERT_TRUE(run({0x66, 0x67, 0x64, 0x0f, 6}).InstructionCompleted);
  EXPECT_EQ(M->controls().CR0, 0x80050017u);
  EXPECT_EQ(S.EFlags, 0xed7u);
}

TEST_F(X86LegacyMachineStatusTest, PrivilegeAndUMIPPrecedeOperandAccess) {
  for (unsigned Form = 0; Form != 3; ++Form) {
    SetUp();
    controls(0x8005001f, true);
    S.Core.GPR[3] = 0x9000;
    put(0x11024, 0, GuestScalarWidth::I32);
    const auto R =
        run(test::legacyMachineStatusBytes(Form, true, true, true, -1));
    ASSERT_EQ(R.DeliveredVector, 13);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(get(S.Core.GPR[4]), 0u);
    EXPECT_EQ(S.CR2, 0x12345678u);
    EXPECT_EQ(M->controls().CR0, 0x8005001fu);
    EXPECT_EQ(get(0x11024), 0u);
  }
}

TEST_F(X86LegacyMachineStatusTest, FaultRepairAndRetryDoNotPublishEarly) {
  for (unsigned Form : {0u, 1u}) {
    SetUp();
    kernel();
    controls(0x80050011);
    S.Core.GPR[3] = 0x9000;
    put(0x9000, 0xdeadfffe, GuestScalarWidth::I32);
    put(0x11024, 0, GuestScalarWidth::I32);
    const auto Bytes =
        test::legacyMachineStatusBytes(Form, true, true, true, -1);
    const auto R = run(Bytes);
    ASSERT_EQ(R.DeliveredVector, 14);
    EXPECT_EQ(M->controls().CR0, 0x80050011u);
    EXPECT_EQ(get(0x9000), 0xdeadfffeu);
    EXPECT_EQ(S.CR2, 0x9000u);
    EXPECT_EQ(get(S.Core.GPR[4]), Form == 0 ? 2u : 0u);
    put(0x11024, 0x9007, GuestScalarWidth::I32);
    ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
    ASSERT_TRUE(run({0xcf}).InstructionCompleted);
    EXPECT_EQ(S.EIP, 0x1000u);
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
    EXPECT_EQ(M->controls().CR0, Form == 0 ? 0x80050011u : 0x8005001fu);
    EXPECT_EQ(get(0x9000), Form == 0 ? 0xdead0011u : 0xdeadfffeu);
  }
}

TEST_F(X86LegacyMachineStatusTest, BackingStopRetainsEntryControlAndCanResume) {
  kernel();
  controls(0x80050011);
  S.Core.GPR[3] = 0x8000;
  put(0x11020, 0x40007, GuestScalarWidth::I32);
  const auto R = run({0x0f, 1, 0x33});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(S.EFlags, 0xed7u);
  EXPECT_EQ(M->controls().CR0, 0x80050011u);
  put(0x11020, 0x8007, GuestScalarWidth::I32);
  put(0x8000, 8, GuestScalarWidth::I16);
  ASSERT_TRUE(run({0x0f, 1, 0x33}).InstructionCompleted);
  EXPECT_EQ(M->controls().CR0, 0x80050019u);
}

TEST_F(X86LegacyMachineStatusTest, ControlCommitsBeforeSingleStepDelivery) {
  for (unsigned Form : {1u, 2u}) {
    SetUp();
    kernel();
    controls(0x8005001b);
    S.EFlags = 0x302;
    O.Debug = X86LegacyDebugProfile::InstructionStep;
    S.Debug.emplace();
    S.Core.GPR[6] = 4;
    const auto Bytes = test::legacyMachineStatusBytes(Form);
    const auto R = run(Bytes);
    ASSERT_EQ(R.DeliveredVector, 1);
    EXPECT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(M->controls().CR0, Form == 1 ? 0x80050015u : 0x80050013u);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u + Bytes.size());
  }
}

TEST_F(X86LegacyMachineStatusTest, ProfileAndExactGrammarRefuseBeforeFetch) {
  const std::vector<std::vector<uint8_t>> Rejected{
      {0x0f, 1, 0xe0}, // Dword SMSW requires an explicit undefined choice.
      {0xf0, 0x0f, 6},       {0xf3, 0x0f, 6},
      {0x0f, 6, 0x90},       {0x0f, 1},
      {0x0f, 1, 0x34},       {0x66, 0x66, 0x0f, 1, 0xe0},
      {0xf0, 0x0f, 1, 0x23}, {0xf2, 0x0f, 1, 0xf0}};
  for (const auto &Bytes : Rejected) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x11004), 0x1007u);
    EXPECT_EQ(S.EIP, 0x1000u);
  }
  // Defined word results do not need a target choice.
  ASSERT_TRUE(run({0x66, 0x0f, 1, 0xe0}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 1u);
}

TEST_F(X86LegacyMachineStatusTest,
       FetchFaultPrecedesPrivilegeAndControlChange) {
  controls(0x8005001f);
  put(0x11004, 0, GuestScalarWidth::I32);
  const auto R = run({0x0f, 6});
  ASSERT_EQ(R.DeliveredVector, 14);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.CR2, 0x1000u);
  EXPECT_EQ(M->controls().CR0, 0x8005001fu);
}

TEST_F(X86LegacyMachineStatusTest, DebugBackingStopKeepsCommittedControl) {
  kernel();
  controls(0x8005001f);
  S.EFlags = 0x302;
  O.Debug = X86LegacyDebugProfile::InstructionStep;
  S.Debug.emplace();
  put(0x11018, 0x40007, GuestScalarWidth::I32); // IDT backing is absent.
  const auto R = run({0x0f, 6});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x1002u);
  EXPECT_EQ(M->controls().CR0, 0x80050017u);
  ASSERT_TRUE(S.PendingEvent);
  put(0x11018, 0x6007, GuestScalarWidth::I32);
  const auto Resumed = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(Resumed.DeliveredVector, 1);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1002u);
  EXPECT_EQ(M->controls().CR0, 0x80050017u);
}

TEST_F(X86LegacyMachineStatusTest, AccessControlUsesTargetFeatureAndPrivilege) {
  for (bool SMAP : {false, true})
    for (bool Supervisor : {false, true})
      for (unsigned Form : {3u, 4u}) {
        resetLegacyState({.PhysicalAddressBits = 32, .SMAP = SMAP});
        if (Supervisor)
          kernel();
        S.EFlags = 0x40ed7;
        const auto R = run(test::legacyMachineStatusBytes(Form));
        if (!SMAP || !Supervisor) {
          EXPECT_EQ(R.DeliveredVector, 6);
          EXPECT_FALSE(R.InstructionCompleted);
          EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
        } else {
          EXPECT_TRUE(R.InstructionCompleted);
          EXPECT_EQ(S.EFlags, Form == 3 ? 0xed7u : 0x40ed7u);
          EXPECT_EQ(M->controls().RFlags, S.EFlags);
        }
      }
}

TEST_F(X86LegacyMachineStatusTest, AccessControlChangesNextSMAPRead) {
  resetLegacyState({.PhysicalAddressBits = 32, .SMAP = true});
  kernel();
  for (unsigned Page = 0; Page != 32; ++Page)
    put(0x11000 + Page * 4, Page * 4096 | (Page == 9 ? 7 : 3),
        GuestScalarWidth::I32);
  auto C = M->controls();
  C.CR4 = 1u << 21;
  llvm::cantFail(M->bindControls(C));
  S.Core.GPR[3] = 0x9000;
  put(0x9000, 0x12345678, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x0f, 1, 0xcb}).InstructionCompleted);
  ASSERT_TRUE(run({0x8b, 3}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  ASSERT_TRUE(run({0x0f, 1, 0xca}).InstructionCompleted);
  const uint32_t FaultPC = S.EIP;
  const auto R = run({0x8b, 3});
  ASSERT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0x9000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 1u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), FaultPC);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
}
