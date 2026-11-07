//===- X86LegacyBranchDebugTests.inc - Protected branch stepping ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Taken predicates, debug acceptance and raw-TF REP grouping.
//===----------------------------------------------------------------------===//

class X86LegacyBranchDebugTest : public X86LegacyDebugTest {
protected:
  void SetUp() override {
    X86LegacyDebugTest::SetUp();
    O.Debug = X86LegacyDebugProfile::BranchWatchpoints;
    O.RepeatDebug.emplace();
    S.Debug->DebugCTL = kX86BranchTraceFlag;
  }
};

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X86LegacyBranchDebugTest, OriginalProtectedBranchSelection) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Comparisons = 0;
  for (bool Code16 : {false, true}) {
    neverd::test::X86BranchDebugOracle CPU;
    using Mode = neverd::test::X86BranchDebugOracle::Mode;
    ASSERT_NO_FATAL_FAILURE(
        CPU.initialize(Code16 ? Mode::Code16 : Mode::Code32));
    for (const auto &Case : neverd::test::x86BranchDebugCases(Code16))
      for (bool Branch : {false, true}) {
        SCOPED_TRACE(testing::Message() << "code16=" << Code16 << " branch="
                                        << Branch << " case=" << Comparisons);
        neverd::test::X86BranchDebugOracle::Result Native;
        ASSERT_NO_FATAL_FAILURE(CPU.run(Case, Branch, Native));
        SetUp();
        if (Code16)
          S.Core.Segments[1].Attributes &= ~0x4000;
        S.EFlags = Case.Flags | 0x100;
        S.Core.GPR[0] = 0;
        S.Core.GPR[1] = Case.Count;
        S.Debug->DebugCTL = Branch ? kX86BranchTraceFlag : 0;
        put(0xd000, 0x1004,
            Code16 ? GuestScalarWidth::I16 : GuestScalarWidth::I32);
        unsigned Offset = 0;
        X86LegacyExecutionResult Got;
        for (const auto Length : Case.Lengths) {
          Got = run(llvm::ArrayRef(Case.Code).slice(Offset, Length));
          Offset += Length;
          if (Got.Status != X86LegacyExecutionStatus::Completed)
            break;
        }
        ASSERT_EQ(Got.DeliveredVector, 1);
        ASSERT_TRUE(Got.InstructionCompleted);
        EXPECT_EQ(get(S.Core.GPR[4]), 0x1000 + Native.PC);
        EXPECT_EQ(S.Core.GPR[0], Native.Accumulator);
        EXPECT_EQ(S.Core.GPR[1], Native.Count);
        EXPECT_EQ(get(S.Core.GPR[4] + 8) & 0xcd5, Native.Flags & 0xcd5);
        EXPECT_EQ(int64_t(get(S.Core.GPR[4] + 12)) - 0xd000, Native.StackDelta);
        EXPECT_EQ(S.Debug->DR6, 0xffff4ff0u);
        EXPECT_EQ(S.Debug->DebugCTL, 0u);
        ++Comparisons;
      }
  }
  RecordProperty("branch_selection_comparisons", Comparisons);
}
#endif

TEST_F(X86LegacyBranchDebugTest, BTFWithoutTFAndOtherControls) {
  S.EFlags &= ~0x100;
  EXPECT_EQ(run({0xeb, 0}).Status, X86LegacyExecutionStatus::Completed);
  EXPECT_EQ(S.Debug->DebugCTL, 2u);
  for (uint64_t Control : {1ULL, 3ULL, 4ULL, 0x8000ULL, 1ULL << 32}) {
    SetUp();
    S.Debug->DebugCTL = Control;
    auto Got = executeX86LegacyInstruction(S, *M, {0x90}, O);
    ASSERT_FALSE(bool(Got));
    llvm::consumeError(Got.takeError());
    EXPECT_EQ(get(0x11004), 0x1007u);
    EXPECT_EQ(S.Debug->DebugCTL, Control);
  }
}

TEST_F(X86LegacyBranchDebugTest,
       DebugCausesClearBTFButSoftwareAndFaultsRetainIt) {
  for (unsigned Case = 0; Case != 7; ++Case) {
    SetUp();
    std::vector<uint8_t> Code{0x90};
    if (Case == 0) {
      S.Debug->DR7 |= 1;
      S.Debug->DR[0] = 0x1000;
    } else if (Case == 1) {
      Code = {0x8b, 0x07};
      S.Core.GPR[7] = 0x3000;
      S.Debug->DR7 |= 0x30001;
      S.Debug->DR[0] = 0x3000;
    } else if (Case == 2) {
      Code = {0xf1};
    } else if (Case == 3) {
      Code = {0xcd, 1};
    } else if (Case == 4) {
      Code = {0x0f, 0x0b};
    } else if (Case == 5) {
      Code = {0xeb, 0};
      S.Core.Segments[1].Limit = 0x1001;
      S.Core.Segments[1].Attributes &= ~0x8000;
    } else {
      Code = {0x0f, 0x21, 0xc0};
      S.Core.CPL = 0;
      S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
      S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
      S.Debug->DR7 |= 0x2000;
      O.GeneralDetectFrame = X86GeneralDetectFrame::FaultResume;
    }
    const auto Got = run(Code);
    EXPECT_EQ(Got.DeliveredVector, Case == 4 ? 6 : Case == 5 ? 13 : 1) << Case;
    const bool Debug = Case < 3 || Case == 6;
    EXPECT_EQ(S.Debug->DebugCTL, Debug ? 0u : 2u) << Case;
    EXPECT_EQ(S.Debug->DR6 & 0x4000, 0u) << Case;
    EXPECT_EQ(S.Debug->DR6 & 0x2000, Case == 6 ? 0x2000u : 0u) << Case;
  }
}

TEST_F(X86LegacyBranchDebugTest,
       DeferredExternalDebugRetainsBTFUntilAcceptance) {
  S.SSBlocked = true;
  const X86LegacyEvent Debug{S.EIP, 0, 0, 1, X86LegacyEventKind::Trap};
  auto Got = llvm::cantFail(deliverX86LegacyEvent(S, *M, Debug, O));
  EXPECT_EQ(Got.Status, X86LegacyExecutionStatus::EventDeferred);
  EXPECT_EQ(S.Debug->DebugCTL, 2u);
  S.SSBlocked = false;
  // Entry cannot read the IDT's physical backing, but #DB is already accepted.
  put(0x11018, 0x30007, GuestScalarWidth::I32);
  Got = llvm::cantFail(deliverX86LegacyEvent(S, *M, Debug, O));
  ASSERT_EQ(Got.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_EQ(S.Debug->DebugCTL, 0u);
  ASSERT_TRUE(S.PendingEvent);
  put(0x11018, 0x6007, GuestScalarWidth::I32);
  Got = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  EXPECT_EQ(Got.DeliveredVector, 1);
  EXPECT_EQ(S.Debug->DebugCTL, 0u);
}

TEST_F(X86LegacyBranchDebugTest, RepeatGroupSeesRawTFAndBTF) {
  O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
  unsigned Calls = 0;
  O.RepeatDebug =
      X86RepeatDebugBinding{[](void *Context, const X86RepeatDebugStep &Step) {
                              ++*static_cast<unsigned *>(Context);
                              EXPECT_NE(Step.Flags & 0x100, 0u);
                              EXPECT_EQ(Step.DebugCTL, 2u);
                              return Step.Count == 1;
                            },
                            &Calls};
  S.Core.GPR[1] = 3;
  S.Core.GPR[6] = 0x3000;
  S.Core.GPR[7] = 0x4000;
  S.Debug->DR[0] = 0x4000;
  S.Debug->DR7 |= 0x10001;
  put(0x3000, 0x332211);
  auto Got = run({0xf3, 0xa4});
  EXPECT_EQ(Got.Status, X86LegacyExecutionStatus::RepeatContinue);
  EXPECT_EQ(S.Debug->PendingData, 1u);
  EXPECT_EQ(S.Debug->DebugCTL, 2u);
  Got = run({0xf3, 0xa4});
  EXPECT_EQ(Got.DeliveredVector, 1);
  EXPECT_FALSE(Got.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[1], 1u);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff1u);
  EXPECT_EQ(S.Debug->DebugCTL, 0u);
  EXPECT_EQ(get(0x4000, GuestScalarWidth::I16), 0x2211u);
  EXPECT_EQ(Calls, 2u);
}

TEST_F(X86LegacyBranchDebugTest,
       ReturnSamplesEntryTFAndStackShadowEndsAtBranch) {
  ASSERT_EQ(run({0x8e, 0xd0}).Status, X86LegacyExecutionStatus::Completed);
  EXPECT_TRUE(S.SSBlocked);
  EXPECT_EQ(S.Debug->DebugCTL, 2u);
  ASSERT_EQ(run({0x74, 0}).Status, X86LegacyExecutionStatus::Completed);
  EXPECT_FALSE(S.SSBlocked); // ZF=0, so this did not branch.
  ASSERT_EQ(run({0xeb, 0}).DeliveredVector, 1);

  SetUp();
  put(0xd000, 0x1234, GuestScalarWidth::I32);
  put(0xd004, 27, GuestScalarWidth::I32);
  put(0xd008, 0x202, GuestScalarWidth::I32); // IRET clears TF.
  ASSERT_EQ(run({0xcf}).DeliveredVector, 1);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1234u);
  EXPECT_EQ(get(S.Core.GPR[4] + 8) & 0x100, 0u);
  EXPECT_EQ(S.Debug->DR6, 0xffff4ff0u);
  EXPECT_EQ(S.Debug->DebugCTL, 0u);
}
