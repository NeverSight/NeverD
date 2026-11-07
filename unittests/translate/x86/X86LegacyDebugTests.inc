//===- X86LegacyDebugTests.inc - Protected instruction-step events --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Debug cause publication, entry-TF sampling and original exception frames.
//===----------------------------------------------------------------------===//

class X86LegacyDebugTest : public X86LegacyExecutionTest {
protected:
  void SetUp() override {
    X86LegacyExecutionTest::SetUp();
    S.Debug.emplace();
    O.Debug = X86LegacyDebugProfile::InstructionStep;
    S.EFlags = 0x302;
  }
};

TEST_F(X86LegacyDebugTest, MissingOrIncompatibleBindingRefusesBeforeFetch) {
  for (unsigned Case = 0; Case != 7; ++Case) {
    SetUp();
    if (Case == 0)
      S.Debug.reset();
    else if (Case == 1)
      O.Debug.reset();
    else if (Case == 2)
      O.Debug = static_cast<X86LegacyDebugProfile>(255);
    else if (Case == 3)
      S.Debug->DR7 |= 1;
    else if (Case == 4)
      S.Debug->DebugCTL = 2;
    else if (Case == 5)
      S.Debug->DR6 |= 0x1000;
    else
      S.Debug->DR6 &= ~0x10;
    auto R = executeX86LegacyInstruction(S, *M, {0x90}, O);
    ASSERT_FALSE(bool(R)) << Case;
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004), 0x1007u); // No instruction fetch or A-bit write.
    EXPECT_FALSE(S.PendingEvent);
  }
}

TEST_F(X86LegacyDebugTest, ResumeFlagDoesNotSuppressSingleStep) {
  S.EFlags |= 0x10000;
  S.Debug->DR6 = 0xfffeafff; // Sticky BD/BT, inactive B0..B3, old RTM=0.
  const auto R = run({0xb8, 0x78, 0x56, 0x34, 0x12});
  ASSERT_EQ(R.DeliveredVector, 1);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(S.Debug->DR6, 0xffffeff0u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1005u);
  EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x302u);
  EXPECT_EQ(S.EFlags, 2u);
}

TEST_F(X86LegacyDebugTest, PriorBusLockStatusSurvivesUnrelatedDebugCauses) {
  // A caller can supply historical BLD/RTM status from a capable processor.
  // These causes are not produced here. The local native oracle has neither
  // capability, so this architectural rule has separate software coverage.
  for (bool Icebp : {false, true}) {
    SetUp();
    S.Debug->DR6 = 0xfffee7ff;
    ASSERT_EQ(run({uint8_t(Icebp ? 0xf1 : 0x90)}).DeliveredVector, 1);
    EXPECT_EQ(S.Debug->DR6, 0xffffe7f0u);
  }
}

TEST_F(X86LegacyDebugTest, PopFlagsSamplesTFBeforeChangingIt) {
  for (bool EntryTF : {false, true}) {
    SetUp();
    S.EFlags = EntryTF ? 0x302 : 0x202;
    put(0xd000, EntryTF ? 0x202 : 0x302, GuestScalarWidth::I32);
    auto R = run({0x9d});
    if (!EntryTF) {
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
      EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
      EXPECT_EQ(S.EIP, 0x1001u);
      R = run({0x90});
    }
    ASSERT_EQ(R.DeliveredVector, 1);
    EXPECT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(S.Debug->DR6, 0xffff4ff0u);
    EXPECT_EQ(get(S.Core.GPR[4]), EntryTF ? 0x1001u : 0x1002u);
    EXPECT_EQ(get(S.Core.GPR[4] + 8), EntryTF ? 0x202u : 0x302u);
  }
}

TEST_F(X86LegacyDebugTest, StackShadowSuppressesRatherThanDefersTF) {
  ASSERT_EQ(run({0x8e, 0xd0}).Status, X86LegacyExecutionStatus::Completed);
  ASSERT_TRUE(S.SSBlocked);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
  // A caller-supplied transition clears TF before the next instruction. There
  // is no postponed BS cause from MOV SS that could turn this NOP into a trap.
  S.EFlags &= ~0x100u;
  ASSERT_EQ(run({0x90}).Status, X86LegacyExecutionStatus::Completed);
  EXPECT_FALSE(S.SSBlocked);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);

  SetUp();
  ASSERT_EQ(run({0x8e, 0xd0}).Status, X86LegacyExecutionStatus::Completed);
  const auto R = run({0x8e, 0xd0});
  ASSERT_EQ(R.DeliveredVector, 1);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1004u);
  EXPECT_FALSE(S.SSBlocked);
}

TEST_F(X86LegacyDebugTest, IcebpAfterStackLoadKeepsItsOwnCause) {
  S.Debug->DR6 = 0xfffe0fff;
  ASSERT_EQ(run({0x8e, 0xd0}).Status, X86LegacyExecutionStatus::Completed);
  const auto R = run({0xf1});
  ASSERT_EQ(R.DeliveredVector, 1);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u); // ICEBP is not the TF/BS producer.
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1003u);
  EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x302u);
  EXPECT_FALSE(S.SSBlocked);
}

TEST_F(X86LegacyDebugTest, SoftwareEntriesAndFaultsDoNotInventDebugStatus) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0xcd, 1}, {0xcc}, {0xce}, {0x0f, 0x0b}}) {
    SetUp();
    S.EFlags |= 0x800;
    S.Debug->DR6 = 0xfffe0fff;
    const auto R = run(Bytes);
    EXPECT_EQ(R.DeliveredVector, Bytes[0] == 0xcd   ? 1
                                 : Bytes[0] == 0xcc ? 3
                                 : Bytes[0] == 0xce ? 4
                                                    : 6);
    EXPECT_EQ(S.Debug->DR6, 0xfffe0fffu);
    EXPECT_EQ(R.InstructionCompleted, Bytes[0] != 0x0f);
  }
}

TEST_F(X86LegacyDebugTest, RepeatIterationTrapsWithOriginalPCAndResumeFlag) {
  O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
  S.Core.GPR[1] = 3;
  S.Core.GPR[6] = 0x4000;
  S.Core.GPR[7] = 0x5000;
  put(0x4000, 0x765432, GuestScalarWidth::I32);
  const auto R = run({0xf3, 0xa4});
  ASSERT_EQ(R.DeliveredVector, 1);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[1], 2u);
  EXPECT_EQ(S.Core.GPR[6], 0x4001u);
  EXPECT_EQ(S.Core.GPR[7], 0x5001u);
  EXPECT_EQ(get(0x5000), 0x32u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x10302u);
  EXPECT_EQ(S.Debug->DR6, 0xffff4ff0u);
}

TEST_F(X86LegacyDebugTest, InterruptReturnSamplesEntryTFAndRestoredRF) {
  put(0xd000, 0x1800, GuestScalarWidth::I32);
  put(0xd004, 27, GuestScalarWidth::I32);
  put(0xd008, 0x10202, GuestScalarWidth::I32);
  S.NMIBlocked = true;
  const auto R = run({0xcf});
  ASSERT_EQ(R.DeliveredVector, 1);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(S.NMIBlocked);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1800u);
  EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x10202u);
  EXPECT_EQ(S.Debug->DR6, 0xffff4ff0u);
}

TEST_F(X86LegacyDebugTest, FailedDebugEntryKeepsCompletedInstructionAndCause) {
  put(0x6008, 0x2000 | (uint64_t{8} << 16) | (uint64_t{0x6e} << 40));
  const auto R = run({0x40}); // INC EAX completes before #NP during #DB entry.
  ASSERT_EQ(R.DeliveredVector, 11);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 36u);
  EXPECT_EQ(S.Debug->DR6, 0xffff4ff0u);
  EXPECT_EQ(get(S.Core.GPR[4]), 11u); // IDT selector plus external-event bit.
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1001u);
}

TEST_F(X86LegacyDebugTest, AcceptedDebugEventResumesWithoutReexecuting) {
  put(0x11018, 0x20007, GuestScalarWidth::I32); // IDT maps missing backing.
  auto R = run({0x40});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingEvent);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x1001u);
  EXPECT_EQ(S.Core.GPR[0], 36u);
  EXPECT_EQ(S.Debug->DR6, 0xffff4ff0u);
  auto Retry = executeX86LegacyInstruction(S, *M, {0x40}, O);
  ASSERT_FALSE(bool(Retry));
  llvm::consumeError(Retry.takeError());
  put(0x11018, 0x6007, GuestScalarWidth::I32);
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(R.DeliveredVector, 1);
  EXPECT_FALSE(S.PendingEvent);
  EXPECT_EQ(S.Core.GPR[0], 36u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1001u);
}

TEST_F(X86LegacyDebugTest, HandlerReturnStepsEveryRepeatElementOnce) {
  O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
  S.Core.GPR[1] = 3;
  S.Core.GPR[6] = 0x4000;
  S.Core.GPR[7] = 0x5000;
  put(0x4000, 0x765432, GuestScalarWidth::I32);
  for (unsigned I = 0; I != 3; ++I) {
    const auto R = run({0xf3, 0xa4});
    ASSERT_EQ(R.DeliveredVector, 1);
    EXPECT_EQ(R.InstructionCompleted, I == 2);
    EXPECT_EQ(get(S.Core.GPR[4]), I == 2 ? 0x1002u : 0x1000u);
    EXPECT_EQ(get(S.Core.GPR[4] + 8), I == 2 ? 0x302u : 0x10302u);
    ASSERT_EQ(run({0xcf}).Status, X86LegacyExecutionStatus::Completed);
    EXPECT_EQ(S.Core.CPL, 3u);
    EXPECT_EQ(S.Core.GPR[4], 0xd000u);
  }
  EXPECT_EQ(S.Core.GPR[1], 0u);
  EXPECT_EQ(S.Core.GPR[6], 0x4003u);
  EXPECT_EQ(S.Core.GPR[7], 0x5003u);
  EXPECT_EQ(get(0x5000), 0x765432u);
}

TEST_F(X86LegacyDebugTest, OperandFaultRepairsBeforeGeneratingStepCause) {
  S.Core.GPR[7] = 0x8000;
  S.Debug->DR6 = 0xfffe0fff;
  put(0x8000, 0x12345678, GuestScalarWidth::I32);
  put(0x11020, 0x8006, GuestScalarWidth::I32);
  const auto Fault = run({0x8b, 0x07});
  ASSERT_EQ(Fault.DeliveredVector, 14);
  EXPECT_FALSE(Fault.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 35u);
  EXPECT_EQ(S.Debug->DR6, 0xfffe0fffu);
  // The original handler repairs the PTE and discards #PF's error slot.
  ASSERT_EQ(
      run({0xc7, 0x05, 0x20, 0x10, 0x01, 0x00, 0x07, 0x80, 0x00, 0x00}).Status,
      X86LegacyExecutionStatus::Completed);
  ASSERT_EQ(run({0x83, 0xc4, 4}).Status, X86LegacyExecutionStatus::Completed);
  ASSERT_EQ(run({0xcf}).Status, X86LegacyExecutionStatus::Completed);
  ASSERT_EQ(S.EIP, 0x1000u);
  const auto Retry = run({0x8b, 0x07});
  ASSERT_EQ(Retry.DeliveredVector, 1);
  EXPECT_TRUE(Retry.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(S.Debug->DR6, 0xffff4ff0u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1002u);
}

TEST_F(X86LegacyDebugTest, ChangedOrUnavailableCodeDoesNotPublishDebugCause) {
  S.Debug->DR6 = 0xfffe0fff;
  put(0x1000, 0x90, GuestScalarWidth::I8);
  auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x40}, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::CodeChanged);
  EXPECT_EQ(S.Debug->DR6, 0xfffe0fffu);
  put(0x11004, 0x20007, GuestScalarWidth::I32);
  R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x40}, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_EQ(S.Debug->DR6, 0xfffe0fffu);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_FALSE(S.PendingEvent);
}
