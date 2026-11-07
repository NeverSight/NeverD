//===- X86LegacyPortIOTests.inc - Protected device boundaries -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by the legacy execution fixture: device effects, faults and resume.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, PortAdmissionAndFetchPrecedeDeviceEffects) {
  PagedPortTape Tape;
  for (unsigned Bad = 0; Bad != 3; ++Bad) {
    O.PortIO = X86PortIOBinding{X86PortIOProfile::InputBeforeWrite,
                                PagedPortTape::transfer, &Tape};
    if (!Bad)
      O.PortIO.reset();
    else if (Bad == 1)
      O.PortIO->Transfer = nullptr;
    else
      O.PortIO->Profile = static_cast<X86PortIOProfile>(255);
    auto R = executeX86LegacyInstruction(S, *M, {0xec}, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x10000), 0x11007u);
  }
  O.PortIO = X86PortIOBinding{X86PortIOProfile::InputBeforeWrite,
                              PagedPortTape::transfer, &Tape};
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0xf2, 0x6c},
                                         {0xf3, 0xed},
                                         {0xf3, 0xf3, 0x6c},
                                         {0x66, 0x66, 0xed},
                                         {0xe4},
                                         {0xec, 0},
                                         {0x64, 0x65, 0x6f}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  EXPECT_EQ(
      llvm::cantFail(executeX86LegacyInstruction(S, *M, {0xec}, O)).Status,
      X86LegacyExecutionStatus::CodeChanged);
  S.EFlags = 0x3202;
  S.Core.GPR[1] = 0;
  EXPECT_EQ(run({0xf0, 0x6c}).DeliveredVector, 6);
  EXPECT_TRUE(Tape.Transfers.empty());
}

TEST_F(X86LegacyExecutionTest, PortPermissionsPrecedeZeroCountAndDoNotWrap) {
  PagedPortTape Tape;
  O.PortIO = X86PortIOBinding{X86PortIOProfile::InputBeforeWrite,
                              PagedPortTape::transfer, &Tape};
  for (unsigned Width : {1, 2, 4})
    for (unsigned Case = 0; Case != 6; ++Case) {
      SCOPED_TRACE(testing::Message() << "width=" << Width << " case=" << Case);
      SetUp();
      S.TR.Limit = 0x3000;
      put(0x5866, 0x100, GuestScalarWidth::I16);
      S.Core.GPR[1] = 0xabcd0000;
      S.Core.GPR[2] = 0x1234ffff;
      S.Core.Segments[0].Unusable = true;
      put(0x5900 + 8192, 0xff, GuestScalarWidth::I8);
      if (Case == 1) {
        S.TR.Attributes = 0x83;
        put(0x5802, 0xe000, GuestScalarWidth::I16);
        put(0x5804, 16, GuestScalarWidth::I16);
      }
      if (Case == 2)
        S.EFlags |= 0x3000;
      if (Case == 3)
        S.TR.Limit = 0x66;
      if (Case == 4)
        put(0x5866, 0xffff, GuestScalarWidth::I16);
      if (Case == 5) {
        S.TR.Limit = 0x6000;
        put(0x5866, 0x3000, GuestScalarWidth::I16);
        put(0x11028, 0xa006, GuestScalarWidth::I32);
      }
      std::vector<uint8_t> Bytes{0xf3, 0x67};
      if (Width == 2)
        Bytes.push_back(0x66);
      Bytes.push_back(Width == 1 ? 0x6c : 0x6d);
      auto R = run(Bytes);
      const bool Completed = Case == 2 || (!Case && Width == 1);
      EXPECT_EQ(R.InstructionCompleted, Completed);
      if (!Completed)
        EXPECT_EQ(R.DeliveredVector, Case == 5 ? 14 : 13);
      if (Completed)
        EXPECT_EQ(S.Core.GPR[1], 0xabcd0000u);
      EXPECT_TRUE(Tape.Transfers.empty());
    }
}

TEST_F(X86LegacyExecutionTest, PortInputFaultProfilesRetainOnlyBoundEffects) {
  for (auto Profile : {X86PortIOProfile::InputBeforeWrite,
                       X86PortIOProfile::InputAfterWriteChecks,
                       X86PortIOProfile::InputBeforeWriteFragments})
    for (unsigned Case = 0; Case != 5; ++Case) {
      SCOPED_TRACE(testing::Message()
                   << "profile=" << unsigned(Profile) << " case=" << Case);
      SetUp();
      PagedPortTape Tape;
      Tape.Ready = Case != 2;
      Tape.Fail = Case == 4;
      O.PortIO = X86PortIOBinding{Profile, PagedPortTape::transfer, &Tape};
      S.EFlags = 0x3202;
      S.Core.GPR[1] = 1;
      S.Core.GPR[2] = 0x123402f9;
      S.Core.GPR[7] = 0xefff;
      if (Case != 3)
        put(0x1103c, Case == 1 ? 0x20007 : 0xf006, GuestScalarWidth::I32);
      put(0x1000, 0x6df3, GuestScalarWidth::I16);
      const bool After = Profile == X86PortIOProfile::InputAfterWriteChecks;
      auto R = executeX86LegacyInstruction(S, *M, {0xf3, 0x6d}, O);
      if (Case == 4 && !After) {
        ASSERT_FALSE(bool(R));
        llvm::consumeError(R.takeError());
      } else {
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->DeviceTransferCompleted, Case == 3 || (!Case && !After));
        EXPECT_EQ(R->InstructionCompleted, Case == 3);
        if (Case == 1)
          EXPECT_EQ(R->Status, X86LegacyExecutionStatus::BackingFault);
        if (Case == 2 && !After)
          EXPECT_EQ(R->Status, X86LegacyExecutionStatus::DeviceUnavailable);
        const unsigned Written =
            Case == 3 ? 4
            : !Case && Profile == X86PortIOProfile::InputBeforeWriteFragments
                ? 1
                : 0;
        EXPECT_EQ(R->PortInputBytesWritten, Written);
        EXPECT_EQ(get(0xefff, GuestScalarWidth::I8), Written ? 0x21u : 0u);
        EXPECT_EQ(S.Core.GPR[1], Case == 3 ? 0u : 1u);
        EXPECT_EQ(S.Core.GPR[7], Case == 3 ? 0xf003u : 0xefffu);
      }
    }
}

TEST_F(X86LegacyExecutionTest, PortDeviceWaitPreservesProgressAndShadows) {
  PagedPortTape Tape;
  Tape.Ready = false;
  O.PortIO = X86PortIOBinding{X86PortIOProfile::InputBeforeWrite,
                              PagedPortTape::transfer, &Tape};
  S.EFlags = 0x13202;
  S.SSBlocked = S.STIBlocked = true;
  S.Core.GPR[1] = 0xabcd0002;
  S.Core.GPR[7] = 0x1234ffff;
  S.Core.GPR[2] = 0x567802f9;
  auto R = run({0xf3, 0x67, 0x6c});
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::DeviceUnavailable);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(S.EFlags, 0x13202u);
  EXPECT_TRUE(S.SSBlocked && S.STIBlocked);
  Tape.Ready = true;
  EXPECT_EQ(run({0xf3, 0x67, 0x6c}).Status,
            X86LegacyExecutionStatus::RepeatContinue);
  EXPECT_EQ(S.Core.GPR[7], 0x12340000u);
  EXPECT_EQ(S.Core.GPR[1], 0xabcd0001u);
  EXPECT_FALSE(S.SSBlocked || S.STIBlocked);
  EXPECT_TRUE(run({0xf3, 0x67, 0x6c}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[1], 0xabcd0000u);
  EXPECT_EQ(S.Core.GPR[7], 0x12340001u);
  ASSERT_EQ(Tape.Transfers.size(), 2u);
  EXPECT_EQ(Tape.Transfers.front().Port, 0x2f9);
}

TEST_F(X86LegacyExecutionTest, PortConsumedInputSurvivesDeliveryBackingStop) {
  for (bool Recursive : {false, true}) {
    SetUp();
    PagedPortTape Tape;
    O.PortIO = X86PortIOBinding{X86PortIOProfile::InputBeforeWrite,
                                PagedPortTape::transfer, &Tape};
    S.EFlags = 0x3202;
    S.Core.GPR[7] = 0xf000;
    put(0x1103c, 0xf006, GuestScalarWidth::I32);
    if (Recursive) {
      S.IDTRBase = 0x9fa0;
      put(0x9fe0, 0x2000 | (uint64_t{8} << 16) | (uint64_t{0xee} << 40));
      put(0x11024, 0x30007, GuestScalarWidth::I32);
    } else {
      put(0x11018, 0x30007, GuestScalarWidth::I32);
    }
    auto R = run({0x6c});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    ASSERT_TRUE(S.PendingEvent);
    EXPECT_TRUE(R.DeviceTransferCompleted);
    EXPECT_EQ(S.PendingEvent->Event.Vector, Recursive ? 8 : 14);
    auto Refused = executeX86LegacyInstruction(S, *M, {0x6c}, O);
    ASSERT_FALSE(bool(Refused));
    llvm::consumeError(Refused.takeError());
    put(Recursive ? 0x11024 : 0x11018, Recursive ? 0x9007 : 0x6007,
        GuestScalarWidth::I32);
    R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
    EXPECT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_EQ(R.DeliveredVector, Recursive ? 8 : 14);
    EXPECT_FALSE(R.DeviceTransferCompleted);
    EXPECT_FALSE(R.EventAccepted);
    EXPECT_FALSE(S.PendingEvent);
    ASSERT_EQ(Tape.Transfers.size(), 1u);
    EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
    EXPECT_EQ(get(S.Core.GPR[4] + 12), 0x13202u);
    if (!Recursive) {
      put(0x1103c, 0xf007, GuestScalarWidth::I32);
      EXPECT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
      EXPECT_TRUE(run({0xcf}).InstructionCompleted);
      EXPECT_EQ(S.EIP, 0x1000u);
      EXPECT_TRUE(run({0x6c}).InstructionCompleted);
      EXPECT_EQ(Tape.Transfers.size(), 2u);
      EXPECT_EQ(get(0xf000, GuestScalarWidth::I8), 0x44u);
    }
  }
}

TEST_F(X86LegacyExecutionTest, AcceptedNMIResumesDespiteItsOwnInhibition) {
  put(0x11018, 0x30007, GuestScalarWidth::I32);
  auto R = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingEvent);
  EXPECT_TRUE(S.NMIBlocked);
  auto Again = deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O);
  ASSERT_FALSE(bool(Again));
  llvm::consumeError(Again.takeError());
  put(0x11018, 0x6007, GuestScalarWidth::I32);
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 2);
  EXPECT_FALSE(R.EventAccepted);
  EXPECT_TRUE(S.NMIBlocked);
}

TEST_F(X86LegacyExecutionTest,
       PortAlignmentPriorityRetainsArchitecturalFaults) {
  for (bool Input : {false, true})
    for (auto Order : {X86AlignmentPriority::BeforePaging,
                       X86AlignmentPriority::AfterPaging})
      for (auto Profile : {X86PortIOProfile::InputBeforeWrite,
                           X86PortIOProfile::InputAfterWriteChecks}) {
        SetUp();
        PagedPortTape Tape;
        O.PortIO = X86PortIOBinding{Profile, PagedPortTape::transfer, &Tape};
        O.Segments.AlignmentPriority = Order;
        S.EFlags = 0x43202;
        auto Controls = M->controls();
        Controls.CR0 |= 0x40000;
        llvm::cantFail(M->bindControls(Controls));
        S.Core.GPR[6] = S.Core.GPR[7] = 0xe101;
        put(0x11038, 0xe006, GuestScalarWidth::I32);
        const auto R = run({uint8_t(Input ? 0x6d : 0x6f)});
        EXPECT_EQ(R.DeliveredVector,
                  Order == X86AlignmentPriority::BeforePaging ? 17 : 14);
        EXPECT_EQ(R.DeviceTransferCompleted,
                  Input && Profile == X86PortIOProfile::InputBeforeWrite);
        EXPECT_EQ(S.Core.GPR[6], 0xe101u);
        EXPECT_EQ(S.Core.GPR[7], 0xe101u);
        EXPECT_EQ(S.CR2, Order == X86AlignmentPriority::BeforePaging
                             ? 0x12345678u
                             : 0xe101u);
      }
}

TEST_F(X86LegacyExecutionTest, PortBitmapImplicitReadsWrapAtThirtyTwoBits) {
  // Both the base field itself and base+bitmap+port arithmetic wrap linearly.
  put(0x10ffc, 0x12003, GuestScalarWidth::I32);
  put(0x12ffc, 0x15003, GuestScalarWidth::I32);
  put(0x11000, 0x18003, GuestScalarWidth::I32);
  put(0x15fff, 0, GuestScalarWidth::I8);
  put(0x18000, 1, GuestScalarWidth::I8);
  auto R = llvm::cantFail(
      checkX86PortBitmap(*M, 0xffffff99, 0x3000, 0x2f9, 4, true));
  EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
  // ffffff99 + 100 + (2f9 >> 3) == f8.
  put(0x180f8, 4, GuestScalarWidth::I8);
  R = llvm::cantFail(
      checkX86PortBitmap(*M, 0xffffff99, 0x3000, 0x2f9, 4, true));
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 13);
  EXPECT_EQ(get(0x12ffc) & 0x60, 0x20u);
  EXPECT_EQ(get(0x11000) & 0x60, 0x20u);
}

TEST_F(X86LegacyExecutionTest, PortElementsResumeAfterIRQAndNMIHandlers) {
  for (bool Input : {false, true})
    for (auto Kind : {X86LegacyEventKind::Interrupt, X86LegacyEventKind::NMI}) {
      SetUp();
      PagedPortTape Tape;
      O.PortIO = X86PortIOBinding{X86PortIOProfile::InputBeforeWrite,
                                  PagedPortTape::transfer, &Tape};
      S.EFlags = 0x3202;
      S.SSBlocked = true;
      S.Core.GPR[1] = 0xabcd0003;
      S.Core.GPR[6] = 0x1234c100;
      S.Core.GPR[7] = 0x5678e100;
      const std::vector<uint8_t> Bytes{0xf3, 0x67,
                                       uint8_t(Input ? 0x6d : 0x6f)};
      EXPECT_EQ(run(Bytes).Status, X86LegacyExecutionStatus::RepeatContinue);
      EXPECT_EQ(S.Core.GPR[1], 0xabcd0002u);
      EXPECT_EQ(Tape.Transfers.size(), 1u);
      const uint8_t Vector = Kind == X86LegacyEventKind::NMI ? 2 : 64;
      auto R = llvm::cantFail(
          deliverX86LegacyEvent(S, *M, {S.EIP, 0, 0, Vector, Kind}, O));
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
      EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x13202u);
      EXPECT_TRUE(run({0xcf}).InstructionCompleted);
      EXPECT_FALSE(S.NMIBlocked);
      EXPECT_EQ(run(Bytes).Status, X86LegacyExecutionStatus::RepeatContinue);
      EXPECT_TRUE(run(Bytes).InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[1], 0xabcd0000u);
      EXPECT_EQ(Tape.Transfers.size(), 3u);
      EXPECT_EQ(S.Core.GPR[6], Input ? 0x1234c100u : 0x1234c10cu);
      EXPECT_EQ(S.Core.GPR[7], Input ? 0x5678e10cu : 0x5678e100u);
      if (Input) {
        EXPECT_EQ(get(0xe100), 0x87654321u);
        EXPECT_EQ(get(0xe108), 0x87654567u);
      }
    }
}
