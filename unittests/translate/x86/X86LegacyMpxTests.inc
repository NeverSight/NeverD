//===- X86LegacyMpxTests.inc - Protected MPX execution contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bounds values, original branch bytes and fault/replay states.
//===----------------------------------------------------------------------===//

class X86LegacyMpxTest : public X86LegacyExecutionTest {};

TEST_F(X86LegacyMpxTest, MakeAndRegisterMovesKeepWidthAndIgnoreSegments) {
  for (unsigned Bank = 0; Bank != 4; ++Bank)
    for (unsigned Scale = 0; Scale != 4; ++Scale) {
      resetLegacyState();
      bindMpx();
      S.Core.GPR[0] = 0xfffffff0;
      S.Core.GPR[1] = 17;
      S.Core.Segments[4].Unusable = true;
      const auto Flags = S.EFlags;
      ASSERT_TRUE(run({0x64, 0xf3, 0x0f, 0x1b, uint8_t(0x44 | Bank << 3),
                       uint8_t(Scale << 6 | 8), 0x7f})
                      .InstructionCompleted);
      const uint32_t Upper = ~(0xfffffff0u + (17u << Scale) + 127u);
      EXPECT_EQ(*S.Mpx->bounds(Bank),
                (X86MpxBounds{{0xfffffff0, UINT32_MAX}, {Upper, UINT32_MAX}}));
      EXPECT_EQ(S.EFlags, Flags & ~0x10000u);
      EXPECT_EQ(S.Mpx->status(), 0x12345678u);
      EXPECT_EQ(get(0x1103c) & 0x20, 0u); // No operand address is accessed.
      for (unsigned Other = 0; Other != 4; ++Other) {
        ASSERT_TRUE(run({0x66, 0x0f, 0x1a, uint8_t(0xc0 | Other << 3 | Bank)})
                        .InstructionCompleted);
        EXPECT_EQ(S.Mpx->bounds(Other), S.Mpx->bounds(Bank));
      }
    }
}

TEST_F(X86LegacyMpxTest, EveryCheckUsesUnsignedBoundsAndPublishesBRStatus) {
  for (unsigned Bank = 0; Bank != 4; ++Bank)
    for (unsigned Reg = 0; Reg != 8; ++Reg)
      for (unsigned Kind = 0; Kind != 3; ++Kind)
        for (uint32_t Value : {0u, 9u, 10u, 20u, 21u, UINT32_MAX}) {
          resetLegacyState();
          bindMpx();
          ASSERT_TRUE(S.Mpx->setBounds(
              Bank, {{10, UINT64_MAX}, {Kind == 2 ? 20u : ~20u, UINT64_MAX}}));
          S.Core.GPR[Reg] = Value;
          const bool Fault = Kind == 0 ? Value < 10 : Value > 20;
          const auto Before = S.Mpx->bounds(Bank);
          auto R = run({uint8_t(Kind == 0 ? 0xf3 : 0xf2), 0x0f,
                        uint8_t(Kind == 2 ? 0x1b : 0x1a),
                        uint8_t(0xc0 | Bank << 3 | Reg)});
          EXPECT_EQ(R.InstructionCompleted, !Fault);
          EXPECT_EQ(R.DeliveredVector, Fault ? 5u : 0u);
          EXPECT_EQ(S.Mpx->status(), Fault ? 1u : 0x12345678u);
          EXPECT_EQ(S.Mpx->bounds(Bank), Before);
          if (Fault) {
            EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u); // #BR has no error slot.
            EXPECT_FALSE(S.PendingMpx);
          }
        }
}

TEST_F(X86LegacyMpxTest, MovesUseOriginalSegmentsAndWholeEightByteOperands) {
  const uint8_t Prefix[]{0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65};
  for (unsigned Segment = 0; Segment != 6; ++Segment)
    for (unsigned Bank = 0; Bank != 4; ++Bank) {
      resetLegacyState();
      bindMpx();
      // Keep CS's fetch base unchanged; other segment bases must participate.
      const uint32_t Base = Segment == 1 ? 0 : 0x100;
      S.Core.Segments[Segment].Base = Base;
      S.Core.GPR[0] = 0xc100;
      put(0xc100 + Base, 0x8765432112345678ULL);
      ASSERT_TRUE(run({Prefix[Segment], 0x66, 0x0f, 0x1a, uint8_t(Bank << 3)})
                      .InstructionCompleted);
      EXPECT_EQ(*S.Mpx->bounds(Bank), (X86MpxBounds{{0x12345678, UINT32_MAX},
                                                    {0x87654321, UINT32_MAX}}));
      S.Core.GPR[0] += 8;
      const auto R =
          run({Prefix[Segment], 0x66, 0x0f, 0x1b, uint8_t(Bank << 3)});
      if (Segment == 1) {
        EXPECT_EQ(R.DeliveredVector, 13);
      } else {
        EXPECT_TRUE(R.InstructionCompleted);
        EXPECT_EQ(get(0xc108 + Base, GuestScalarWidth::I64),
                  0x8765432112345678ULL);
      }
    }
}

TEST_F(X86LegacyMpxTest, TablesUseSegmentedBaseUnscaledIndexAndThreeFields) {
  for (unsigned Bank = 0; Bank != 4; ++Bank)
    for (unsigned Scale = 0; Scale != 4; ++Scale) {
      resetLegacyState();
      bindMpx();
      S.Core.GPR[0] = 0x3004;
      S.Core.GPR[1] = 0x12345678;
      S.Core.Segments[4].Base = 0x1000;
      S.Core.Segments[4].Unusable = true; // MIB computes, never probes FS.
      put(0x8010, 0xa001, GuestScalarWidth::I32);
      put(0xa01c, 0xdeadbeef, GuestScalarWidth::I32);
      const std::vector<uint8_t> Store{0x64, 0x0f, 0x1b, uint8_t(Bank << 3 | 4),
                                       uint8_t(Scale << 6 | 8)};
      auto Load = Store;
      Load[2] = 0x1a;
      ASSERT_TRUE(run(Store).InstructionCompleted);
      EXPECT_EQ(get(0xa010), 0x100u + Bank);
      EXPECT_EQ(get(0xa014), 0xfffff000u + Bank);
      EXPECT_EQ(get(0xa018), 0x12345678u);
      EXPECT_EQ(get(0xa01c), 0xdeadbeefu);
      ASSERT_TRUE(S.Mpx->setBounds(Bank, {}));
      ASSERT_TRUE(run(Load).InstructionCompleted);
      EXPECT_EQ(*S.Mpx->bounds(Bank),
                (X86MpxBounds{{0x100u + Bank, UINT32_MAX},
                              {0xfffff000u + Bank, UINT32_MAX}}));
      ++S.Core.GPR[1];
      ASSERT_TRUE(run(Load).InstructionCompleted);
      EXPECT_EQ(*S.Mpx->bounds(Bank),
                (X86MpxBounds{{0, UINT32_MAX}, {0, UINT32_MAX}}));
      EXPECT_EQ(get(0x11010) & 0x20, 0u);
      EXPECT_EQ(S.Mpx->status(), 0x12345678u);
    }
}

TEST_F(X86LegacyMpxTest, FaultsRetainOnlyArchitecturallyQualifiedState) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    resetLegacyState();
    bindMpx();
    const auto Before = S.Mpx->bounds(0);
    S.Core.GPR[0] = 0;
    put(0x8000, 0xaffd, GuestScalarWidth::I32);
    put(0xaffc, 0xaabbccdd, GuestScalarWidth::I32);
    if (Case == 0)
      put(0x11020, 0x8006, GuestScalarWidth::I32); // BDE #PF.
    else if (Case <= 2)
      put(0x1102c, 0xb006, GuestScalarWidth::I32); // BTE #PF.
    else {
      put(0x8000, 0, GuestScalarWidth::I32); // Invalid BDE #BR.
      if (Case == 4)
        S.IDTRLimit = 0; // Status survives failed delivery and shutdown.
      if (Case == 5)
        O.EventDelivery.MaxAttempts = 1;
    }
    const auto R = run({0x0f, uint8_t(Case == 2 ? 0x1b : 0x1a), 0});
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_FALSE(S.PendingMpx);
    EXPECT_EQ(S.Mpx->status(), Case >= 3 ? 0x8002u : 0x12345678u);
    EXPECT_EQ(S.Mpx->bounds(0), Case == 1 ? X86MpxBounds{} : *Before);
    EXPECT_EQ(get(0xaffc), 0xaabbccddu); // No partial BNDSTX store.
    if (Case < 3)
      EXPECT_EQ(S.CR2, Case == 0 ? 0x8000u : 0xb000u);
    if (Case == 4)
      EXPECT_EQ(R.Status, X86LegacyExecutionStatus::Shutdown);
  }
}

TEST_F(X86LegacyMpxTest, BackingResumeRetainsAliasedDirectoryAndFetch) {
  for (bool Store : {false, true}) {
    resetLegacyState();
    bindMpx(0x10001);
    S.Core.GPR[0] = 0x421000;
    S.Core.GPR[1] = 0x1234;
    // BDE is the BTE page's PTE. Its A/D write changes the value already read.
    put(0x11084, 0x21007, GuestScalarWidth::I32);
    const std::vector<uint8_t> Bytes{0x0f, uint8_t(Store ? 0x1b : 0x1a), 4, 8};
    const auto Before = S.Mpx;
    auto R = run(Bytes);
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    ASSERT_TRUE(S.PendingMpx);
    ASSERT_TRUE(S.PendingMpx->Directory);
    EXPECT_EQ(S.PendingMpx->Directory->Value, 0x21007u);
    EXPECT_NE(get(0x11084), 0x21007u);
    EXPECT_EQ(S.Mpx, Before);
    EXPECT_TRUE(R.InstructionAttempted);
    auto Event = deliverX86LegacyEvent(
        S, *M, {S.EIP, 0, 0, 64, X86LegacyEventKind::Interrupt}, O);
    ASSERT_FALSE(bool(Event));
    llvm::consumeError(Event.takeError());
    auto Other = executeX86LegacyInstruction(S, *M, {0x90}, O);
    ASSERT_FALSE(bool(Other));
    llvm::consumeError(Other.takeError());
    const auto Paused = S;
    const auto OriginalControls = M->controls();
    auto ChangedControls = OriginalControls;
    ChangedControls.CR4 &= ~(uint64_t{1} << 18);
    ASSERT_FALSE(M->bindControls(ChangedControls));
    auto Changed = executeX86LegacyInstruction(S, *M, Bytes, O);
    EXPECT_FALSE(bool(Changed));
    if (!Changed)
      llvm::consumeError(Changed.takeError());
    S = Paused;
    ASSERT_FALSE(M->bindControls(OriginalControls));
    repairMpxBacking();
    put(0x21004, 0x2222222211111111ULL);
    put(0x2100c, 0x1234, GuestScalarWidth::I32);
    put(0x21010, 0xdeadbeef, GuestScalarWidth::I32);
    put(0x1000, 0xcc, GuestScalarWidth::I8); // Already authenticated.
    R = llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
    ASSERT_TRUE(R.InstructionCompleted);
    EXPECT_FALSE(R.InstructionAttempted);
    EXPECT_FALSE(S.PendingMpx);
    EXPECT_EQ(S.EIP, 0x1004u);
    if (Store)
      EXPECT_EQ(get(0x21004, GuestScalarWidth::I64), 0xfffff00000000100ULL);
    else
      EXPECT_EQ(*S.Mpx->bounds(0), (X86MpxBounds{{0x11111111, UINT32_MAX},
                                                 {0x22222222, UINT32_MAX}}));
    EXPECT_EQ(get(0x21010), 0xdeadbeefu);
    EXPECT_EQ(get(0x21024),
              0u); // Re-reading the changed BDE uses wrong storage.
  }
}

TEST_F(X86LegacyMpxTest, BranchesInitializeAtCompletionIncludingUntakenJcc) {
  for (bool Preserve : {false, true})
    for (uint8_t Prefix : {uint8_t{0}, uint8_t{0xf2}, uint8_t{0xf3}})
      for (bool Taken : {false, true})
        for (std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0x74, 8},
                                           {0x0f, 0x84, 8, 0, 0, 0},
                                           {0xe9, 8, 0, 0, 0},
                                           {0xe8, 8, 0, 0, 0},
                                           {0xc3},
                                           {0xff, 0xe0},
                                           {0xff, 0xd0},
                                           {0xeb, 8},
                                           {0xe2, 8}}) {
          resetLegacyState();
          bindMpx(Preserve ? 0x8003 : 0x8001);
          const bool Short = Bytes[0] == 0xeb || Bytes[0] == 0xe2;
          const auto Before = S.Mpx->bounds(0);
          if (Prefix)
            Bytes.insert(Bytes.begin(), Prefix);
          S.Core.GPR[0] = 0x1100;
          S.Core.GPR[1] = Taken ? 2 : 1;
          S.EFlags = Taken ? 0x242 : 0x202;
          put(0xd000, 0x1100, GuestScalarWidth::I32);
          ASSERT_TRUE(run(Bytes).InstructionCompleted);
          const bool Init = !Preserve && Prefix != 0xf2 && !Short;
          for (unsigned I = 0; I != 4; ++I)
            if (Init)
              EXPECT_EQ(*S.Mpx->bounds(I),
                        (X86MpxBounds{{0, UINT64_MAX}, {0, UINT64_MAX}}));
          if (!Init)
            EXPECT_EQ(S.Mpx->bounds(0), Before);
          EXPECT_EQ(S.Mpx->status(), 0x12345678u);
        }
}

TEST_F(X86LegacyMpxTest, FailedBranchesKeepBoundsAndDebugSeesCommittedBank) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    resetLegacyState();
    bindMpx();
    const auto Before = S.Mpx->bounds(0);
    if (Case == 0)
      put(0x11030, 0xc006, GuestScalarWidth::I32); // CALL stack #PF.
    if (Case == 1) {
      S.Core.Segments[1].Limit = 0x1005; // Target #GP after stack store.
      S.Core.Segments[1].Attributes &= ~0x8000;
    }
    if (Case == 2) {
      O.Debug = X86LegacyDebugProfile::InstructionStep;
      S.Debug.emplace();
      S.EFlags |= 0x100;
    }
    const auto R = run({0xe8, 8, 0, 0, 0});
    EXPECT_EQ(R.DeliveredVector, Case == 0 ? 14u : Case == 1 ? 13u : 1u);
    EXPECT_EQ(R.InstructionCompleted, Case == 2);
    EXPECT_EQ(
        S.Mpx->bounds(0),
        (Case == 2 ? X86MpxBounds{{0, UINT64_MAX}, {0, UINT64_MAX}} : *Before));
  }
}

TEST_F(X86LegacyMpxTest, AdmissionDisabledStateAndOriginalNopsAreDistinct) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x66, 0x0f, 0x1a, 0},
        {0x0f, 0x1a, 0},
        {0xe9, 0, 0, 0, 0}}) {
    resetLegacyState();
    bindMpx();
    S.Mpx.reset(); // CR4.OSXSAVE is set: missing state is not disabled.
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
    bindMpx(0x8000);
    O.MpxMemory.reset();
    S.Core.GPR[0] = 0xffffffff;
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
    EXPECT_EQ(S.Mpx->bounds(0)->Lower.Value, 0x100u);
  }
  resetLegacyState();
  auto Controls = M->controls();
  Controls.CR4 |= uint64_t{1} << 18;
  ASSERT_FALSE(M->bindControls(Controls));
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x0f, 0x1a, 0xc0},
        {0x0f, 0x1b, 0xff},
        {0xf3, 0x0f, 0x1b, 0xc0}})
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x66, 0x0f, 0x1a},
        {0x66, 0x0f, 0x1a, 0, 0},
        {0x67, 0x66, 0x0f, 0x1a, 6, 0}, // Truncated address16 disp16.
        {0x66, 0x0f, 0x1a, 0xe0},
        {0xf0, 0x0f, 0x1a, 0},
        {0xf2, 0x90},
        {0xf2, 0xf3, 0x74, 0}}) {
    auto Error = validateX86LegacyInstructionEncoding(Bytes, true);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST_F(X86LegacyMpxTest, MoveLockAndAlignmentRespectFaultPriority) {
  for (bool Locked : {false, true})
    for (bool Store : {false, true})
      for (bool Mapped : {false, true})
        for (bool Early : {false, true}) {
          if (Locked && !Store)
            continue;
          resetLegacyState();
          bindMpx();
          auto Controls = M->controls();
          Controls.CR0 |= 0x40000;
          ASSERT_FALSE(M->bindControls(Controls));
          S.EFlags |= 0x40000;
          S.Core.GPR[0] = 0xcffd;
          put(0xcffd, 0x12345678, GuestScalarWidth::I32);
          put(0x5804, 0xb000, GuestScalarWidth::I32);
          if (!Mapped)
            put(0x11034, 0xd006, GuestScalarWidth::I32);
          O.Segments.AlignmentPriority =
              Early ? X86AlignmentPriority::BeforePaging
                    : X86AlignmentPriority::AfterPaging;
          const auto Before = S.Mpx->bounds(0);
          std::vector<uint8_t> Bytes{0x66, 0x0f, uint8_t(Store ? 0x1b : 0x1a),
                                     0};
          if (Locked)
            Bytes.insert(Bytes.begin(), 0xf0);
          const auto R = run(Bytes);
          EXPECT_EQ(R.DeliveredVector, Early || Mapped ? 17 : 14);
          EXPECT_FALSE(R.InstructionCompleted);
          EXPECT_EQ(S.Mpx->bounds(0), Before);
          EXPECT_EQ(get(0xcffd), 0x12345678u);
          EXPECT_EQ(get(0x11030) & 0x20, Early ? 0u : 0x20u);
        }
  resetLegacyState();
  bindMpx();
  S.Core.GPR[0] = 0xc100;
  ASSERT_TRUE(run({0xf0, 0x66, 0x0f, 0x1b, 0}).InstructionCompleted);
  EXPECT_EQ(get(0xc100, GuestScalarWidth::I64), 0xfffff00000000100ULL);
}

TEST_F(X86LegacyMpxTest, FetchAndSegmentFailuresPrecedeBoundsEffects) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    resetLegacyState();
    bindMpx();
    const auto Before = S.Mpx->bounds(0);
    S.Core.GPR[5] = 0xc000;
    std::vector<uint8_t> Bytes{0x66, 0x0f, 0x1a, 0x45, 0};
    if (Case == 0)
      put(0x11004, 0x1006, GuestScalarWidth::I32);
    if (Case == 1)
      S.Core.Segments[2].Limit = 0xbfff;
    if (Case == 2) {
      S.Core.Segments[4].Unusable = true;
      Bytes.insert(Bytes.begin(), 0x64);
    }
    X86LegacyExecutionResult R;
    if (Case == 3)
      R = llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
    else
      R = run(Bytes);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(S.Mpx->bounds(0), Before);
    EXPECT_EQ(S.Mpx->status(), 0x12345678u);
    if (Case == 0)
      EXPECT_EQ(R.DeliveredVector, 14);
    else if (Case == 1)
      EXPECT_EQ(R.DeliveredVector, 12);
    else if (Case == 2)
      EXPECT_EQ(R.DeliveredVector, 13);
    else
      EXPECT_EQ(R.Status, X86LegacyExecutionStatus::CodeChanged);
    EXPECT_EQ(get(0x11030) & 0x20, 0u);
  }
}

TEST_F(X86LegacyMpxTest, DirectoryWatchpointSurvivesBackingUntilCompletion) {
  bindMpx(0x10001);
  S.EFlags = 0x202;
  S.Core.GPR[0] = 0x421000;
  S.Debug.emplace();
  O.Debug = X86LegacyDebugProfile::Watchpoints;
  S.Debug->DR[0] = 0x11084;
  S.Debug->DR7 = 0xf0401; // Read/write, four bytes.
  put(0x11084, 0x21007, GuestScalarWidth::I32);
  const std::vector<uint8_t> Bytes{0x0f, 0x1a, 0};
  auto R = run(Bytes);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingMpx);
  EXPECT_EQ(S.PendingMpx->DataBreakpoints, 1);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
  repairMpxBacking();
  R = llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
  EXPECT_EQ(R.DeliveredVector, 1);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_FALSE(S.PendingMpx);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff1u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1003u);
  EXPECT_EQ(S.Mpx->bounds(0)->Lower.KnownMask, UINT32_MAX);
}

TEST_F(X86LegacyMpxTest, MissingMemoryAndTransactionBindingsRefuseBeforeFetch) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    resetLegacyState();
    bindMpx();
    S.Core.GPR[0] = 0xc100;
    std::vector<uint8_t> Bytes{0x66, 0x0f, 0x1a, 0};
    if (Case == 0)
      O.MpxMemory.reset();
    if (Case == 1) {
      S.Core.Segments[3].Base = 1;
      Bytes = {0x0f, 0x1a, 0};
    }
    if (Case == 2)
      O.MpxMemory = static_cast<X86LegacyMpxProfile>(255);
    if (Case == 3) {
      auto Controls = M->controls();
      Controls.CR0 |= 0x40000;
      ASSERT_FALSE(M->bindControls(Controls));
      S.EFlags |= 0x40000;
      O.Segments.AlignmentPriority.reset();
    }
    if (Case >= 4) {
      auto Configuration = S.Mpx->configuration();
      Configuration.InTransaction = true;
      Configuration.Supported = Case == 4;
      ASSERT_TRUE(S.Mpx->configure(Configuration));
    }
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.Mpx->status(), 0x12345678u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
    EXPECT_FALSE(S.PendingMpx);
  }
}

TEST_F(X86LegacyMpxTest, UnknownStoredBoundsNeverPublishFabricatedValues) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x66, 0x0f, 0x1b, 0}, {0x0f, 0x1b, 0}}) {
    resetLegacyState();
    bindMpx();
    S.Core.GPR[0] = 0xc100;
    put(0xc100, 0x1122334455667788ULL);
    ASSERT_TRUE(S.Mpx->setBounds(0, {}));
    for (unsigned I = 0; I != Bytes.size(); ++I)
      put(0x1000 + I, Bytes[I], GuestScalarWidth::I8);
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0xc100, GuestScalarWidth::I64), 0x1122334455667788ULL);
    EXPECT_EQ(get(0x11030) & 0x20, 0u);
    EXPECT_EQ(get(0x11020) & 0x20, 0u);
    EXPECT_FALSE(S.PendingMpx);
  }
}

TEST_F(X86LegacyMpxTest, Code16UsesAddress32AndFullBoundsAcrossEveryFamily) {
  S.Core.Segments[1].Attributes &= ~0x4000u;
  bindMpx();
  S.Core.GPR[0] = 0x3004;
  S.Core.GPR[1] = 0x1234;
  S.Core.GPR[2] = 0;
  S.Core.GPR[3] = 0xc100;
  put(0x800c, 0xa001, GuestScalarWidth::I32);
  put(0xa01c, 0xdeadbeef, GuestScalarWidth::I32);
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x67, 0xf3, 0x0f, 0x1b, 0x44, 8, 8},
                                         {0x66, 0x0f, 0x1a, 0xc8},
                                         {0x67, 0x66, 0x0f, 0x1b, 0x0b},
                                         {0x67, 0x66, 0x0f, 0x1a, 0x13},
                                         {0x67, 0x0f, 0x1b, 0x14, 8},
                                         {0x67, 0x0f, 0x1a, 0x1c, 8},
                                         {0xf3, 0x0f, 0x1a, 0xd8},
                                         {0x67, 0xf2, 0x0f, 0x1a, 0xd8},
                                         {0xf2, 0x0f, 0x1b, 0xda}})
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
  for (unsigned I = 0; I != 4; ++I)
    EXPECT_EQ(*S.Mpx->bounds(I),
              (X86MpxBounds{{0x3004, UINT32_MAX}, {~0x4240u, UINT32_MAX}}));
  EXPECT_EQ(get(0xc100, GuestScalarWidth::I64), 0xffffbdbf00003004ULL);
  EXPECT_EQ(get(0xa010, GuestScalarWidth::I64), 0xffffbdbf00003004ULL);
  EXPECT_EQ(get(0xa018), 0x1234u);
  EXPECT_EQ(get(0xa01c), 0xdeadbeefu);
  EXPECT_EQ(S.Mpx->status(), 0x12345678u);
}

TEST_F(X86LegacyMpxTest,
       Address16FaultNeedsNoRAMProfileAndDisabledFormsComplete) {
  for (bool Code32 : {false, true})
    for (bool Enabled : {false, true})
      for (unsigned Kind = 0; Kind != 8; ++Kind) {
        resetLegacyState();
        if (!Code32)
          S.Core.Segments[1].Attributes &= ~0x4000u;
        bindMpx(Enabled ? 0x8001 : 0x8000);
        O.MpxMemory.reset();
        S.Core.Segments[3].Unusable = true;
        S.Core.GPR[6] = 0xffffffff;
        const auto Bounds = S.Mpx->bounds(0);
        std::vector<uint8_t> Bytes;
        if (Code32)
          Bytes.push_back(0x67);
        if (Kind < 6)
          Bytes.push_back(Kind < 2 ? 0xf3 : Kind < 4 ? 0xf2 : 0x66);
        Bytes.insert(Bytes.end(), {0x0f, uint8_t(Kind % 2 ? 0x1b : 0x1a), 4});
        const auto R = run(Bytes);
        EXPECT_TRUE(R.InstructionAttempted);
        EXPECT_EQ(R.InstructionCompleted, !Enabled);
        EXPECT_EQ(R.DeliveredVector, Enabled ? 6 : 0);
        EXPECT_EQ(S.Mpx->bounds(0), Bounds);
        EXPECT_EQ(S.Mpx->status(), 0x12345678u);
        EXPECT_FALSE(S.PendingMpx);
        if (Enabled)
          EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
        else
          EXPECT_EQ(S.EIP, 0x1000 + Bytes.size());
      }
}

TEST_F(X86LegacyMpxTest, Address16FetchFaultPrecedesUndefinedOpcode) {
  S.Core.Segments[1].Attributes &= ~0x4000u;
  bindMpx();
  O.MpxMemory.reset();
  S.EIP = 0x1fff;
  put(0x11008, 0, GuestScalarWidth::I32);
  const auto R = run({0x66, 0x0f, 0x1a, 4});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0x2000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1fffu);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_FALSE(S.PendingMpx);
}

TEST_F(X86LegacyMpxTest, Code16TableResumeRetainsDirectoryAndInstructionMode) {
  S.Core.Segments[1].Attributes &= ~0x4000u;
  bindMpx(0x10001);
  S.Core.GPR[0] = 0x421000;
  S.Core.GPR[1] = 0x1234;
  put(0x11084, 0x21007, GuestScalarWidth::I32);
  const std::vector<uint8_t> Bytes{0x67, 0x0f, 0x1a, 4, 8};
  auto R = run(Bytes);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingMpx && S.PendingMpx->Directory);
  EXPECT_EQ(S.PendingMpx->Directory->Value, 0x21007u);
  EXPECT_TRUE(R.InstructionAttempted);
  repairMpxBacking();
  put(0x21004, 0x2222222211111111ULL);
  put(0x2100c, 0x1234, GuestScalarWidth::I32);
  put(0x1000, 0xcc, GuestScalarWidth::I8);
  R = llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_FALSE(S.PendingMpx);
  EXPECT_EQ(S.EIP, 0x1005u);
  EXPECT_EQ(*S.Mpx->bounds(0),
            (X86MpxBounds{{0x11111111, UINT32_MAX}, {0x22222222, UINT32_MAX}}));
}
