//===- X86LegacyExecutionBreakpointTests.inc - Execute fault contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Match original linear instruction starts before fetch, with persistent #DB.
//===----------------------------------------------------------------------===//

class X86LegacyExecutionBreakpointTest : public X86LegacyDebugTest {
protected:
  void SetUp() override {
    X86LegacyDebugTest::SetUp();
    O.Debug = X86LegacyDebugProfile::ExecuteBreakpoints;
    S.Debug->DR7 = 0x411;
    S.Debug->DR = {0x1000, 0x1001, 0x1000, 0x1007};
  }
};

TEST_F(X86LegacyExecutionBreakpointTest, ResumeAndStepHaveIndependentCauses) {
  for (unsigned Flags = 0; Flags != 4; ++Flags) {
    SetUp();
    S.EFlags = 0x202 | ((Flags & 1) ? 0x100 : 0) | ((Flags & 2) ? 0x10000 : 0);
    const auto R = run({0x40});
    EXPECT_TRUE(R.InstructionAttempted);
    EXPECT_EQ(R.InstructionCompleted, bool(Flags & 2));
    EXPECT_EQ(S.Core.GPR[0], Flags & 2 ? 36u : 35u);
    EXPECT_EQ(S.Debug->DR6, !(Flags & 2) ? 0xffff0ff5u
                            : Flags & 1  ? 0xffff4ff0u
                                         : 0xffff0ff0u);
    if (Flags == 2) {
      EXPECT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
      EXPECT_EQ(S.EFlags & 0x10000, 0u);
    } else {
      EXPECT_EQ(R.DeliveredVector, 1);
      EXPECT_EQ(get(S.Core.GPR[4]), Flags & 2 ? 0x1001u : 0x1000u);
      EXPECT_EQ(get(S.Core.GPR[4] + 8) & 0x10000, 0u);
    }
  }
}

TEST_F(X86LegacyExecutionBreakpointTest, NextComparatorCombinesWithSingleStep) {
  S.Debug->DR = {0x1001, 0x1000, 0x1001, 0x1000};
  const auto R = run({0x40});
  ASSERT_EQ(R.DeliveredVector, 1);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 36u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1001u);
  EXPECT_EQ(S.Debug->DR6, 0xffff4ff5u);
}

TEST_F(X86LegacyExecutionBreakpointTest, LinearAddressPrecedesFetchAndCSLimit) {
  S.Core.Segments[1].Base = 0xfffff000;
  S.Core.Segments[1].Limit = 0xfff;
  S.Debug->DR = {0, 1, 0, 7}; // 32-bit linear address wraps at CS.base + EIP.
  const auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x40}, O));
  ASSERT_EQ(R.DeliveredVector, 1);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 35u);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff5u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
  EXPECT_EQ(get(0x11000), 7u); // No fetch from the wrapped linear address.
}

TEST_F(X86LegacyExecutionBreakpointTest, AcceptedFaultResumesWithoutFetching) {
  put(0x11004, 0x21007, GuestScalarWidth::I32); // Missing instruction backing.
  put(0x11018, 0x20007, GuestScalarWidth::I32); // Missing IDT backing.
  const auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x40}, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingEvent);
  EXPECT_EQ(S.PendingEvent->Event.Kind,
            X86LegacyEventKind::InstructionBreakpoint);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff5u);
  put(0x11018, 0x6007, GuestScalarWidth::I32);
  const auto Resumed = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(Resumed.DeliveredVector, 1);
  EXPECT_FALSE(S.PendingEvent);
  EXPECT_EQ(S.Core.GPR[0], 35u);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff5u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 8) & 0x10000, 0u);
}

TEST_F(X86LegacyExecutionBreakpointTest, UnsupportedControlsRefuseBeforeFetch) {
  for (uint64_t Control : {0x10411, 0x20411, 0x40411, 0x2411, 0xc11, 0x11}) {
    SetUp();
    S.Debug->DR7 = Control;
    auto Result = executeX86LegacyInstruction(S, *M, {0x40}, O);
    ASSERT_FALSE(bool(Result)) << Control;
    llvm::consumeError(Result.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
    EXPECT_EQ(get(0x11004), 0x1007u);
    EXPECT_FALSE(S.PendingEvent);
  }
}
