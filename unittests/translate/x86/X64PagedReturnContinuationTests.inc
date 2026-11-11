//===- X64PagedReturnContinuationTests.inc - Retained return accesses ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included after the paged source fixture. Backing repair must continue the
/// original return even when descriptor writes have changed its frame inputs.
//===----------------------------------------------------------------------===//

class X64PagedReturnContinuationTest : public X64PagedExecutionTest {
protected:
  static std::vector<std::string> compilers() {
    std::vector<std::string> Result{NEVERD_TEST_CLANG};
    if (const auto *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
      Result.emplace_back(NeverC);
    return Result;
  }

  void frame(bool Interrupt, bool Outer = false) {
    reset({0x48, uint8_t(Interrupt ? 0xcf : 0xcb)});
    auto &Core = State.Core.Core.Core.Core;
    Core.GPR[4] = Interrupt ? 0x17fe0 : 0x17fe8;
    Core.RFlags = 0x202;
    State.NMIBlocked = 1;
    put(Core.GPR[4], 0x1800);
    put(Core.GPR[4] + 8, Outer ? 0x1b : 8);
    if (Interrupt)
      put(0x17ff0, 2);
    put(0x17ff8, 0xe800);
    put(0x18000, Outer ? 0x23 : 16);
  }

  std::unique_ptr<X86PagedMemory> repair(X86PagedMemory &Current,
                                         uint64_t Physical, unsigned Original) {
    auto Snapshot = RAM;
    Snapshot.Memory = Current.physicalMemory().snapshotMemoryRegions();
    const auto &Bytes = RAM.Memory.front().Bytes;
    Snapshot.Memory.push_back(
        {Physical, MemoryPermission::Read | MemoryPermission::Write, 0,
         std::vector<uint8_t>(Bytes.begin() + Original,
                              Bytes.begin() + Original + 4096)});
    return llvm::cantFail(X86PagedMemory::create(Snapshot, Current.features(),
                                                 Current.controls()));
  }
};

TEST_F(X64PagedReturnContinuationTest,
       PagedReturnResumeRetainsAliasedSelector) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool Interrupt : {false, true})
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true})
        for (const std::string O : {"-O0", "-O2"}) {
          const std::vector<uint8_t> Code{0x48,
                                          uint8_t(Interrupt ? 0xcf : 0xcb)};
          Run Driver = nullptr;
          ASSERT_NO_FATAL_FAILURE(
              compile(Code, LLVM, O, Driver, {0x1000}, Compiler));
          reset(Code);
          auto &Core = State.Core.Core.Core.Core;
          Core.GPR[4] = Interrupt ? 0x17fe0 : 0x17fe8;
          State.NMIBlocked = 1;
          const auto Descriptor = Core.GPR[4] + 3;
          State.GDTR = {Descriptor - 0xafd8, 0xffff};
          put(Core.GPR[4], 0x1800);
          // CS == 0xafda selects this DPL2 code descriptor. Its accessed byte
          // is also the low byte of saved CS. The write changes RPL2 to RPL3 in
          // RAM.
          put(Descriptor, 0x00afda000000ffffULL);
          if (Interrupt)
            put(0x17ff0, 2);
          put(0x17ff8, 0xe800);
          put(0x18000, 0x32);
          put(State.GDTR.Base + 0x30, 0x00cfd3000000ffffULL);
          put(0x130c0, 0x50007); // Outer SS has no physical backing yet.
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecutionOptions Options;
          Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
          Options.FarReturnProfile = X64PagedFarReturnProfile::SelectorFirst;
          X64PagedExecution Context(*Memory, Options);
          uint64_t Budget = 2;
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u);
          EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          EXPECT_EQ(Budget, 1u);
          EXPECT_EQ(State.NMIBlocked, Interrupt ? 0u : 1u);
          EXPECT_EQ(Memory->physicalMemory()
                        .loadScalar(Core.GPR[4] + 8, GuestScalarWidth::I16)
                        .Value,
                    0xafdbu);
          const auto Before = Memory->physicalMemory().snapshotMemoryRegions();
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u);
          EXPECT_FALSE(Context.lastResult().InstructionAttempted);
          EXPECT_EQ(Budget, 1u);
          EXPECT_EQ(
              Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
              Before.front().Bytes);
          auto Repaired = repair(*Memory, 0x50000, 0x18000);
          X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
              6u);
          EXPECT_TRUE(Resumed.lastError().empty()) << Resumed.lastError();
          EXPECT_EQ(Budget, 1u);
          EXPECT_EQ(Core.PC, 0x000000ffff001800ULL);
          EXPECT_EQ(Core.GPR[4], 0xe800u);
          EXPECT_EQ(State.Core.CS.Selector, 0xafdau);
          EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
          EXPECT_FALSE(Resumed.lastResult().InstructionAttempted);
        }
}

TEST_F(X64PagedReturnContinuationTest,
       PagedReturnResumePreservesShadowsAndBudget) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"})
        for (unsigned Case = 0; Case != 4; ++Case) {
          const bool Interrupt = Case == 0, Consumed = Case == 0 || Case == 2;
          const std::vector<uint8_t> Code{0x48,
                                          uint8_t(Interrupt ? 0xcf : 0xcb)};
          Run Driver = nullptr;
          Schedule Scheduled = nullptr;
          ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, {0x1000},
                                          Compiler, 0x1000, &Scheduled,
                                          std::nullopt, false, true));
          frame(Interrupt, !Interrupt);
          // RETF stops before any read in case 1, after an accessed-bit write
          // in case 2, or after reads without a write in case 3. IRET consumes
          // an attempt at NMI unblocking.
          if (Case == 2)
            put(0x5018, 0x00affa000000ffffULL);
          const auto Page = Case == 1 ? 0x17000 : 0x18000;
          put(0x13000 + (Page >> 12) * 8, 0x50007);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedDebugState Debug;
          X64PagedExecutionOptions Options;
          Options.Debug = &Debug;
          Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
          Options.FarReturnProfile = X64PagedFarReturnProfile::SelectorFirst;
          X64PagedExecution Context(*Memory, Options);
          Shadows Shadow;
          Shadow.STIBlocked = Shadow.SSBlocked = 1;
          Producer P;
          P.Ready = 0;
          uint64_t Budget = 1, Polls = 1;
          const auto Run = [&](X64PagedExecution &Current) {
            return Scheduled(&State, reinterpret_cast<uint8_t *>(&Current),
                             &Budget, &Shadow, &Polls, Producer::peek,
                             Producer::accepted, &P);
          };
          ASSERT_EQ(Run(Context), 18u) << Context.lastError();
          EXPECT_EQ(Budget, Consumed ? 0u : 1u);
          EXPECT_EQ(Context.pendingProgress(),
                    kX64PagedInstructionPending |
                        (Consumed ? kX64PagedAttemptConsumed : 0));
          EXPECT_EQ(Shadow.STIBlocked, 1);
          EXPECT_EQ(Shadow.SSBlocked, 1);
          EXPECT_EQ(P.Polls, 1u);
          P.Ready = 1;
          P.Candidate = {2,
                         3}; // A newly ready NMI cannot interrupt the return.
          Budget = 0;
          EXPECT_EQ(Run(Context), Consumed ? 18u : 9u) << Context.lastError();
          EXPECT_EQ(P.Polls, 1u);
          EXPECT_EQ(P.Acks, 0u);
          if (!Consumed)
            Budget = 1;
          // A second backing stop retains the original inhibition and budget.
          EXPECT_EQ(Run(Context), 18u) << Context.lastError();
          EXPECT_EQ(Budget, Consumed ? 0u : 1u);
          EXPECT_EQ(Shadow.STIBlocked, 1);
          EXPECT_EQ(Shadow.SSBlocked, 1);
          auto Repaired = repair(*Memory, 0x50000, Page);
          X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
          EXPECT_EQ(Run(Resumed), 6u) << Resumed.lastError();
          EXPECT_EQ(Budget, 0u);
          EXPECT_EQ(Polls, 0u);
          EXPECT_EQ(P.Polls, 1u);
          EXPECT_EQ(P.Acks, 0u);
          EXPECT_EQ(Shadow.STIBlocked, 0);
          EXPECT_EQ(Shadow.SSBlocked, 0);
          EXPECT_EQ(State.NMIBlocked, Interrupt ? 0 : 1);
          EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x1800u);
          EXPECT_EQ(State.Core.Core.Core.Core.GPR[4], 0xe800u);
          EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
          EXPECT_EQ(Resumed.lastResult().InstructionAttempted, !Consumed);
          EXPECT_FALSE(Resumed.continuation());
        }
}

TEST_F(X64PagedReturnContinuationTest,
       PagedReturnResumeRejectsChangedBindings) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0xcf};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr, OtherDriver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                    NEVERD_TEST_CLANG, 0x1000, &Scheduled,
                                    std::nullopt, false, true));
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", OtherDriver));
    frame(true);
    put(0x130c0, 0x50007);
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedDebugState Debug;
    X64PagedExecutionOptions Options;
    Options.Debug = &Debug;
    Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
    X64PagedExecution Context(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u);
    const auto Pending = Context.continuation();
    ASSERT_TRUE(Pending);
    auto Repaired = repair(*Memory, 0x50000, 0x18000);
    for (unsigned Case = 0; Case != 10; ++Case) {
      SCOPED_TRACE(Case);
      const auto Before = State;
      const auto BeforeDebug = Debug;
      const auto BeforeControls = Repaired->controls();
      auto Binding = Options;
      if (Case == 0)
        ++State.Core.Core.Core.Core.GPR[15];
      if (Case == 1)
        ++State.Core.Core.Core.Core.ZMM[31][63];
      if (Case == 2)
        ++State.Core.Core.Core.Core.Undefined[7];
      if (Case == 3)
        State.NMIBlocked = 1;
      if (Case == 4)
        ++Debug.DR[0];
      if (Case == 5)
        Binding.AlignmentPriority = X64PagedAlignmentPriority::BeforePaging;
      if (Case == 6)
        Binding.IretAccessOrder.reset();
      if (Case == 7) {
        auto Changed = BeforeControls;
        Changed.CR3 += 0x1000;
        ASSERT_FALSE(bool(Repaired->bindControls(Changed)));
      }
      X64PagedExecution Refused(*Repaired, Binding, Pending);
      const auto RAMBefore = Repaired->physicalMemory().snapshotMemoryRegions();
      auto *Opaque = reinterpret_cast<uint8_t *>(&Refused);
      if (Case == 8) {
        X64ContinuationEventV1 Event;
        Event.Kind = X64ContinuationEventKind::NMI;
        Event.Vector = 2;
        auto Result = Refused.deliver(State, Event, 0xffff);
        ASSERT_FALSE(bool(Result));
        llvm::consumeError(Result.takeError());
      } else {
        EXPECT_EQ((Case == 9 ? OtherDriver : Driver)(&State, Opaque, &Budget),
                  5u);
        EXPECT_FALSE(Refused.lastError().empty());
      }
      EXPECT_EQ(Refused.continuation(), Pending);
      EXPECT_FALSE(Refused.lastResult().InstructionAttempted);
      EXPECT_FALSE(Refused.lastResult().InstructionResumed);
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(
          Repaired->physicalMemory().snapshotMemoryRegions().front().Bytes,
          RAMBefore.front().Bytes);
      State = Before;
      Debug = BeforeDebug;
      ASSERT_FALSE(bool(Repaired->bindControls(BeforeControls)));
    }
    X64PagedExecution Resumed(*Repaired, Options, Pending);
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
              6u)
        << Resumed.lastError();
    EXPECT_EQ(Budget, 0u);
    EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
  }
}

TEST_F(X64PagedReturnContinuationTest,
       PagedReturnResumeRetainsDebugBeforeEventHandoff) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0xcf};
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, {0x1000},
                                        Compiler, 0x1000, nullptr, std::nullopt,
                                        false, true));
        for (unsigned Cause = 1; Cause != 4; ++Cause)
          for (bool PauseEvent : {false, true}) {
            frame(true);
            auto &Core = State.Core.Core.Core.Core;
            Core.RFlags = Cause & 1 ? 0x102 : 2;
            put(0x130c0, 0x50007);
            if (PauseEvent)
              put(0x13030, 0x60007);
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedDebugState Debug;
            if (Cause & 2) {
              Debug.DR[0] = 0x17ff0;
              Debug.DR7 |= 0xb0001; // Read/write eight-byte FLAGS slot.
            }
            const auto BeforeDebug = Debug;
            X64PagedExecutionOptions Options;
            Options.Debug = &Debug;
            Options.DebugProfile = X86InstructionDebugProfile::Watchpoints;
            Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
            X64PagedExecution Context(*Memory, Options);
            uint64_t Budget = 1;
            ASSERT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                18u)
                << Context.lastError();
            EXPECT_EQ(Debug, BeforeDebug);
            EXPECT_EQ(Budget, 0u);
            auto Repaired = repair(*Memory, 0x50000, 0x18000);
            X64PagedExecution Resumed(*Repaired, Options,
                                      Context.continuation());
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
                PauseEvent ? 18u : 7u)
                << Resumed.lastError();
            EXPECT_EQ(Budget, 0u);
            EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
            EXPECT_FALSE(Resumed.lastResult().InstructionAttempted);
            EXPECT_FALSE(Resumed.continuation());
            EXPECT_EQ(Debug.DR6 & 0x4001,
                      (Cause & 1 ? 0x4000u : 0u) | (Cause & 2 ? 1u : 0u));
            if (PauseEvent) {
              ASSERT_TRUE(Resumed.lastResult().Delivery);
              ASSERT_TRUE(Resumed.lastResult().Delivery->Continuation);
              auto EventMemory = repair(*Repaired, 0x60000, 0x6000);
              X64PagedExecution EventContext(*EventMemory, Options);
              auto Event = EventContext.resume(
                  State, *Resumed.lastResult().Delivery->Continuation, 0xffff);
              ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
              EXPECT_EQ(Event->Exit, 7u);
              EXPECT_FALSE(Event->InstructionAttempted);
              EXPECT_FALSE(Event->InstructionCompleted);
              EXPECT_EQ(Core.PC, 0x4010u);
            } else {
              EXPECT_EQ(Core.PC, 0x4010u);
            }
          }
      }
}

TEST_F(X64PagedReturnContinuationTest,
       PagedReturnResumeFaultRetiresBeforeEvent) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool Interrupt : {false, true})
    for (bool LLVM : {false, true}) {
      const std::vector<uint8_t> Code{0x48, uint8_t(Interrupt ? 0xcf : 0xcb)};
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                      NEVERD_TEST_CLANG, 0x1000, &Scheduled));
      frame(Interrupt, true);
      put(0x5018, 0x00affa000000ffffULL);
      put(0x18000, 0x1b); // The outer SS selector names a code descriptor.
      put(0x130c0, 0x50007);
      put(0x13030, 0x60007); // The resulting #GP will pause at IDT access.
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecutionOptions Options;
      Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
      Options.FarReturnProfile = X64PagedFarReturnProfile::SelectorFirst;
      X64PagedExecution Context(*Memory, Options);
      Shadows Shadow;
      Shadow.SSBlocked = 1;
      Producer P;
      P.Ready = 0;
      uint64_t Budget = 1, Polls = 1;
      const auto Run = [&](X64PagedExecution &Current) {
        return Scheduled(&State, reinterpret_cast<uint8_t *>(&Current), &Budget,
                         &Shadow, &Polls, Producer::peek, Producer::accepted,
                         &P);
      };
      ASSERT_EQ(Run(Context), 18u) << Context.lastError();
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(Shadow.SSBlocked, 1);
      auto Repaired = repair(*Memory, 0x50000, 0x18000);
      X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
      ASSERT_EQ(Run(Resumed), 18u) << Resumed.lastError();
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(Shadow.SSBlocked, 0);
      EXPECT_EQ(P.Polls, 1u);
      EXPECT_FALSE(Resumed.continuation());
      EXPECT_FALSE(Resumed.lastResult().InstructionCompleted);
      EXPECT_FALSE(Resumed.lastResult().InstructionAttempted);
      ASSERT_TRUE(Resumed.lastResult().Exception);
      EXPECT_EQ(Resumed.lastResult().Exception->Vector, 13);
      EXPECT_EQ(Resumed.lastResult().Exception->ErrorCode, 0x18u);
      ASSERT_TRUE(Resumed.lastResult().Delivery);
      ASSERT_TRUE(Resumed.lastResult().Delivery->Continuation);
      auto EventMemory = repair(*Repaired, 0x60000, 0x6000);
      X64PagedExecution EventContext(*EventMemory, Options);
      auto Event = EventContext.resume(
          State, *Resumed.lastResult().Delivery->Continuation, 0xffff);
      ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
      EXPECT_EQ(Event->Exit, 7u);
      EXPECT_FALSE(Event->InstructionAttempted);
      EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x40d0u);
    }
}

TEST_F(X64PagedReturnContinuationTest,
       PagedReturnOwnerPreservesImmutableProgress) {
  for (bool Interrupt : {false, true}) {
    frame(Interrupt, true);
    put(0x5018, 0x00affa000000ffffULL);
    put(0x130c0, 0x50007);
    Controls.RFlags = State.Core.Core.Core.Core.RFlags;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    auto First =
        Interrupt
            ? returnX64PagedInterrupt(State, *Memory, 8,
                                      X64PagedAlignmentPriority::AfterPaging,
                                      X64PagedIretAccessOrder::FlagsFirst)
            : returnX64PagedFar(State, *Memory, 8, 0,
                                X64PagedAlignmentPriority::AfterPaging,
                                X64PagedFarReturnProfile::SelectorFirst);
    ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
    ASSERT_TRUE(First->BackingFault);
    ASSERT_TRUE(First->Continuation);
    EXPECT_TRUE(First->InstructionAttempted);
    const auto Pending = First->Continuation;
    const auto Before = State;
    ++State.Core.Core.Core.Core.FSBase;
    auto BadState = resumeX64PagedReturn(State, *Memory, Pending);
    ASSERT_FALSE(bool(BadState));
    llvm::consumeError(BadState.takeError());
    State = Before;
    auto Snapshot = RAM;
    Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
    auto Changed = Features;
    Changed.PSE = !Changed.PSE;
    auto Other = llvm::cantFail(
        X86PagedMemory::create(Snapshot, Changed, Memory->controls()));
    auto BadFeatures = resumeX64PagedReturn(State, *Other, Pending);
    ASSERT_FALSE(bool(BadFeatures));
    llvm::consumeError(BadFeatures.takeError());
    EXPECT_EQ(State, Before);
    auto Again = resumeX64PagedReturn(State, *Memory, Pending);
    ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
    ASSERT_TRUE(Again->BackingFault);
    EXPECT_FALSE(Again->InstructionAttempted);
    EXPECT_NE(Again->Continuation, Pending);
    auto Repaired = repair(*Memory, 0x50000, 0x18000);
    auto Done = resumeX64PagedReturn(State, *Repaired, Again->Continuation);
    ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
    EXPECT_TRUE(Done->Completed);
    EXPECT_FALSE(Done->InstructionAttempted);
    EXPECT_FALSE(Done->Continuation);
    EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x1800u);
  }
}

TEST_F(X64PagedReturnContinuationTest,
       PagedReturnResumeRetainsEveryFrameWidth) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool Interrupt : {false, true})
    for (unsigned Width : {2u, 4u, 8u})
      for (const auto &Compiler : compilers())
        for (bool LLVM : {false, true}) {
          std::vector<uint8_t> Code;
          if (Width != 4)
            Code.push_back(Width == 2 ? 0x66 : 0x48);
          Code.push_back(Interrupt ? 0xcf : 0xcb);
          Run Driver = nullptr;
          ASSERT_NO_FATAL_FAILURE(
              compile(Code, LLVM, "-O2", Driver, {0x1000}, Compiler));
          reset(Code);
          auto &Core = State.Core.Core.Core.Core;
          Core.GPR[4] = 0x18000 - (Interrupt ? 4 : 3) * Width;
          State.NMIBlocked = 1;
          put(Core.GPR[4], 0x1800);
          put(Core.GPR[4] + Width, 0x1b);
          if (Interrupt)
            put(Core.GPR[4] + Width * 2, 2);
          put(0x18000 - Width, 0xe800);
          put(0x18000, 0x23);
          put(0x5018, 0x00affa000000ffffULL);
          put(0x130c0, 0x50007);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecutionOptions Options;
          Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
          Options.FarReturnProfile = X64PagedFarReturnProfile::SelectorFirst;
          X64PagedExecution Context(*Memory, Options);
          uint64_t Budget = 1;
          ASSERT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u)
              << Context.lastError();
          EXPECT_EQ(Budget, 0u);
          auto Repaired = repair(*Memory, 0x50000, 0x18000);
          X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
              6u)
              << Resumed.lastError();
          EXPECT_EQ(Budget, 0u);
          EXPECT_EQ(Core.PC, 0x1800u);
          EXPECT_EQ(Core.GPR[4], 0xe800u);
          EXPECT_EQ(State.Core.CS.Selector, 0x1bu);
          EXPECT_EQ(State.Core.SS.Selector, 0x23u);
        }
}
