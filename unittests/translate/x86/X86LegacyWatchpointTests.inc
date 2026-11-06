//===- X86LegacyWatchpointTests.inc - Persistent data and I/O causes ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise restart, device progress and software-entry cause preservation.
//===----------------------------------------------------------------------===//

class X86LegacyWatchpointTest : public X86LegacyDebugTest {
protected:
  void SetUp() override {
    X86LegacyDebugTest::SetUp();
    O.Debug = X86LegacyDebugProfile::Watchpoints;
    O.RepeatDebug.emplace();
    S.EFlags = 0x202;
    S.Core.GPR[7] = 0x4000;
    put(0x4000, 35, GuestScalarWidth::I32);
    S.Debug->DR7 = 0x30401;
    S.Debug->DR[0] = 0x4000;
  }
};

TEST_F(X86LegacyWatchpointTest, RepeatGroupsRetainCausesAcrossHostStops) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    SCOPED_TRACE(Case);
    SetUp();
    O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
    S.Core.GPR[1] = 3;
    S.Core.GPR[6] = 0x4000;
    S.Core.GPR[7] = 0x9000;
    S.EFlags = Case == 3 ? 0x302 : 0x202;
    std::vector<X86RepeatDebugStep> Steps;
    O.RepeatDebug->Context = &Steps;
    O.RepeatDebug->EndGroup = [](void *Context,
                                 const X86RepeatDebugStep &Step) {
      static_cast<std::vector<X86RepeatDebugStep> *>(Context)->push_back(Step);
      return false;
    };
    auto R = run({0xf3, 0xa4});
    EXPECT_EQ(S.Core.GPR[1], 2u);
    if (Case == 3) {
      EXPECT_EQ(R.DeliveredVector, 1);
      EXPECT_TRUE(Steps.empty());
      EXPECT_EQ(S.Debug->DR6, 0xffff4ff1u);
      continue;
    }
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::RepeatContinue);
    ASSERT_EQ(Steps.size(), 1u);
    EXPECT_EQ(Steps[0].PC, 0x1000u);
    EXPECT_EQ(Steps[0].Count, 2u);
    EXPECT_EQ(Steps[0].Source, 0x4001u);
    EXPECT_EQ(Steps[0].Destination, 0x9001u);
    EXPECT_EQ(Steps[0].AddressBytes, 4);
    EXPECT_EQ(Steps[0].ElementBytes, 1);
    EXPECT_EQ(S.Debug->PendingData, 1);
    EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
    if (Case == 1 || Case == 2) {
      put(0x11010, Case == 1 ? 0x40007 : 0, GuestScalarWidth::I32);
      R = run({0xf3, 0xa4});
      EXPECT_EQ(Steps.size(), 1u);
      EXPECT_EQ(S.Core.GPR[1], 2u);
      if (Case == 2) {
        EXPECT_EQ(R.DeliveredVector, 14);
        EXPECT_EQ(S.Debug->PendingData, 0);
        EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
        continue;
      }
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
      EXPECT_EQ(S.Debug->PendingData, 1);
      put(0x11010, 0x4007, GuestScalarWidth::I32);
    }
    EXPECT_EQ(run({0xf3, 0xa4}).Status,
              X86LegacyExecutionStatus::RepeatContinue);
    EXPECT_EQ(S.Debug->PendingData, 1);
    R = run({0xf3, 0xa4});
    EXPECT_EQ(R.DeliveredVector, 1);
    EXPECT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(Steps.size(), 2u);
    EXPECT_EQ(S.Core.GPR[1], 0u);
    EXPECT_EQ(S.Core.GPR[6], 0x4003u);
    EXPECT_EQ(S.Core.GPR[7], 0x9003u);
    EXPECT_EQ(S.Debug->PendingData, 0);
    EXPECT_EQ(S.Debug->DR6, 0xffff0ff1u);
  }
}

TEST_F(X86LegacyWatchpointTest, RepeatRequiresGroupingBeforeOperandEffects) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    SetUp();
    O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
    S.Core.GPR[1] = Case & 1 ? 0 : 3;
    if (Case < 2)
      O.RepeatDebug.reset();
    else
      O.RepeatDebug->EndGroup = nullptr;
    auto R = executeX86LegacyInstruction(S, *M, {0xf3, 0xa4}, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("debug-group binding"),
              std::string::npos);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004), 0x1007u);
    EXPECT_EQ(S.Debug->PendingData, 0);
  }
}

TEST_F(X86LegacyWatchpointTest, StackShadowSurvivesBackingButNotFaults) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SetUp();
    ASSERT_EQ(run({0x8e, 0x17}).Status, X86LegacyExecutionStatus::Completed);
    ASSERT_TRUE(S.SSBlocked);
    EXPECT_EQ(S.Debug->PendingData, 1);
    EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
    if (Case == 0) {
      put(0x11004, 0x40007, GuestScalarWidth::I32);
      EXPECT_EQ(run({0x90}).Status, X86LegacyExecutionStatus::BackingFault);
      EXPECT_EQ(S.Debug->PendingData, 1);
      put(0x11004, 0x1007, GuestScalarWidth::I32);
    }
    const auto R = Case == 1 ? run({0x0f, 0x0b}) : run({0x90});
    EXPECT_EQ(R.DeliveredVector, Case == 1 ? 6 : 1);
    EXPECT_EQ(S.Debug->PendingData, 0);
    EXPECT_EQ(S.Debug->DR6, Case == 1 ? 0xffff0ff0u : 0xffff0ff1u);
    EXPECT_FALSE(S.SSBlocked);
  }
}

TEST_F(X86LegacyWatchpointTest,
       SoftwareEntryAndDataTrapHaveSeparateContinuations) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SetUp();
    if (Case == 0) {
      ASSERT_EQ(run({0x8e, 0x17}).Status, X86LegacyExecutionStatus::Completed);
      put(0x11018, 0x40007, GuestScalarWidth::I32); // IDT backing.
    } else {
      S.Debug->DR[0] = 0x6200; // INT 64 IDT entry; #DB uses another entry.
      put(0x1101c, 0x40007,
          GuestScalarWidth::I32); // GDT, after comparator hit.
    }
    const auto R = run({0xcd, 64});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    ASSERT_TRUE(S.PendingEvent);
    EXPECT_EQ(S.PendingEvent->DataBreakpoints, Case ? 1 : 0);
    put(Case ? 0x1101c : 0x11018, Case ? 0x7007 : 0x6007,
        GuestScalarWidth::I32);
    if (Case == 2)
      put(0x6008, 0x2000 | (uint64_t{8} << 16) | (uint64_t{0x6e} << 40));
    auto Got = resumeX86LegacyEvent(S, *M, O);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    EXPECT_TRUE(Got->SoftwareCompleted);
    EXPECT_EQ(Got->DeliveredVector, Case == 2 ? 11 : 1);
    EXPECT_EQ(S.Debug->PendingData, 0);
    EXPECT_EQ(S.Debug->DR6, Case ? 0xffff0ff1u : 0xffff0ff0u);
    EXPECT_FALSE(S.PendingEvent);
    EXPECT_EQ(get(S.Core.GPR[4] + (Case == 2 ? 4 : 0)), 0x2000u);
  }
}

TEST_F(X86LegacyWatchpointTest,
       FaultingReadModifyWriteDoesNotPublishDataCause) {
  S.Core.GPR[7] = 0x8fff;
  S.Debug->DR[0] = 0x8fff;
  put(0x11024, 0, GuestScalarWidth::I32);
  const auto R = run({0xff, 0x07});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
  EXPECT_EQ(S.Debug->PendingData, 0);
}

TEST_F(X86LegacyWatchpointTest,
       PortTrapUsesCompletedElementAndDoesNotWrapAt16Bits) {
  for (bool Input : {false, true})
    for (unsigned Case = 0; Case != 6; ++Case) {
      SetUp();
      S.EFlags = 0x3202;
      auto Controls = M->controls();
      Controls.CR4 |= 8;
      ASSERT_FALSE(bool(M->bindControls(Controls)));
      S.Core.GPR[2] = Case == 4 ? 0xffff : 0x2ff;
      S.Core.GPR[6] = S.Core.GPR[7] = 0x4000;
      S.Core.GPR[1] = Case == 2 ? 0 : 3;
      S.Debug->DR7 = 0x20401;
      S.Debug->DR[0] = Case == 4 ? 0x10000 : 0x300;
      PagedPortTape Tape;
      Tape.Ready = Case != 3;
      O.PortIO = X86PortIOBinding{X86PortIOProfile::InputAfterWriteChecks,
                                  PagedPortTape::transfer, &Tape};
      const std::vector<uint8_t> Code =
          Case == 0 || Case == 4
              ? std::vector<uint8_t>{uint8_t(Input ? 0xed : 0xef)}
              : std::vector<uint8_t>{0xf3, uint8_t(Input ? 0x6d : 0x6f)};
      if (Case == 5)
        put(0x11010, 0, GuestScalarWidth::I32);
      const auto R = run(Code);
      if (Case == 2 || Case == 3 || Case == 5) {
        EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
        EXPECT_TRUE(Tape.Transfers.empty());
        EXPECT_EQ(R.Status, Case == 2 ? X86LegacyExecutionStatus::Completed
                            : Case == 3
                                ? X86LegacyExecutionStatus::DeviceUnavailable
                                : X86LegacyExecutionStatus::EventDelivered);
        if (Case == 5)
          EXPECT_EQ(R.DeliveredVector, 14);
      } else {
        ASSERT_EQ(R.DeliveredVector, 1);
        EXPECT_EQ(S.Debug->DR6, 0xffff0ff1u);
        ASSERT_EQ(Tape.Transfers.size(), 1u);
        EXPECT_TRUE(R.DeviceTransferCompleted);
        EXPECT_EQ(S.Core.GPR[1], Case == 1 ? 2u : 3u);
        EXPECT_EQ(R.InstructionCompleted, Case != 1);
        EXPECT_EQ(get(S.Core.GPR[4]), Case == 1 ? 0x1000u : 0x1001u);
      }
    }
}

TEST_F(X86LegacyWatchpointTest, InvalidComparatorControlRefusesBeforeFetch) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    SetUp();
    if (Case == 0)
      S.Debug->DR7 = 0x20401; // I/O without CR4.DE.
    if (Case == 1)
      S.Debug->DR7 = 0x40401; // Noncanonical execution length.
    if (Case == 2)
      S.Debug->PendingData = 0x80;
    if (Case == 3)
      S.Debug->DR7 |= 0x800; // RTM.
    auto R = executeX86LegacyInstruction(S, *M, {0x90}, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x11004), 0x1007u);
    EXPECT_FALSE(S.PendingEvent);
  }
}

TEST_F(X86LegacyWatchpointTest, EventBudgetRetainsSoftwareAndSecondaryFaults) {
  for (bool Fault : {false, true}) {
    SCOPED_TRACE(Fault);
    const auto Setup = [&] {
      SetUp();
      O.EventDelivery = {};
      S.Debug->DR[0] = 0x6200;
      if (Fault) {
        S.Debug->DR7 = 0x400;
        put(0x6200, 0x2000 | (uint64_t{8} << 16) | (uint64_t{0x6e} << 40));
        put(0x6058, 0x2000 | (uint64_t{8} << 16) | (uint64_t{0x0e} << 40));
      }
    };
    Setup();
    const auto Complete = run({0xcd, 64});
    ASSERT_EQ(Complete.Status, X86LegacyExecutionStatus::EventDelivered);
    ASSERT_EQ(Complete.DeliveredVector, Fault ? 8 : 1);
    const auto Expected = S;
    const auto ExpectedRAM = M->physicalMemory().snapshotMemoryRegions();
    Setup();
    O.EventDelivery.MaxAttempts = 1;
    auto R = run({0xcd, 64});
    EXPECT_TRUE(R.EventAccepted);
    EXPECT_EQ(R.InstructionCompleted, !Fault);
    const unsigned Attempts = Fault ? 3 : 2;
    for (unsigned Attempt = 0; Attempt != Attempts; ++Attempt) {
      EXPECT_EQ(R.DeliveryAttempts, 1u);
      EXPECT_EQ(R.SoftwareCompleted, !Fault);
      if (Attempt + 1 == Attempts) {
        EXPECT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_FALSE(S.PendingEvent);
        break;
      }
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventBudgetExhausted);
      ASSERT_TRUE(S.PendingEvent);
      auto Next = resumeX86LegacyEvent(S, *M, O);
      ASSERT_TRUE(bool(Next)) << llvm::toString(Next.takeError());
      R = *Next;
      EXPECT_FALSE(R.EventAccepted);
      EXPECT_FALSE(R.InstructionAttempted);
    }
    EXPECT_EQ(S.EIP, Expected.EIP);
    EXPECT_EQ(S.EFlags, Expected.EFlags);
    EXPECT_EQ(S.Core.GPR, Expected.Core.GPR);
    EXPECT_EQ(S.Core.CPL, Expected.Core.CPL);
    EXPECT_EQ(S.Debug->DR6, Expected.Debug->DR6);
    EXPECT_EQ(S.Debug->DR7, Expected.Debug->DR7);
    EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
              ExpectedRAM.front().Bytes);
  }
}

TEST_F(X86LegacyWatchpointTest, ZeroEventWorkRefusesBeforeFetch) {
  const auto Original = S;
  const auto OriginalRAM = M->physicalMemory().snapshotMemoryRegions();
  O.EventDelivery.MaxAttempts = 0;
  auto R = executeX86LegacyInstruction(S, *M, {0xcd, 64}, O);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(S.EIP, Original.EIP);
  EXPECT_EQ(S.EFlags, Original.EFlags);
  EXPECT_EQ(S.Core.GPR, Original.Core.GPR);
  EXPECT_EQ(S.Debug->DR6, Original.Debug->DR6);
  EXPECT_FALSE(S.PendingEvent);
  EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
            OriginalRAM.front().Bytes);
}
