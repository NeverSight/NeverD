//===- X86LegacyEventReplayTests.inc - Retained event inputs -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A partial frame can overwrite the TSS inputs consumed before a backing stop.
//===----------------------------------------------------------------------===//

class X86LegacyEventReplayTest : public X86LegacyExecutionTest {
protected:
  void setupAlias(bool Watch, bool Missing, bool Gate32 = true) {
    X86LegacyExecutionTest::SetUp();
    O.EventDelivery = {};
    O.Debug.reset();
    S.IDTRBase = 0x9000;
    S.TR.Base = 0x8000;
    S.EFlags = 0x202;
    for (unsigned V = 0; V != 256; ++V)
      put(0x9000 + V * 8, 0x2000 | (uint64_t{8} << 16) |
                              (uint64_t(Gate32 ? 0xee : 0xe6) << 40));
    // The upper target-stack page aliases TSS backing. Three frame stores
    // overwrite ESP0/SS0 before the next store reaches the missing lower page.
    put(0x5004, Gate32 ? 0x600c : 0x6006, GuestScalarWidth::I32);
    put(0x5008, 16, GuestScalarWidth::I16);
    put(0x11018, 0x5007, GuestScalarWidth::I32);
    put(0x11020, 0x5007, GuestScalarWidth::I32);
    put(0x11014, Missing ? 0x40007 : 0xa007, GuestScalarWidth::I32);
    if (Watch) {
      O.Debug = X86LegacyDebugProfile::Watchpoints;
      S.Debug.emplace();
      S.Debug->DR7 = 0xf0401;
      S.Debug->DR[0] = 0x8004;
    }
  }

  void expectState(const X86LegacyExecutionState &Want) {
    EXPECT_EQ(S.EIP, Want.EIP);
    EXPECT_EQ(S.EFlags, Want.EFlags);
    EXPECT_EQ(S.CR2, Want.CR2);
    EXPECT_EQ(S.Core.GPR, Want.Core.GPR);
    EXPECT_EQ(S.Core.CPL, Want.Core.CPL);
    EXPECT_EQ(S.Core.GDTRBase, Want.Core.GDTRBase);
    EXPECT_EQ(S.Core.GDTRLimit, Want.Core.GDTRLimit);
    EXPECT_EQ(S.IDTRBase, Want.IDTRBase);
    EXPECT_EQ(S.IDTRLimit, Want.IDTRLimit);
    const auto Cache = [](const auto &A, const auto &B) {
      EXPECT_EQ(A.Base, B.Base);
      EXPECT_EQ(A.Limit, B.Limit);
      EXPECT_EQ(A.Selector, B.Selector);
      EXPECT_EQ(A.Attributes, B.Attributes);
      EXPECT_EQ(A.Unusable, B.Unusable);
    };
    for (unsigned I = 0; I != S.Core.Segments.size(); ++I)
      Cache(S.Core.Segments[I], Want.Core.Segments[I]);
    Cache(S.Core.LDTR, Want.Core.LDTR);
    Cache(S.TR, Want.TR);
    EXPECT_EQ(S.STIBlocked, Want.STIBlocked);
    EXPECT_EQ(S.SSBlocked, Want.SSBlocked);
    EXPECT_EQ(S.NMIBlocked, Want.NMIBlocked);
    EXPECT_EQ(S.Sysenter.CS, Want.Sysenter.CS);
    EXPECT_EQ(S.Sysenter.ESP, Want.Sysenter.ESP);
    EXPECT_EQ(S.Sysenter.EIP, Want.Sysenter.EIP);
    EXPECT_FALSE(S.Mpx);
    EXPECT_FALSE(Want.Mpx);
    ASSERT_EQ(bool(S.Debug), bool(Want.Debug));
    if (S.Debug) {
      EXPECT_EQ(S.Debug->DR, Want.Debug->DR);
      EXPECT_EQ(S.Debug->DR6, Want.Debug->DR6);
      EXPECT_EQ(S.Debug->DR7, Want.Debug->DR7);
      EXPECT_EQ(S.Debug->DebugCTL, Want.Debug->DebugCTL);
      EXPECT_EQ(S.Debug->PendingData, Want.Debug->PendingData);
    }
  }
};

TEST_F(X86LegacyEventReplayTest, PartialFrameRetainsAliasedTSSInputs) {
  for (bool Gate32 : {false, true})
    for (bool Watch : {false, true})
      for (bool Bounded : {false, true}) {
        SCOPED_TRACE(testing::Message() << "watch=" << Watch << " bounded="
                                        << Bounded << " gate32=" << Gate32);
        setupAlias(Watch, false, Gate32);
        const auto Complete = run({0xcd, 64});
        ASSERT_EQ(Complete.Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_EQ(Complete.DeliveredVector, Watch ? 1 : 64);
        const auto Want = S;
        const auto WantRAM = M->physicalMemory().snapshotMemoryRegions();
        const auto WantFlags = M->controls().RFlags;
        setupAlias(Watch, true, Gate32);
        if (Bounded)
          O.EventDelivery.MaxAttempts = 1;
        const auto Partial = run({0xcd, 64});
        ASSERT_EQ(Partial.Status, X86LegacyExecutionStatus::BackingFault);
        ASSERT_TRUE(S.PendingEvent);
        EXPECT_FALSE(S.PendingEvent->CompletedAccesses.empty());
        EXPECT_EQ(S.PendingEvent->DataBreakpoints, Watch ? 1 : 0);
        EXPECT_EQ(get(0x5008), Gate32 ? 35u : 16u);
        EXPECT_EQ(get(0x5004), Gate32 ? 0xd000u : 35u);
        put(0x11014, 0xa007, GuestScalarWidth::I32);
        auto Got = resumeX86LegacyEvent(S, *M, O);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        if (Watch && Bounded) {
          ASSERT_EQ(Got->Status,
                    X86LegacyExecutionStatus::EventBudgetExhausted);
          ASSERT_TRUE(S.PendingEvent);
          EXPECT_TRUE(S.PendingEvent->CompletedAccesses.empty());
          EXPECT_TRUE(Got->SoftwareCompleted);
          Got = resumeX86LegacyEvent(S, *M, O);
          ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        }
        EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_EQ(Got->DeliveredVector, Complete.DeliveredVector);
        EXPECT_FALSE(Got->EventAccepted);
        EXPECT_FALSE(Got->InstructionAttempted);
        EXPECT_FALSE(S.PendingEvent);
        ASSERT_NO_FATAL_FAILURE(expectState(Want));
        EXPECT_EQ(M->controls().RFlags, WantFlags);
        EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
                  WantRAM.front().Bytes);
      }
}

TEST_F(X86LegacyEventReplayTest, ChangedAccessRecordRefusesBeforeReplay) {
  for (unsigned Change = 0; Change != 4; ++Change) {
    setupAlias(true, true);
    ASSERT_EQ(run({0xcd, 64}).Status, X86LegacyExecutionStatus::BackingFault);
    ASSERT_TRUE(S.PendingEvent);
    auto &Accesses = S.PendingEvent->CompletedAccesses;
    ASSERT_FALSE(Accesses.empty());
    if (Change == 0)
      ++Accesses.front().Address;
    else if (Change == 1)
      --Accesses.front().Bytes;
    else if (Change == 2)
      Accesses.front().Write = true;
    else {
      auto Write = std::find_if(Accesses.begin(), Accesses.end(),
                                [](const auto &A) { return A.Write; });
      ASSERT_NE(Write, Accesses.end());
      ++Write->Value;
    }
    const auto Want = S;
    const auto WantRAM = M->physicalMemory().snapshotMemoryRegions();
    auto R = resumeX86LegacyEvent(S, *M, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    ASSERT_NO_FATAL_FAILURE(expectState(Want));
    EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
              WantRAM.front().Bytes);
  }
}
