//===- X64PagedBranchDebugTests.inc - Generated branch stepping ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original branch decisions and software qualification of the debug bank,
/// v1 refusal, REP grouping, event effects and resumable entry.
//===----------------------------------------------------------------------===//

class X64PagedBranchDebugTest : public X64PagedDebugTest {
protected:
  X64PagedExecutionOptions branchOptions(X64PagedDebugState &Debug) {
    auto O = options(Debug);
    O.DebugProfile = X86InstructionDebugProfile::BranchWatchpoints;
    O.GeneralDetectFrame = X86GeneralDetectFrame::FaultResume;
    O.RepeatDebug.emplace();
    return O;
  }

  uint64_t saved(unsigned Offset = 0) {
    auto Got = Memory->physicalMemory().loadScalar(
        State.Core.Core.Core.Core.GPR[4] + Offset, GuestScalarWidth::I64);
    EXPECT_EQ(Got.Status, GuestMemoryAccessStatus::Completed);
    return Got.Value;
  }
};

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedBranchDebugTest, OriginalBranchSelectionThroughBothCRoutes) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  neverd::test::X86BranchDebugOracle CPU;
  ASSERT_NO_FATAL_FAILURE(
      CPU.initialize(neverd::test::X86BranchDebugOracle::Mode::Long64));
  auto Cases = neverd::test::x86BranchDebugCases(false);
  for (uint8_t Opcode : {0xe0, 0xe1, 0xe2})
    for (uint64_t Count : {0ULL, 1ULL, 2ULL, 0x100000001ULL})
      for (bool ZF : {false, true}) {
        const bool Taken =
            uint32_t(Count - 1) && (Opcode == 0xe2 || (Opcode == 0xe1) == ZF);
        Cases.push_back(
            {{0x67, Opcode, 0, 0x90, 0xeb, 0, 0xcc},
             Taken ? std::vector<unsigned>{3} : std::vector<unsigned>{3, 1, 2},
             uint64_t(0xa97 | (ZF ? 64 : 0)),
             Count});
      }
  std::vector<uint8_t> Code(Cases.size() * 32, 0xcc);
  std::vector<uint64_t> PCs;
  for (unsigned I = 0; I != Cases.size(); ++I) {
    std::copy(Cases[I].Code.begin(), Cases[I].Code.end(),
              Code.begin() + I * 32);
    unsigned Offset = I * 32;
    for (const auto Length : Cases[I].Lengths) {
      PCs.push_back(0x1000 + Offset);
      Offset += Length;
    }
  }
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const auto *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Comparisons = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string Optimization : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled, PCs,
                                             Compiler, Optimization));
        for (unsigned I = 0; I != Cases.size(); ++I)
          for (bool Branch : {false, true}) {
            SCOPED_TRACE(testing::Message()
                         << Compiler << " llvm=" << LLVM << Optimization
                         << " case=" << I << " branch=" << Branch);
            neverd::test::X86BranchDebugOracle::Result Native;
            ASSERT_NO_FATAL_FAILURE(CPU.run(Cases[I], Branch, Native));
            resetDebug(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = 0x1000 + I * 32;
            Core.RFlags = Cases[I].Flags | 0x100;
            Core.GPR[0] = 0;
            Core.GPR[1] = Cases[I].Count;
            const uint64_t Stack = Core.GPR[4], Start = Core.PC;
            put(Stack, Start + 4);
            X64PagedDebugState Debug;
            Debug.DebugCTL = Branch ? kX86BranchTraceFlag : 0;
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(*Memory, branchOptions(Debug));
            uint64_t Budget = Cases[I].Lengths.size();
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                7u)
                << Context.lastError();
            ASSERT_TRUE(Context.lastResult().InstructionCompleted);
            ASSERT_EQ(Context.lastResult().Delivery->DeliveredVector, 1);
            EXPECT_EQ(saved(), Start + Native.PC);
            EXPECT_EQ(Core.GPR[0], Native.Accumulator);
            EXPECT_EQ(Core.GPR[1], Native.Count);
            EXPECT_EQ(saved(16) & 0xcd5, Native.Flags & 0xcd5);
            EXPECT_EQ(int64_t(saved(24)) - int64_t(Stack), Native.StackDelta);
            EXPECT_EQ(Debug.DR6, 0xffff4ff0u);
            EXPECT_EQ(Debug.DebugCTL, 0u);
            EXPECT_EQ(Budget, Branch ? 0u : Cases[I].Lengths.size() - 1);
            ++Comparisons;
          }
      }
  RecordProperty("branch_selection_comparisons", Comparisons);
}
#endif

TEST_F(X64PagedBranchDebugTest, OldBridgesRefuseBeforeValidationOrFetch) {
  resetDebug({0x90});
  X64PagedDebugState Debug;
  Debug.DebugCTL = kX86BranchTraceFlag;
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X64PagedExecution Context(*Memory, branchOptions(Debug));
  const auto Before = State;
  auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
  auto *Bytes = reinterpret_cast<uint8_t *>(&State);
  const auto Unreachable = +[](void *, void *) -> uint64_t {
    ADD_FAILURE() << "Old body/validator ran with active BTF";
    return 0;
  };
  const uint8_t Nop = 0x90;
  for (bool TF : {true, false}) {
    State.Core.Core.Core.Core.RFlags = TF ? 0x302 : 0x202;
    EXPECT_EQ(neverd_x64_paged_debug_validate_v1(Opaque, Bytes, 0x1000,
                                                 Unreachable, 48),
              5u);
    EXPECT_EQ(neverd_x64_paged_debug_step_v1(Opaque, Bytes, 0x1000, &Nop, 1,
                                             Unreachable, Unreachable, 0xffff,
                                             48, 0),
              5u);
    EXPECT_EQ(neverd_x64_paged_step_v1(Opaque, Bytes, 0x1000, &Nop, 1,
                                       Unreachable, Unreachable, 0xffff, 48),
              5u);
    EXPECT_NE(Context.lastError().find("v2"), std::string::npos);
    EXPECT_EQ(neverd_x64_paged_progress_v1(Opaque), 0u);
    EXPECT_EQ(Debug.DebugCTL, 2u);
  }
  State.Core.Core.Core.Core.RFlags = Before.Core.Core.Core.Core.RFlags;
  EXPECT_EQ(std::memcmp(&Before, &State, sizeof(State)), 0);
  EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.Memory.front().Bytes);
}

TEST_F(X64PagedBranchDebugTest, ControlAdmissionPrecedesProducerPolling) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug({0xeb, 0}, LLVM, Driver, Scheduled));
    for (uint64_t Control : {1ULL, 2ULL, 3ULL, 0x8000ULL}) {
      resetDebug({0xeb, 0});
      State.Core.Core.Core.Core.RFlags &= ~uint64_t{0x100};
      const auto Before = State;
      X64PagedDebugState Debug;
      Debug.DebugCTL = Control;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, branchOptions(Debug));
      uint64_t Budget = 1, Polls = 1;
      Shadows Shadow;
      Producer P;
      P.Ready = 0;
      EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                          &Budget, &Shadow, &Polls, Producer::peek,
                          Producer::accepted, &P),
                Control == 2 ? 9u : 5u);
      EXPECT_EQ(Debug.DebugCTL, Control);
      EXPECT_EQ(Debug.DR6, 0xffff0ff0u);
      EXPECT_EQ(P.Polls, Control == 2 ? 1u : 0u);
      EXPECT_EQ(Budget, Control == 2 ? 0u : 1u);
      if (Control != 2) {
        EXPECT_EQ(std::memcmp(&Before, &State, sizeof(State)), 0);
        EXPECT_EQ(
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.Memory.front().Bytes);
      }
    }
  }
}

TEST_F(X64PagedBranchDebugTest, ExternalDebugClearsOnlyAfterAcceptance) {
  resetDebug({0x90});
  put(0x13030, 0x50067);
  X64PagedDebugState Debug;
  Debug.DebugCTL = 2;
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X64PagedExecution Context(*Memory, branchOptions(Debug));
  X64ContinuationEventV1 Event;
  Event.SavedPC = 0x1000;
  Event.Vector = 1;
  Event.Kind = X64ContinuationEventKind::Trap;
  Event.SSBlocked = 1;
  auto Got = llvm::cantFail(Context.deliver(State, Event, 0xffff));
  EXPECT_EQ(Got.Delivery->Status, X64PagedEventStatus::Deferred);
  EXPECT_EQ(Debug.DebugCTL, 2u);
  Event.SSBlocked = 0;
  Got = llvm::cantFail(Context.deliver(State, Event, 0xffff));
  EXPECT_EQ(Got.Delivery->Status, X64PagedEventStatus::BackingFault);
  EXPECT_EQ(Debug.DebugCTL, 0u);
  EXPECT_EQ(Debug.DR6, 0xffff0ff0u); // Cause remains the external producer's.
}

TEST_F(X64PagedBranchDebugTest, DebugCausesAndFaultsHaveSeparateBTFEffects) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<std::vector<uint8_t>> Forms{
      {0x90},       {0x48, 0x8b, 0x07}, {0xf1}, {0xcd, 1}, {0x0f, 0x0b},
      {0xff, 0xe0}, {0x0f, 0x21, 0xc0}};
  for (bool LLVM : {false, true})
    for (unsigned Case = 0; Case != Forms.size(); ++Case) {
      SCOPED_TRACE(testing::Message() << "llvm=" << LLVM << " case=" << Case);
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(
          compileDebug(Forms[Case], LLVM, Driver, Scheduled));
      resetDebug(Forms[Case]);
      X64PagedDebugState Debug;
      Debug.DebugCTL = 2;
      auto &Core = State.Core.Core.Core.Core;
      if (Case == 0) {
        Debug.DR[0] = Core.PC;
        Debug.DR7 |= 1;
      } else if (Case == 1) {
        Debug.DR[0] = Core.GPR[7];
        Debug.DR7 |= 0x30001;
      } else if (Case == 5) {
        Core.GPR[0] = 0x800000000000ULL;
      } else if (Case == 6) {
        Debug.DR7 |= 0x2000;
      }
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, branchOptions(Debug));
      uint64_t Budget = 1;
      EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                7u)
          << Context.lastError();
      ASSERT_TRUE(Context.lastResult().Delivery);
      EXPECT_EQ(Context.lastResult().Delivery->DeliveredVector, Case == 4 ? 6
                                                                : Case == 5
                                                                    ? 13
                                                                    : 1);
      EXPECT_EQ(Debug.DebugCTL, Case < 3 || Case == 6 ? 0u : 2u);
      EXPECT_EQ(Debug.DR6 & 0x4000, 0u);
      EXPECT_EQ(Debug.DR6 & 0x2000, Case == 6 ? 0x2000u : 0u);
      EXPECT_EQ(Budget, 0u);
    }
}

TEST_F(X64PagedBranchDebugTest, TakenBranchRetainsCompletionAcrossEntryStop) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug({0x74, 0}, LLVM, Driver, Scheduled));
    resetDebug({0x74, 0});
    State.Core.Core.Core.Core.RFlags |= 64;
    put(0x13030, 0x50067); // No physical backing for the IDT page.
    X64PagedDebugState Debug;
    Debug.DebugCTL = 2;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory, branchOptions(Debug));
    uint64_t Budget = 1;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u);
    EXPECT_EQ(Budget, 0u);
    EXPECT_TRUE(Context.lastResult().InstructionCompleted);
    EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x1002u);
    EXPECT_EQ(Debug.DebugCTL, 0u);
    EXPECT_EQ(Debug.DR6, 0xffff4ff0u);
    ASSERT_TRUE(Context.lastResult().Delivery);
    ASSERT_TRUE(Context.lastResult().Delivery->Continuation);
    ASSERT_EQ(Memory->physicalMemory()
                  .storeScalar(0x13030, GuestScalarWidth::I64, 0x6067)
                  .Status,
              GuestMemoryAccessStatus::Completed);
    const auto Resumed = llvm::cantFail(Context.resume(
        State, *Context.lastResult().Delivery->Continuation, 0xffff));
    EXPECT_EQ(Resumed.Delivery->DeliveredVector, 1);
    EXPECT_EQ(saved(), 0x1002u);
    EXPECT_EQ(Debug.DebugCTL, 0u);
    EXPECT_FALSE(Resumed.InstructionAttempted);
  }
}

TEST_F(X64PagedBranchDebugTest, RepeatGroupRetainsRawTFAndHasNoBS) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compileDebug({0xf3, 0xa4}, LLVM, Driver, Scheduled));
    resetDebug({0xf3, 0xa4});
    auto &Core = State.Core.Core.Core.Core;
    Core.GPR[1] = 3;
    Core.GPR[6] = 0x3000;
    Core.GPR[7] = 0x4000;
    put(0x3000, 0x332211);
    X64PagedDebugState Debug;
    Debug.DebugCTL = 2;
    Debug.DR[0] = 0x4000;
    Debug.DR7 |= 0x10001;
    auto O = branchOptions(Debug);
    unsigned Calls = 0;
    O.RepeatDebug = X86RepeatDebugBinding{
        [](void *Context, const X86RepeatDebugStep &Step) {
          ++*static_cast<unsigned *>(Context);
          EXPECT_NE(Step.Flags & 0x100, 0u);
          EXPECT_EQ(Step.DebugCTL, 2u);
          return Step.Count == 1;
        },
        &Calls};
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory, O);
    uint64_t Budget = 1;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              9u);
    EXPECT_EQ(Debug.PendingData, 1u);
    EXPECT_EQ(Debug.DebugCTL, 2u);
    Budget = 1;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              7u);
    EXPECT_EQ(Core.GPR[1], 1u);
    EXPECT_EQ(saved(), 0x1000u);
    EXPECT_FALSE(Context.lastResult().InstructionCompleted);
    EXPECT_EQ(Debug.DR6, 0xffff0ff1u);
    EXPECT_EQ(Debug.DebugCTL, 0u);
    EXPECT_EQ(Calls, 2u);
  }
}
