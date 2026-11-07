//===- X86LegacyStackTests.inc - Protected stack continuation contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve partial registers, aliased RAM and debug causes across host stops.
//===----------------------------------------------------------------------===//

class X86LegacyStackTest : public X86LegacyExecutionTest {};

TEST_F(X86LegacyStackTest, ExactGrammarAndProfileAdmissionPrecedeFetch) {
  for (bool Code32 : {false, true}) {
    for (const std::vector<uint8_t> Bytes :
         {std::vector<uint8_t>{0x60},
          {0x61},
          {0x66, 0x60},
          {0x66, 0x61},
          {0x67, 0x64, 0xc8, 0xff, 0xff, 0xff}}) {
      auto Error = validateX86LegacyInstructionEncoding(Bytes, Code32);
      EXPECT_FALSE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
    for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0x60, 0},
                                             {0x61, 0},
                                             {0xc8},
                                             {0xc8, 0},
                                             {0xc8, 0, 0},
                                             {0xc8, 0, 0, 0, 0},
                                             {0xf0, 0x60},
                                             {0xf0, 0xc8, 0, 0, 0},
                                             {0x66, 0x66, 0x61}}) {
      auto Error = validateX86LegacyInstructionEncoding(Bytes, Code32);
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
  }
  O.Scalars.reset();
  auto Result = executeX86LegacyInstruction(S, *M, {0x60}, O);
  ASSERT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(get(0x11004), 0x1007u);
  EXPECT_FALSE(S.PendingStack);
}

TEST_F(X86LegacyStackTest, PopAllFaultKeepsLoadedRegistersAndOriginalSP) {
  put(0x5804, 0xb000, GuestScalarWidth::I32);
  S.Core.GPR = {1, 2, 3, 4, 0xcff0, 5, 6, 7};
  put(0xcff0, 0x11223344, GuestScalarWidth::I32);
  put(0xcff4, 0x22334455, GuestScalarWidth::I32);
  put(0xcff8, 0x33445566, GuestScalarWidth::I32);
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  const auto R = run({0x61});
  ASSERT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.Core.GPR[7], 0x11223344u);
  EXPECT_EQ(S.Core.GPR[6], 0x22334455u);
  EXPECT_EQ(S.Core.GPR[5], 0x33445566u);
  EXPECT_EQ(S.Core.GPR[3], 4u);
  EXPECT_EQ(get(S.Core.GPR[4] + 16), 0xcff0u);
  EXPECT_EQ(S.CR2, 0xd000u);
  EXPECT_FALSE(S.PendingStack);
}

TEST_F(X86LegacyStackTest, PopAllBackingStopRetainsCompletedInputs) {
  S.Core.GPR[4] = 0xcff0;
  for (unsigned I = 0; I != 8; ++I)
    put(0xcff0 + I * 4, 0x12340000 + I, GuestScalarWidth::I32);
  put(0x11034, 0x40007, GuestScalarWidth::I32);
  const auto First = run({0x61});
  ASSERT_EQ(First.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingStack);
  EXPECT_TRUE(First.InstructionAttempted);
  EXPECT_EQ(S.Core.GPR[4], 0xcff0u);
  EXPECT_EQ(S.Core.GPR[7], 0x12340000u);
  EXPECT_EQ(S.PendingStack->CompletedAccesses.size(), 4u);
  put(0x11034, 0xd007, GuestScalarWidth::I32);
  const auto Resumed =
      llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x61}, O));
  EXPECT_TRUE(Resumed.InstructionCompleted);
  EXPECT_FALSE(Resumed.InstructionAttempted);
  EXPECT_EQ(S.Core.GPR[4], 0xd010u);
  EXPECT_EQ(S.Core.GPR[0], 0x12340007u);
  EXPECT_EQ(S.Core.GPR[3], 0x12340004u);
  EXPECT_FALSE(S.PendingStack);
}

TEST_F(X86LegacyStackTest, EnterResumeDoesNotReplayAliasedInputsOrFetch) {
  S.Core.GPR[4] = 0xd010;
  S.Core.GPR[5] = 0xd008;
  S.EIP = 0xd00c; // The first push overwrites the already fetched instruction.
  put(0xd004, 0x11223344, GuestScalarWidth::I32);
  put(0xd000, 0x55667788, GuestScalarWidth::I32);
  put(0xcffc, 0x99aabbcc, GuestScalarWidth::I32);
  put(0x11030, 0x40007, GuestScalarWidth::I32);
  const std::vector<uint8_t> Bytes{0xc8, 0, 0, 4};
  const auto First = run(Bytes);
  ASSERT_EQ(First.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingStack);
  EXPECT_EQ(S.PendingStack->CompletedAccesses.size(), 5u);
  EXPECT_EQ(get(0xd004), 0x55667788u); // Overwrote the first completed read.
  EXPECT_EQ(get(0xd008), 0x11223344u);
  EXPECT_EQ(get(0xd00c), 0xd008u);
  EXPECT_EQ(S.Core.GPR[4], 0xd010u);
  EXPECT_EQ(S.Core.GPR[5], 0xd008u);
  put(0x11030, 0xc007, GuestScalarWidth::I32);
  const auto Resumed =
      llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
  ASSERT_TRUE(Resumed.InstructionCompleted);
  EXPECT_FALSE(Resumed.InstructionAttempted);
  EXPECT_FALSE(S.PendingStack);
  EXPECT_EQ(get(0xd008), 0x11223344u);
  EXPECT_EQ(get(0xd004), 0x55667788u);
  EXPECT_EQ(get(0xd000), 0x99aabbccu);
  EXPECT_EQ(get(0xcffc), 0xd00cu);
  EXPECT_EQ(S.Core.GPR[4], 0xcffcu);
  EXPECT_EQ(S.Core.GPR[5], 0xd00cu);
  EXPECT_EQ(S.EIP, 0xd010u);
}

TEST_F(X86LegacyStackTest, PendingStackRejectsRedirectionAndExternalEvents) {
  S.Core.GPR[4] = 0xd008;
  put(0x11030, 0x40007, GuestScalarWidth::I32);
  ASSERT_EQ(run({0x60}).Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingStack);
  auto Reject = [](auto Result) {
    EXPECT_FALSE(bool(Result));
    if (!Result)
      llvm::consumeError(Result.takeError());
  };
  Reject(executeX86LegacyInstruction(S, *M, {0x61}, O));
  for (auto Kind : {X86LegacyEventKind::Interrupt, X86LegacyEventKind::NMI})
    Reject(deliverX86LegacyEvent(
        S, *M,
        {S.EIP, 0, 0, uint8_t(Kind == X86LegacyEventKind::NMI ? 2 : 64), Kind},
        O));
  ++S.Core.GPR[0];
  Reject(executeX86LegacyInstruction(S, *M, {0x60}, O));
  --S.Core.GPR[0];
  ++S.EIP;
  Reject(executeX86LegacyInstruction(S, *M, {0x60}, O));
  --S.EIP;
  EXPECT_EQ(S.PendingStack->CompletedAccesses.size(), 2u);
  EXPECT_EQ(get(0x11018), 0x6007u); // Rejected events never read the IDT.
  put(0x11030, 0xc007, GuestScalarWidth::I32);
  EXPECT_TRUE(llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x60}, O))
                  .InstructionCompleted);
}

TEST_F(X86LegacyStackTest, DiscardedSlotReadsButFinalProbeDoesNotStore) {
  for (uint8_t Opcode : {0x61, 0xc8}) {
    SetUp();
    S.EFlags = 0x202;
    S.Core.GPR[4] = 0xd100;
    S.Debug.emplace();
    O.Debug = X86LegacyDebugProfile::Watchpoints;
    S.Debug->DR[0] = Opcode == 0x61 ? 0xd10c : 0xc0fc;
    S.Debug->DR7 = 0xf0401; // Four-byte data read/write comparator.
    put(0xc0fc, 0xabcdef01, GuestScalarWidth::I32);
    const auto R = Opcode == 0x61 ? run({0x61}) : run({0xc8, 0, 0x10, 0});
    EXPECT_EQ(R.Status, Opcode == 0x61
                            ? X86LegacyExecutionStatus::EventDelivered
                            : X86LegacyExecutionStatus::Completed);
    EXPECT_EQ(S.Debug->DR6, Opcode == 0x61 ? 0xffff0ff1u : 0xffff0ff0u);
    EXPECT_EQ(get(0xc0fc), 0xabcdef01u);
    if (Opcode == 0xc8)
      EXPECT_EQ(get(0x11030) & 0x60, 0x60u); // Probe still sets A/D.
  }
}

TEST_F(X86LegacyStackTest, WatchpointsSurviveBackingThenTrapAfterCompletion) {
  S.EFlags = 0x202;
  S.Core.GPR[4] = 0xd008;
  S.Debug.emplace();
  O.Debug = X86LegacyDebugProfile::Watchpoints;
  S.Debug->DR[0] = 0xd004;
  S.Debug->DR7 = 0xd0401;
  put(0x11030, 0x40007, GuestScalarWidth::I32);
  auto R = run({0x60});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingStack);
  EXPECT_EQ(S.PendingStack->DataBreakpoints, 1);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
  put(0x11030, 0xc007, GuestScalarWidth::I32);
  R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x60}, O));
  ASSERT_EQ(R.DeliveredVector, 1);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff1u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1001u);
  EXPECT_FALSE(S.PendingStack);
}

TEST_F(X86LegacyStackTest, ResumedFaultRetainsPushButDiscardsSpeculativeDebug) {
  put(0x5804, 0xb000, GuestScalarWidth::I32);
  S.EFlags = 0x202;
  S.Core.GPR[4] = 0xd100;
  S.Core.GPR[5] = 0x12345678;
  S.Debug.emplace();
  O.Debug = X86LegacyDebugProfile::Watchpoints;
  S.Debug->DR[0] = 0xd0fc;
  S.Debug->DR7 = 0xd0401;
  put(0x11030, 0x40007, GuestScalarWidth::I32);
  auto R = run({0xc8, 0, 0x10, 0});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingStack);
  EXPECT_EQ(S.PendingStack->CompletedAccesses.size(), 1u);
  EXPECT_EQ(S.PendingStack->DataBreakpoints, 1);
  put(0x11030, 0xc006, GuestScalarWidth::I32);
  R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0xc8, 0, 0x10, 0}, O));
  ASSERT_EQ(R.DeliveredVector, 14);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[5], 0x12345678u);
  EXPECT_EQ(get(0xd0fc), 0x12345678u);
  EXPECT_EQ(get(S.Core.GPR[4] + 16), 0xd100u);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
  EXPECT_FALSE(S.PendingStack);
}
