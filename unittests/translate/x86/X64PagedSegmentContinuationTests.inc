//===- X64PagedSegmentContinuationTests.inc - Retained selector loads ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included after the paged source fixture. Page walks can change earlier
/// operands or original bytes; backing repair must retain accepted progress.
//===----------------------------------------------------------------------===//

class X64PagedSegmentContinuationTest : public X64PagedExecutionTest {
protected:
  static std::vector<std::string> compilers() {
    std::vector<std::string> Result{NEVERD_TEST_CLANG};
    if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
      Result.emplace_back(NeverC);
    return Result;
  }

  X64PagedExecutionOptions options(X64PagedDebugState *Debug = nullptr) {
    X64PagedExecutionOptions Options;
    Options.AlignmentPriority = X64PagedAlignmentPriority::AfterPaging;
    Options.FarPointerProfile = X64PagedFarPointerProfile::OffsetFirst;
    Options.SegmentMoveProfile = X64PagedSegmentMoveProfile::SelectorFirst;
    Options.Debug = Debug;
    Options.DebugProfile = X86InstructionDebugProfile::Watchpoints;
    return Options;
  }

  static std::vector<uint8_t> aliasCode(unsigned Case) {
    switch (Case) {
    case 0:
      return {0x8e, 0x27}; // MOV FS,[RDI].
    case 1:
      return {0x0f, 0xa1}; // POP FS.
    case 2:
      return {0x66, 0x0f, 0xa1}; // POP FS, word stack slot.
    case 3:
      return {0x0f, 0xb4, 0x07}; // LFS EAX,[RDI].
    case 4:
      return {0x48, 0x0f, 0xb4, 0x07}; // LFS RAX,[RDI].
    default:
      return {0x0f, 0xb4, 0x00}; // LFS EAX,[RAX].
    }
  }

  // The descriptor page is present for the original CPU and absent only in
  // the software embedding. Every repair adds that page without rewriting
  // accepted operands, original bytes or page-table effects.
  unsigned alias(unsigned Case) {
    reset(aliasCode(Case));
    auto &Core = State.Core.Core.Core.Core;
    State.DS = State.ES = State.Core.SS;
    State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
    Core.FSBase = 0xffff800000022000ULL;
    put(0x13100, 0x13003); // linear 0x20000 aliases the PTE page.
    put(0x13028, Case == 4 ? 0x30003 : 0x3001b);
    Core.GPR[7] = Case == 3 ? 0x20024 : 0x20028;
    if (Case == 1 || Case == 2)
      Core.GPR[4] = 0x20028;
    if (Case == 4)
      put(0x13030, 0x10); // Selector after the full eight-byte PTE operand.
    unsigned Page = 0x30000;
    if (Case == 5) {
      // The opcode itself is the low byte of the GDT PTE. Its A update
      // changes 0f b4 00 to 2f b4 00 only after authenticated fetch.
      Core.PC = 0x1028;
      Core.GPR[0] = 0x20000;
      put(0x13008, 0x13003);
      put(0x13028, 0x00b40f);
      put(0x13100, 0x30007);
      put(0x30000, 0x001012345678ULL);
      Page = 0xb000;
    }
    put(Page + 0x10, 0x00cff2022000ffffULL);
    put(Page + 0x18, 0x00cff2022000ffffULL);
    put(Page + 0x38, 0x00cff2033000ffffULL);
    return Page;
  }

  std::unique_ptr<X86PagedMemory> without(unsigned Page) {
    auto Snapshot = RAM;
    const auto &Bytes = RAM.Memory.front().Bytes;
    Snapshot.Memory.clear();
    Snapshot.Memory.push_back({0,
                               MemoryPermission::Read | MemoryPermission::Write,
                               0,
                               {Bytes.begin(), Bytes.begin() + Page}});
    Snapshot.Memory.push_back({Page + 4096,
                               MemoryPermission::Read | MemoryPermission::Write,
                               0,
                               {Bytes.begin() + Page + 4096, Bytes.end()}});
    return llvm::cantFail(X86PagedMemory::create(Snapshot, Features, Controls));
  }

  std::unique_ptr<X86PagedMemory> repair(X86PagedMemory &Current, unsigned Page,
                                         unsigned Original) {
    auto Snapshot = RAM;
    Snapshot.Memory = Current.physicalMemory().snapshotMemoryRegions();
    const auto &Bytes = RAM.Memory.front().Bytes;
    Snapshot.Memory.push_back(
        {Page,
         MemoryPermission::Read | MemoryPermission::Write,
         0,
         {Bytes.begin() + Original, Bytes.begin() + Original + 4096}});
    return llvm::cantFail(X86PagedMemory::create(Snapshot, Current.features(),
                                                 Current.controls()));
  }

  static std::vector<uint8_t> bytes(X86PagedMemory &Current) {
    std::vector<uint8_t> Result(0x40000);
    for (const auto &Region :
         Current.physicalMemory().snapshotMemoryRegions()) {
      if (Region.Address + Region.Bytes.size() <= Result.size())
        std::copy(Region.Bytes.begin(), Region.Bytes.end(),
                  Result.begin() + Region.Address);
    }
    return Result;
  }
};

TEST_F(X64PagedSegmentContinuationTest, PagingAliasesRetainOperandsAndCode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (unsigned Case = 0; Case != 6; ++Case)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true})
        for (const std::string O : {"-O0", "-O2"}) {
          SCOPED_TRACE(llvm::formatv("case={0} {1} llvm={2} {3}", Case,
                                     Compiler, LLVM, O)
                           .str());
          const auto Code = aliasCode(Case);
          const uint64_t Address = Case == 5 ? 0x1028 : 0x1000;
          Run Driver = nullptr;
          ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, {Address},
                                          Compiler, Address, nullptr,
                                          X64NullSegmentBasePolicy::Clear));
          const auto Page = alias(Case);
          const auto Before = State;
          Memory = without(Page);
          auto Options = options();
          X64PagedExecution Context(*Memory, Options);
          uint64_t Budget = 1;
          ASSERT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u)
              << Context.lastError();
          EXPECT_EQ(State, Before);
          EXPECT_EQ(Budget, 1u);
          EXPECT_FALSE(Context.lastResult().InstructionAttempted);
          EXPECT_EQ(Context.pendingProgress(), kX64PagedInstructionPending);
          EXPECT_EQ(Memory->physicalMemory()
                        .loadScalar(0x13028, GuestScalarWidth::I8)
                        .Value,
                    Case == 5   ? 0x2fu
                    : Case == 4 ? 0x23u
                                : 0x3bu);
          const auto StoppedRAM = bytes(*Memory);
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u)
              << Context.lastError();
          EXPECT_EQ(bytes(*Memory), StoppedRAM);
          EXPECT_EQ(Budget, 1u);
          Budget = 0;
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              9u)
              << Context.lastError();
          EXPECT_EQ(Context.pendingProgress(), kX64PagedInstructionPending);
          auto Repaired = repair(*Memory, Page, Page);
          X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
          Budget = 1;
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
              9u)
              << Resumed.lastError();
          EXPECT_EQ(Budget, 0u);
          EXPECT_TRUE(Resumed.lastResult().InstructionAttempted);
          EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
          EXPECT_TRUE(Resumed.lastResult().InstructionResumed);
          EXPECT_FALSE(Resumed.continuation());
          auto Expected = Before;
          auto &Core = Expected.Core.Core.Core.Core;
          Core.PC += Code.size();
          Core.FSBase = 0x22000;
          Expected.FS = {UINT32_MAX, uint16_t(Case < 4 ? 0x1b : 0x10), 0xc0f3};
          if (Case == 1 || Case == 2)
            Core.GPR[4] += Case == 1 ? 8 : 2;
          if (Case >= 3)
            Core.GPR[0] = Case == 3 ? 0 : Case == 4 ? 0x30003 : 0x12345678;
          EXPECT_EQ(State, Expected);
        }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedSegmentContinuationTest, AliasesMatchOriginalInstructions) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Comparisons = 0;
  for (unsigned Case = 0; Case != 6; ++Case)
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(llvm::formatv("case={0} llvm={1}", Case, LLVM).str());
      X64PagedNativeCPU CPU;
      ASSERT_NO_FATAL_FAILURE(CPU.initialize());
      if (testing::Test::IsSkipped())
        return;
      if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
        GTEST_SKIP() << "Segment alias oracle requires KVM pre-fault memory";
      // Stage-2 faults can restart the original instruction after its guest
      // page walk changed an aliased operand. Preserve cold guest PTEs while
      // populating only KVM's physical backing before this comparison.
      CPU.PrefaultMemory = true;
      Features.PhysicalAddressBits = CPU.PhysicalBits;
      const auto Code = aliasCode(Case);
      const uint64_t Address = Case == 5 ? 0x1028 : 0x1000;
      Run Driver = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {Address},
                                      NEVERD_TEST_CLANG, Address, nullptr,
                                      X64NullSegmentBasePolicy::Clear));
      const auto Page = alias(Case);
      X64ContinuationStateV6 Native;
      bool Fault = false;
      ASSERT_NO_FATAL_FAILURE(CPU.execute(
          RAM.Memory.front().Bytes, State, Controls.CR0, Code.size(), Native,
          Fault, false, nullptr, Controls.CR4, true, false));
      ASSERT_FALSE(Fault);
      Memory = without(Page);
      auto Options = options();
      X64PagedExecution Context(*Memory, Options);
      uint64_t Budget = 1;
      ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                18u)
          << Context.lastError();
      auto Repaired = repair(*Memory, Page, Page);
      X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
      ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
                9u)
          << Resumed.lastError();
      const auto &ActualCore = State.Core.Core.Core.Core;
      const auto &NativeCore = Native.Core.Core.Core.Core;
      EXPECT_EQ(ActualCore.GPR[0], NativeCore.GPR[0]);
      EXPECT_EQ(ActualCore.FSBase, NativeCore.FSBase);
      EXPECT_EQ(State.FS.Selector, Native.FS.Selector);
      EXPECT_EQ(State, Native);
      const auto Actual = bytes(*Repaired);
      const auto Difference =
          std::mismatch(Actual.begin(), Actual.end(), CPU.RAM);
      EXPECT_EQ(Difference.first, Actual.end())
          << "RAM differs at " << (Difference.first - Actual.begin());
      ++Comparisons;
    }
  RecordProperty("original_segment_alias_comparisons", Comparisons);
}
#endif

TEST_F(X64PagedSegmentContinuationTest,
       EmptyProgressKeepsBudgetAndEventBoundary) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<std::vector<uint8_t>> Forms{{0x8e, 0x27},
                                                {0x0f, 0xa1},
                                                {0x0f, 0xb4, 0x07},
                                                {0x8c, 0x27},
                                                {0x0f, 0xa0}};
  for (unsigned Form = 0; Form != Forms.size(); ++Form)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true})
        for (const std::string O : {"-O0", "-O2"}) {
          SCOPED_TRACE(llvm::formatv("form={0} {1} llvm={2} {3}", Form,
                                     Compiler, LLVM, O)
                           .str());
          Run Driver = nullptr;
          Schedule Scheduled = nullptr;
          ASSERT_NO_FATAL_FAILURE(compile(
              Forms[Form], LLVM, O, Driver, {0x1000}, Compiler, 0x1000,
              &Scheduled, X64NullSegmentBasePolicy::Clear, false, true));
          reset(Forms[Form]);
          State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
          auto &Core = State.Core.Core.Core.Core;
          Core.RFlags = 0x202;
          if (Form == 1 || Form == 4)
            Core.GPR[4] = Form == 1 ? 0x20000 : 0x20008;
          put(0x30000, Form == 2 ? 0x001012345678ULL : 16);
          put(0x13100, 0x50007);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedDebugState Debug;
          auto Options = options(&Debug);
          X64PagedExecution Context(*Memory, Options);
          Shadows Shadow;
          Shadow.STIBlocked = Shadow.SSBlocked = 1;
          Producer Events;
          Events.Ready = 0;
          uint64_t Budget = 1, Polls = 1;
          const auto Execute = [&](X64PagedExecution &Current) {
            return Scheduled(&State, reinterpret_cast<uint8_t *>(&Current),
                             &Budget, &Shadow, &Polls, Producer::peek,
                             Producer::accepted, &Events);
          };
          const auto Before = State;
          ASSERT_EQ(Execute(Context), 18u) << Context.lastError();
          EXPECT_EQ(State, Before);
          EXPECT_EQ(Budget, 1u);
          EXPECT_EQ(Context.pendingProgress(), kX64PagedInstructionPending);
          EXPECT_EQ(Events.Polls, 1u);
          EXPECT_EQ(Shadow.STIBlocked, 1);
          EXPECT_EQ(Shadow.SSBlocked, 1);
          Events.Ready = 1;
          Events.Candidate = {2,
                              3}; // NMI may not split an admitted instruction.
          Budget = 0;
          EXPECT_EQ(Execute(Context), 9u) << Context.lastError();
          EXPECT_EQ(Events.Polls, 1u);
          EXPECT_EQ(Events.Acks, 0u);
          Budget = 1;
          EXPECT_EQ(Execute(Context), 18u) << Context.lastError();
          EXPECT_EQ(Budget, 1u);
          auto Repaired = repair(*Memory, 0x50000, 0x30000);
          X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
          EXPECT_EQ(Execute(Resumed), 9u) << Resumed.lastError();
          EXPECT_EQ(Budget, 0u);
          EXPECT_EQ(Events.Polls, 1u);
          EXPECT_EQ(Events.Acks, 0u);
          EXPECT_EQ(Shadow.STIBlocked, 0);
          EXPECT_EQ(Shadow.SSBlocked, 0);
          EXPECT_EQ(State.NMIBlocked, 0u);
          EXPECT_TRUE(Resumed.lastResult().InstructionAttempted);
          EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
          EXPECT_TRUE(Resumed.lastResult().InstructionResumed);
          EXPECT_FALSE(Resumed.continuation());
          EXPECT_EQ(Core.PC, 0x1000u + Forms[Form].size());
          if (Form == 1)
            EXPECT_EQ(Core.GPR[4], 0x20008u);
          if (Form == 4)
            EXPECT_EQ(Core.GPR[4], 0x20000u);
          if (Form == 2)
            EXPECT_EQ(Core.GPR[0], 0x12345678u);
        }
}

TEST_F(X64PagedSegmentContinuationTest,
       ResumeRejectsChangedBindingsBeforeEffects) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (unsigned Form : {0u, 4u})
    for (bool LLVM : {false, true}) {
      const auto Code = aliasCode(Form);
      Run Driver = nullptr, OtherDriver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(
          Code, LLVM, "-O2", Driver, {0x1000}, NEVERD_TEST_CLANG, 0x1000,
          &Scheduled, X64NullSegmentBasePolicy::Clear, false, true));
      ASSERT_NO_FATAL_FAILURE(compile(
          Code, LLVM, "-O2", OtherDriver, {0x1000}, NEVERD_TEST_CLANG, 0x1000,
          nullptr, X64NullSegmentBasePolicy::Preserve, false, true));
      const auto Page = alias(Form);
      Memory = without(Page);
      X64PagedDebugState Debug;
      auto Options = options(&Debug);
      X64PagedExecution Context(*Memory, Options);
      uint64_t Budget = 1;
      ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                18u)
          << Context.lastError();
      const auto Pending = Context.continuation();
      ASSERT_TRUE(Pending);
      auto Repaired = repair(*Memory, Page, Page);
      for (unsigned Case = 0; Case != 17; ++Case) {
        SCOPED_TRACE(
            llvm::formatv("form={0} llvm={1} case={2}", Form, LLVM, Case)
                .str());
        const auto Before = State;
        const auto BeforeDebug = Debug;
        const auto BeforeControls = Repaired->controls();
        const auto BeforeRAM = bytes(*Repaired);
        auto Binding = Options;
        switch (Case) {
        case 0:
          ++State.Core.Core.Core.Core.GPR[15];
          break;
        case 1:
          ++State.Core.Core.Core.Core.ZMM[31][63];
          break;
        case 2:
          ++State.Core.Core.Core.Core.Undefined[7];
          break;
        case 3:
          State.NMIBlocked = 1;
          break;
        case 4:
          ++State.Core.Core.Core.Core.FSBase;
          break;
        case 5:
          ++State.GDTR.Base;
          break;
        case 6:
          ++State.LDTR.Base;
          break;
        case 7:
          ++Debug.DR[0];
          break;
        case 8:
          Binding.DebugProfile = X86InstructionDebugProfile::InstructionStep;
          break;
        case 9:
          Binding.AlignmentPriority = X64PagedAlignmentPriority::BeforePaging;
          break;
        case 10:
          Binding.FarPointerProfile.reset();
          break;
        case 11:
          Binding.SegmentMoveProfile.reset();
          break;
        case 12: {
          auto Changed = BeforeControls;
          Changed.CR3 += 4096;
          ASSERT_FALSE(bool(Repaired->bindControls(Changed)));
          break;
        }
        default:
          break;
        }
        X64PagedExecution Refused(*Repaired, Binding, Pending);
        if (Case == 13 || Case == 14) {
          X64ContinuationEventV1 Event;
          Event.Vector = 2;
          Event.Kind = X64ContinuationEventKind::NMI;
          if (Case == 13) {
            auto Got = Refused.deliver(State, Event, 0xffff);
            ASSERT_FALSE(bool(Got));
            llvm::consumeError(Got.takeError());
          } else {
            X64PagedEventContinuation EventProgress;
            EventProgress.Event = Event;
            auto Got = Refused.resume(State, EventProgress, 0xffff);
            ASSERT_FALSE(bool(Got));
            llvm::consumeError(Got.takeError());
          }
        } else if (Case == 16) {
          Shadows Shadow;
          Shadow.SSBlocked = 1;
          Producer Events;
          uint64_t Polls = 1;
          EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Refused),
                              &Budget, &Shadow, &Polls, Producer::peek,
                              Producer::accepted, &Events),
                    5u);
          EXPECT_EQ(Events.Polls, 0u);
          EXPECT_EQ(Events.Acks, 0u);
        } else {
          EXPECT_EQ((Case == 15 ? OtherDriver : Driver)(
                        &State, reinterpret_cast<uint8_t *>(&Refused), &Budget),
                    5u);
          EXPECT_FALSE(Refused.lastError().empty());
        }
        EXPECT_EQ(Refused.continuation(), Pending);
        EXPECT_FALSE(Refused.lastResult().InstructionAttempted);
        EXPECT_FALSE(Refused.lastResult().InstructionResumed);
        EXPECT_EQ(Budget, 1u);
        EXPECT_EQ(bytes(*Repaired), BeforeRAM);
        State = Before;
        Debug = BeforeDebug;
        ASSERT_FALSE(bool(Repaired->bindControls(BeforeControls)));
      }
      X64PagedExecution Resumed(*Repaired, Options, Pending);
      EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
                9u)
          << Resumed.lastError();
      EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
      EXPECT_EQ(Budget, 0u);
    }
}

TEST_F(X64PagedSegmentContinuationTest, DebugAndSSStateSurviveEventHandoff) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<std::vector<uint8_t>> Forms{
      {0x8e, 0x27}, {0x8e, 0x17}, {0x0f, 0xb4, 0x07}, {0x0f, 0xb2, 0x07}};
  for (unsigned Form = 0; Form != Forms.size(); ++Form)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true})
        for (const std::string O : {"-O0", "-O2"}) {
          auto Code = Forms[Form];
          Code.push_back(0x90); // Consume a new MOV-SS shadow before #DB.
          Run Driver = nullptr;
          Schedule Scheduled = nullptr;
          ASSERT_NO_FATAL_FAILURE(compile(
              Code, LLVM, O, Driver, {0x1000, 0x1000 + Forms[Form].size()},
              Compiler, 0x1000, &Scheduled, X64NullSegmentBasePolicy::Clear,
              false, true));
          for (bool Incoming : {false, true})
            for (bool PauseEvent : {false, true}) {
              SCOPED_TRACE(
                  llvm::formatv(
                      "form={0} incoming={1} event={2} {3} llvm={4} {5}", Form,
                      Incoming, PauseEvent, Compiler, LLVM, O)
                      .str());
              reset(Code);
              State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
              auto &Core = State.Core.Core.Core.Core;
              Core.RFlags =
                  0x10102; // Entry TF and RF, RF clears on completion.
              put(0x30000, Form >= 2 ? 0x001012345678ULL : 16);
              put(0x13028, 0x50007); // Descriptor backing is initially absent.
              if (PauseEvent)
                put(0x13030, 0x60007);
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              X64PagedDebugState Debug;
              Debug.DR[0] = 0x20000;
              Debug.DR7 |=
                  0x30001; // Read/write one byte of the pointer/selector.
              const auto BeforeDebug = Debug;
              auto Options = options(&Debug);
              X64PagedExecution Context(*Memory, Options);
              Shadows Shadow;
              Shadow.SSBlocked = Incoming;
              Producer Events;
              Events.Ready = 0;
              uint64_t Budget = 1, Polls = 1;
              const auto Execute = [&](X64PagedExecution &Current) {
                return Scheduled(&State, reinterpret_cast<uint8_t *>(&Current),
                                 &Budget, &Shadow, &Polls, Producer::peek,
                                 Producer::accepted, &Events);
              };
              ASSERT_EQ(Execute(Context), 18u) << Context.lastError();
              EXPECT_EQ(Budget, 1u);
              EXPECT_EQ(Debug, BeforeDebug);
              EXPECT_EQ(Shadow.SSBlocked, Incoming);
              auto Repaired = repair(*Memory, 0x50000, 0x5000);
              X64PagedExecution Resumed(*Repaired, Options,
                                        Context.continuation());
              const bool NewShadow = Form == 1 && !Incoming;
              EXPECT_EQ(Execute(Resumed), NewShadow    ? 9u
                                          : PauseEvent ? 18u
                                                       : 7u)
                  << Resumed.lastError();
              EXPECT_EQ(Budget, 0u);
              EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
              EXPECT_TRUE(Resumed.lastResult().InstructionAttempted);
              EXPECT_TRUE(Resumed.lastResult().InstructionResumed);
              EXPECT_EQ(Resumed.lastResult().StartsSSShadow, NewShadow);
              EXPECT_FALSE(Resumed.continuation());
              EXPECT_FALSE(Core.RFlags & 0x10000);
              if (NewShadow) {
                EXPECT_EQ(Shadow.SSBlocked, 1);
                EXPECT_EQ(Debug.PendingData, 1u);
                EXPECT_EQ(Debug.DR6, BeforeDebug.DR6);
                Budget = 1;
                Polls = 1;
                EXPECT_EQ(Execute(Resumed), PauseEvent ? 18u : 7u)
                    << Resumed.lastError();
                EXPECT_EQ(Budget, 0u);
              }
              EXPECT_EQ(Shadow.SSBlocked, 0);
              EXPECT_EQ(Debug.DR6 & 0x4001, 0x4001u);
              EXPECT_EQ(Debug.PendingData, 0u);
              if (PauseEvent) {
                ASSERT_TRUE(Resumed.lastResult().Delivery);
                ASSERT_TRUE(Resumed.lastResult().Delivery->Continuation);
                auto EventMemory = repair(*Repaired, 0x60000, 0x6000);
                X64PagedExecution EventContext(*EventMemory, Options);
                const auto PublishedDebug = Debug;
                auto Event = EventContext.resume(
                    State, *Resumed.lastResult().Delivery->Continuation,
                    0xffff);
                ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
                EXPECT_EQ(Event->Exit, 7u);
                EXPECT_FALSE(Event->InstructionAttempted);
                EXPECT_FALSE(Event->InstructionCompleted);
                EXPECT_EQ(Debug, PublishedDebug);
              }
              EXPECT_EQ(Core.PC, 0x4010u);
            }
        }
}

TEST_F(X64PagedSegmentContinuationTest, GuestFaultRetiresBeforeRetainedEvent) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (unsigned Form : {0u, 3u})
    for (bool LLVM : {false, true})
      for (unsigned Fault = 0; Fault != 3; ++Fault) {
        const auto Code =
            Form == 0 ? aliasCode(0) : std::vector<uint8_t>{0x0f, 0xb2, 0x07};
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(
            Code, LLVM, "-O2", Driver, {0x1000}, NEVERD_TEST_CLANG, 0x1000,
            nullptr, X64NullSegmentBasePolicy::Clear, false, true));
        reset(Code);
        State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
        auto &Core = State.Core.Core.Core.Core;
        const auto BeforeRAX = Core.GPR[0];
        const auto BeforeFS = State.FS;
        const auto BeforeSS = State.Core.SS;
        Core.RFlags = 0x102;
        put(0x30000, Form == 0 ? 16 : 0x001012345678ULL);
        if (Fault == 0)
          put(0x5010, 0x00cf19000000ffffULL); // Wrong type precedes present.
        else if (Fault == 1)
          put(0x5010, 0x00cf13000000ffffULL); // Nonpresent data: #NP or #SS.
        else
          State.GDTR.Limit = 0x10; // The full descriptor exceeds the table.
        put(0x13100, 0x50007);     // First stop at the explicit operand.
        put(0x13030, 0x60007);     // Guest fault then stops at IDT backing.
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedDebugState Debug;
        auto Options = options(&Debug);
        X64PagedExecution Context(*Memory, Options);
        uint64_t Budget = 1;
        ASSERT_EQ(
            Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget), 18u)
            << Context.lastError();
        auto Repaired = repair(*Memory, 0x50000, 0x30000);
        X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
        ASSERT_EQ(
            Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget), 18u)
            << Resumed.lastError();
        EXPECT_EQ(Budget, 0u);
        EXPECT_TRUE(Resumed.lastResult().InstructionAttempted);
        EXPECT_TRUE(Resumed.lastResult().InstructionResumed);
        EXPECT_FALSE(Resumed.lastResult().InstructionCompleted);
        EXPECT_FALSE(Resumed.continuation());
        EXPECT_EQ(Core.GPR[0], BeforeRAX);
        EXPECT_EQ(State.FS, BeforeFS);
        EXPECT_EQ(State.Core.SS, BeforeSS);
        ASSERT_TRUE(Resumed.lastResult().Exception);
        const unsigned Vector = Fault == 1 ? (Form == 0 ? 11 : 12) : 13;
        EXPECT_EQ(Resumed.lastResult().Exception->Vector, Vector);
        EXPECT_EQ(Resumed.lastResult().Exception->ErrorCode, 0x10u);
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
        EXPECT_EQ(Core.PC, 0x4000u + 16 * Vector);
      }
}

TEST_F(X64PagedSegmentContinuationTest, NullPolicyRefusalKeepsIncomingShadows) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const std::vector<uint8_t> Code :
       {std::vector<uint8_t>{0x8e, 0x27}, {0x0f, 0xa1}, {0x0f, 0xb4, 0x07}})
    for (bool LLVM : {false, true}) {
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                      NEVERD_TEST_CLANG, 0x1000, &Scheduled,
                                      std::nullopt, false, true));
      reset(Code);
      State.FS = {UINT32_MAX, 16, 0xc093};
      State.Core.Core.Core.Core.FSBase = 0xffff800000022000ULL;
      State.Core.Core.Core.Core.RFlags = 0x202;
      if (Code == std::vector<uint8_t>{0x0f, 0xa1})
        State.Core.Core.Core.Core.GPR[4] = 0x20000;
      put(0x30000, 0); // Null selector, with no declared base action.
      put(0x13100, 0x50007);
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedDebugState Debug;
      auto Options = options(&Debug);
      X64PagedExecution Context(*Memory, Options);
      Shadows Shadow;
      Shadow.STIBlocked = Shadow.SSBlocked = 1;
      Producer Events;
      Events.Ready = 0;
      uint64_t Budget = 1, Polls = 1;
      const auto Execute = [&](X64PagedExecution &Current) {
        return Scheduled(&State, reinterpret_cast<uint8_t *>(&Current), &Budget,
                         &Shadow, &Polls, Producer::peek, Producer::accepted,
                         &Events);
      };
      const auto Before = State;
      const auto BeforeDebug = Debug;
      ASSERT_EQ(Execute(Context), 18u) << Context.lastError();
      auto Repaired = repair(*Memory, 0x50000, 0x30000);
      X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
      EXPECT_EQ(Execute(Resumed), 5u) << Resumed.lastError();
      EXPECT_EQ(State, Before);
      EXPECT_EQ(Debug, BeforeDebug);
      EXPECT_EQ(Budget, 1u);
      EXPECT_EQ(Shadow.STIBlocked, 1);
      EXPECT_EQ(Shadow.SSBlocked, 1);
      EXPECT_EQ(Events.Polls, 1u);
      EXPECT_EQ(Events.Acks, 0u);
      EXPECT_FALSE(Resumed.lastResult().InstructionAttempted);
      EXPECT_FALSE(Resumed.lastResult().InstructionCompleted);
      EXPECT_FALSE(Resumed.lastResult().InstructionResumed);
      EXPECT_FALSE(Resumed.continuation());
    }
}

TEST_F(X64PagedSegmentContinuationTest, AccessedWriteBackingRetainsProgress) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (unsigned Case : {0u, 5u})
    for (bool LLVM : {false, true}) {
      const auto Code = aliasCode(Case);
      const uint64_t Address = Case == 5 ? 0x1028 : 0x1000;
      Run Driver = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {Address},
                                      NEVERD_TEST_CLANG, Address, nullptr,
                                      X64NullSegmentBasePolicy::Clear));
      const auto Page = alias(Case);
      auto Hole = without(Page);
      auto Snapshot = RAM;
      Snapshot.Memory = Hole->physicalMemory().snapshotMemoryRegions();
      const auto &Original = RAM.Memory.front().Bytes;
      Snapshot.Memory.push_back(
          {Page,
           MemoryPermission::Read,
           0,
           {Original.begin() + Page, Original.begin() + Page + 4096}});
      Memory =
          llvm::cantFail(X86PagedMemory::create(Snapshot, Features, Controls));
      auto Options = options();
      X64PagedExecution Context(*Memory, Options);
      uint64_t Budget = 1;
      ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                18u)
          << Context.lastError();
      EXPECT_EQ(Budget, 1u);
      EXPECT_FALSE(Context.lastResult().InstructionAttempted);
      EXPECT_EQ(Context.pendingProgress(), kX64PagedInstructionPending);
      const uint64_t Descriptor = Page + (Case == 5 ? 0x10 : 0x18);
      EXPECT_FALSE(Memory->physicalMemory()
                       .loadScalar(Descriptor, GuestScalarWidth::I64)
                       .Value &
                   (uint64_t(1) << 40));
      const auto BeforeRAM = bytes(*Memory);
      Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
      for (auto &Region : Snapshot.Memory)
        if (Region.Address == Page)
          Region.Permissions = MemoryPermission::Read | MemoryPermission::Write;
      auto Repaired = llvm::cantFail(X86PagedMemory::create(
          Snapshot, Memory->features(), Memory->controls()));
      EXPECT_EQ(bytes(*Repaired), BeforeRAM);
      X64PagedExecution Resumed(*Repaired, Options, Context.continuation());
      EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
                9u)
          << Resumed.lastError();
      EXPECT_EQ(Budget, 0u);
      EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
      EXPECT_TRUE(Resumed.lastResult().InstructionResumed);
      EXPECT_EQ(State.Core.Core.Core.Core.FSBase, 0x22000u);
      EXPECT_EQ(State.Core.Core.Core.Core.PC, Address + Code.size());
      EXPECT_EQ(State.FS.Selector, Case == 5 ? 0x10 : 0x1b);
      EXPECT_TRUE(Repaired->physicalMemory()
                      .loadScalar(Descriptor, GuestScalarWidth::I64)
                      .Value &
                  (uint64_t(1) << 40));
    }
}

TEST_F(X64PagedSegmentContinuationTest,
       FullModeOwnerKeepsUnpaidSegmentProgress) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (unsigned Case : {0u, 5u})
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true}) {
        const auto Code = aliasCode(Case);
        const uint64_t Address = Case == 5 ? 0x1028 : 0x1000;
        llvm::json::Object Settings{
            {"profile", "kernel-events-v6"},
            {"mxcsr_mask", "0xffff"},
            {"paging_address_bits", 48},
            {"paged_memory", true},
            {"scheduled_events", true},
            {"instructions", llvm::json::Array{Address}},
            {"null_segment_base_policy", "clear"}};
        const auto Request =
            llvm::formatv("{0}", llvm::json::Value(std::move(Settings))).str();
        auto Export =
            exportX64Continuation(image(Code, Address), Request, LLVM);
        ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
        const auto Manifest =
            llvm::cantFail(llvm::json::parse(Export->Manifest));
        const auto *Units = Manifest.getAsObject()->getArray("units");
        ASSERT_NE(Units, nullptr);
        ASSERT_EQ(Units->size(), 1u);
        const auto Symbol = Units->front().getAsObject()->getString("name");
        ASSERT_TRUE(Symbol);
        const auto Adapters =
            "\nuint64_t mode_body(void *s, void *e) { return " + Symbol->str() +
            "_paged_adapter(s,e); }\n" +
            "uint64_t mode_validate(void *s, void *e) { return " +
            Symbol->str() + "_validate_paged_adapter(s,e); }\n";
        llvm::sys::DynamicLibrary DL;
        const auto Name = std::string("segment-mode-") +
                          (LLVM ? "llvm" : "high") + "-" +
                          std::to_string(Compilation++);
        ASSERT_NO_FATAL_FAILURE(
            compileProgram(*Export, Name, Compiler, "-O2", DL, Adapters));
        X86ModeInstruction Instruction{Address, X86ExecutionMode::Long64, Code};
        Instruction.Body =
            reinterpret_cast<X64PagedBody>(DL.getAddressOfSymbol("mode_body"));
        Instruction.Validate = reinterpret_cast<X64PagedBody>(
            DL.getAddressOfSymbol("mode_validate"));
        ASSERT_NE(Instruction.Body, nullptr);
        ASSERT_NE(Instruction.Validate, nullptr);
        const auto Page = alias(Case);
        X86ModeState Machine;
        Machine.Core.Core = State;
        Machine.SSBlocked = true;
        X86ModeExecutionOptions Options;
        Options.Long = options();
        Options.MXCSRMask = 0xffff;
        Options.PagingAddressBits = 48;
        Options.Undefined.emplace();
        Options.Legacy.Segments.FullLimitChoice =
            X86FullSegmentLimitChoice::Wrap;
        Memory = without(Page);
        X86ModeExecution Context(*Memory, Options);
        uint64_t Budget = 1;
        auto First = Context.run(Machine, {Instruction}, Budget);
        ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
        EXPECT_TRUE(First->OwnerError.empty()) << First->OwnerError;
        EXPECT_EQ(First->Status, X86LegacyExecutionStatus::BackingFault);
        EXPECT_FALSE(First->InstructionAttempted);
        EXPECT_EQ(Budget, 1u);
        ASSERT_TRUE(Machine.Pending);
        const auto Pending = Machine.Pending;
        auto Interrupted = Context.interrupt(Machine, X86EventKind::NMI, 2);
        ASSERT_FALSE(bool(Interrupted));
        llvm::consumeError(Interrupted.takeError());
        EXPECT_EQ(Machine.Pending, Pending);
        EXPECT_TRUE(Machine.SSBlocked);
        Budget = 0;
        auto Stopped = Context.run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Stopped)) << llvm::toString(Stopped.takeError());
        EXPECT_TRUE(Stopped->BudgetExhausted);
        EXPECT_EQ(Machine.Pending, Pending);
        auto Repaired = repair(*Memory, Page, Page);
        X86ModeExecution Resumed(*Repaired, Options);
        Budget = 1;
        auto Done = Resumed.run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
        EXPECT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
        EXPECT_EQ(Done->Attempts, 1u);
        EXPECT_EQ(Done->Completions, 1u);
        EXPECT_EQ(Budget, 0u);
        EXPECT_FALSE(Machine.Pending);
        EXPECT_FALSE(Machine.SSBlocked);
        EXPECT_EQ(Machine.Core.Core.Core.Core.Core.Core.PC,
                  Address + Code.size());
        EXPECT_EQ(Machine.Core.Core.Core.Core.Core.Core.FSBase, 0x22000u);
      }
}

TEST_F(X64PagedSegmentContinuationTest,
       DirectOwnerFreezesStateAndPagingFeatures) {
  for (unsigned Case : {0u, 4u}) {
    const auto Code = aliasCode(Case);
    const auto Page = alias(Case);
    const auto Before = State;
    Memory = without(Page);
    auto First =
        Case == 0
            ? moveX64PagedSegment(State, *Memory, Code,
                                  X64PagedAlignmentPriority::AfterPaging,
                                  X64PagedSegmentMoveProfile::SelectorFirst,
                                  X64NullSegmentBasePolicy::Clear)
            : loadX64PagedFarPointer(State, *Memory, Code,
                                     X64PagedAlignmentPriority::AfterPaging,
                                     X64PagedFarPointerProfile::OffsetFirst,
                                     X64NullSegmentBasePolicy::Clear);
    ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
    ASSERT_TRUE(First->BackingFault);
    ASSERT_TRUE(First->Continuation);
    EXPECT_FALSE(First->InstructionAttempted);
    EXPECT_EQ(State, Before);
    const auto Pending = First->Continuation;
    const auto StoppedRAM = bytes(*Memory);
    ++State.Core.Core.Core.Core.GPR[15];
    auto Refused = resumeX64PagedSegment(State, *Memory, Pending);
    ASSERT_FALSE(bool(Refused));
    llvm::consumeError(Refused.takeError());
    EXPECT_EQ(bytes(*Memory), StoppedRAM);
    State = Before;
    auto Changed = Memory->features();
    Changed.PSE = !Changed.PSE;
    auto Snapshot = RAM;
    Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
    auto Other = llvm::cantFail(
        X86PagedMemory::create(Snapshot, Changed, Memory->controls()));
    auto WrongFeatures = resumeX64PagedSegment(State, *Other, Pending);
    ASSERT_FALSE(bool(WrongFeatures));
    llvm::consumeError(WrongFeatures.takeError());
    EXPECT_EQ(bytes(*Other), StoppedRAM);
    EXPECT_EQ(State, Before);
    auto Again = resumeX64PagedSegment(State, *Memory, Pending);
    ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
    ASSERT_TRUE(Again->Continuation);
    EXPECT_NE(Again->Continuation, Pending);
    EXPECT_TRUE(Again->BackingFault);
    EXPECT_FALSE(Again->InstructionAttempted);
    EXPECT_EQ(bytes(*Memory), StoppedRAM);
    auto Repaired = repair(*Memory, Page, Page);
    auto Done = resumeX64PagedSegment(State, *Repaired, Again->Continuation);
    ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
    EXPECT_TRUE(Done->Completed);
    EXPECT_TRUE(Done->InstructionAttempted);
    EXPECT_FALSE(Done->Continuation);
    EXPECT_EQ(State.Core.Core.Core.Core.FSBase, 0x22000u);
    if (Case == 4)
      EXPECT_EQ(State.Core.Core.Core.Core.GPR[0], 0x30003u);
  }
}
