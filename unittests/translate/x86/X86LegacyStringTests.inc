//===- X86LegacyStringTests.inc - Segmented REP restart contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Element publication, zero counts, competing faults and asynchronous stops.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, StringsRequireAnExplicitProgressProfile) {
  for (auto Profile :
       {std::optional<X86LegacyStringProfile>{},
        std::optional(static_cast<X86LegacyStringProfile>(255))}) {
    O.Strings = Profile;
    for (std::vector<uint8_t> Bytes :
         {std::vector<uint8_t>{0xf3, 0xa4}, {0xf2, 0xa6}, {0xac}}) {
      auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
      ASSERT_FALSE(bool(R));
      EXPECT_NE(llvm::toString(R.takeError()).find("profile"),
                std::string::npos);
      EXPECT_EQ(get(0x11004) & 0x20, 0u); // No original fetch before admission.
    }
  }
  O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
  for (std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0xf2, 0xa4},
                                     {0xf3, 0xf3, 0xa4},
                                     {0xf0, 0xf3, 0xa4},
                                     {0x67, 0x67, 0xa6},
                                     {0xf3, 0xa4, 0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
}

TEST_F(X86LegacyExecutionTest, ZeroCountStringsDoNotInspectOperands) {
  for (uint8_t Opcode :
       {0xa4, 0xa5, 0xa6, 0xa7, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf}) {
    SetUp();
    O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
    S.Core.GPR[1] = 0xabcd0000;
    S.Core.GPR[6] = 0x1234c001;
    S.Core.GPR[7] = 0x5678d001;
    S.Core.Segments[0].Unusable = S.Core.Segments[3].Unusable = true;
    S.EFlags = 0x50ed7;
    S.SSBlocked = S.STIBlocked = true;
    auto Controls = M->controls();
    Controls.CR0 |= 0x40000;
    ASSERT_FALSE(bool(M->bindControls(Controls)));
    const auto Before = S.Core.GPR;
    auto R = run({0x67, 0xf3, Opcode});
    EXPECT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(S.Core.GPR, Before);
    EXPECT_EQ(S.EFlags, 0x40ed7u);
    EXPECT_FALSE(S.SSBlocked);
    EXPECT_FALSE(S.STIBlocked);
    EXPECT_EQ(S.EIP, 0x1003u);
  }
}

TEST_F(X86LegacyExecutionTest, StringProgressPreservesHighWordsAndDirection) {
  for (bool Backward : {false, true}) {
    SetUp();
    O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
    S.Core.GPR[1] = 0x12340002;
    S.Core.GPR[6] = Backward ? 0xabcd0000 : 0xabcdffff;
    S.Core.GPR[7] = 0xef01c100;
    S.Core.Segments[3].Base = 0x8000;
    S.EFlags = Backward ? 0xed7 : 0xad7;
    S.SSBlocked = S.STIBlocked = true;
    put(0x17fff, 0x11, GuestScalarWidth::I8);
    put(0x8000, 0x22, GuestScalarWidth::I8);
    auto R = run({0x67, 0xf3, 0xa4});
    EXPECT_EQ(R.Status, X86LegacyExecutionStatus::RepeatContinue);
    EXPECT_TRUE(R.InstructionAttempted);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.Core.GPR[1], 0x12340001u);
    EXPECT_EQ(S.Core.GPR[6], Backward ? 0xabcdffffu : 0xabcd0000u);
    EXPECT_EQ(S.EFlags, Backward ? 0x10ed7u : 0x10ad7u);
    EXPECT_FALSE(S.SSBlocked);
    EXPECT_FALSE(S.STIBlocked);
    EXPECT_TRUE(run({0x67, 0xf3, 0xa4}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[1], 0x12340000u);
    EXPECT_EQ(get(0xc100, GuestScalarWidth::I8), Backward ? 0x22u : 0x11u);
    EXPECT_EQ(get(Backward ? 0xc0ff : 0xc101, GuestScalarWidth::I8),
              Backward ? 0x11u : 0x22u);
  }
}

TEST_F(X86LegacyExecutionTest,
       ComparisonFaultRetainsPriorElementsAndEntryFlags) {
  for (uint8_t Opcode : {0xa6, 0xae})
    for (bool Equal : {false, true}) {
      SetUp();
      O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
      S.Core.GPR[0] = 0x11;
      S.Core.GPR[1] = 3;
      S.Core.GPR[6] = 0xc100;
      S.Core.GPR[7] = 0xefff;
      S.EFlags = 0xad7;
      put(0xc100, 0x11111111, GuestScalarWidth::I32);
      put(0xefff, Equal ? 0x11 : 0x22, GuestScalarWidth::I8);
      put(0x1103c, 0xf006, GuestScalarWidth::I32);
      const std::vector<uint8_t> Bytes{uint8_t(Equal ? 0xf3 : 0xf2), Opcode};
      EXPECT_EQ(run(Bytes).Status, X86LegacyExecutionStatus::RepeatContinue);
      EXPECT_EQ(S.Core.GPR[1], 2u);
      EXPECT_EQ(S.EFlags, 0x10ad7u);
      auto R = run(Bytes);
      ASSERT_EQ(R.DeliveredVector, 14);
      EXPECT_EQ(S.Core.GPR[1], 2u);
      EXPECT_EQ(S.Core.GPR[6], Opcode == 0xa6 ? 0xc101u : 0xc100u);
      EXPECT_EQ(S.Core.GPR[7], 0xf000u);
      EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
      EXPECT_EQ(get(S.Core.GPR[4] + 12), 0x10ad7u);
      EXPECT_EQ(S.CR2, 0xf000u);
    }
}

TEST_F(X86LegacyExecutionTest, StringsOwnSourceAndDestinationFaultOrder) {
  for (uint8_t Opcode : {0xa4, 0xa6}) {
    SetUp();
    O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
    S.Core.GPR[1] = 1;
    S.Core.GPR[6] = 0xc100;
    S.Core.GPR[7] = 0xf100;
    put(0x11030, 0xc006, GuestScalarWidth::I32);
    put(0x1103c, 0xf006, GuestScalarWidth::I32);
    const auto R = run({0xf3, Opcode});
    EXPECT_EQ(R.DeliveredVector, 14);
    EXPECT_EQ(S.CR2, Opcode == 0xa4 ? 0xc100u : 0xf100u);
    EXPECT_EQ(S.Core.GPR[1], 1u);
  }
}

TEST_F(X86LegacyExecutionTest, StringsRetainCompletedStoresOnBackingStop) {
  O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
  S.Core.GPR[0] = 0xa5;
  S.Core.GPR[1] = 3;
  S.Core.GPR[7] = 0xefff;
  put(0x1103c, 0x20007, GuestScalarWidth::I32); // Missing physical RAM.
  EXPECT_EQ(run({0xf3, 0xaa}).Status, X86LegacyExecutionStatus::RepeatContinue);
  auto R = run({0xf3, 0xaa});
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.InstructionAttempted);
  EXPECT_EQ(get(0xefff, GuestScalarWidth::I8), 0xa5u);
  EXPECT_EQ(S.Core.GPR[1], 2u);
  EXPECT_EQ(S.Core.GPR[7], 0xf000u);
  EXPECT_EQ(S.EIP, 0x1000u);
  put(0x1103c, 0xf007, GuestScalarWidth::I32);
  EXPECT_EQ(run({0xf3, 0xaa}).Status, X86LegacyExecutionStatus::RepeatContinue);
  EXPECT_TRUE(run({0xf3, 0xaa}).InstructionCompleted);
  EXPECT_EQ(get(0xf000, GuestScalarWidth::I16), 0xa5a5u);
}

TEST_F(X86LegacyExecutionTest, StringInterruptsReturnToTheRemainingCount) {
  for (auto Kind : {X86LegacyEventKind::Interrupt, X86LegacyEventKind::NMI}) {
    SetUp();
    O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
    S.Core.GPR[0] = 0x55;
    S.Core.GPR[1] = 3;
    S.Core.GPR[7] = 0xc100;
    S.SSBlocked = true;
    const uint8_t Vector = Kind == X86LegacyEventKind::NMI ? 2 : 64;
    auto Event = [&] {
      return llvm::cantFail(
          deliverX86LegacyEvent(S, *M, {S.EIP, 0, 0, Vector, Kind}, O));
    };
    EXPECT_EQ(Event().Status, X86LegacyExecutionStatus::EventDeferred);
    EXPECT_EQ(run({0xf3, 0xaa}).Status,
              X86LegacyExecutionStatus::RepeatContinue);
    EXPECT_EQ(Event().Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
    EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x10202u);
    EXPECT_TRUE(run({0xcf}).InstructionCompleted);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.Core.GPR[1], 2u);
    EXPECT_EQ(run({0xf3, 0xaa}).Status,
              X86LegacyExecutionStatus::RepeatContinue);
    EXPECT_TRUE(run({0xf3, 0xaa}).InstructionCompleted);
    EXPECT_EQ(get(0xc100), 0x555555u);
    EXPECT_FALSE(S.NMIBlocked);
  }
}

TEST_F(X86LegacyExecutionTest, StringLockIsInvalidEvenWhenCountIsZero) {
  O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
  S.Core.GPR[1] = 0;
  EXPECT_EQ(run({0xf0, 0xa4}).DeliveredVector, 6);
}

TEST_F(X86LegacyExecutionTest, StringsAuthenticateFetchBeforeZeroCountOrFault) {
  O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
  S.EIP = 0x1fff;
  S.Core.GPR[1] = 0;
  put(0x1fff, 0xf3, GuestScalarWidth::I8);
  put(0x11008, 0x2006, GuestScalarWidth::I32);
  auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0xf3, 0xa4}, O));
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0x2000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1fffu);
}
