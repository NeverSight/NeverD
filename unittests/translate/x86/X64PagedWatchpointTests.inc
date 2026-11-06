//===- X64PagedWatchpointTests.inc - Data and port debug traps ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare original accesses, delayed causes and event frames with both C
/// routes.
//===----------------------------------------------------------------------===//

class X64PagedWatchpointTest : public X64PagedDebugTest {
protected:
  X64PagedExecutionOptions watchOptions(X64PagedDebugState &Debug) {
    auto O = options(Debug);
    O.DebugProfile = X86InstructionDebugProfile::Watchpoints;
    O.GeneralDetectFrame = X86GeneralDetectFrame::ClearResume;
    O.RepeatDebug.emplace();
    return O;
  }
#if defined(__linux__) && defined(__x86_64__)
  void compare(X64PagedNativeCPU &CPU, Schedule Scheduled,
               X64PagedDebugState &Debug, unsigned BudgetValue,
               const X86RepeatDebugBinding *RepeatDebug = nullptr) {
    CPU.DebugRegisters.emplace();
    CPU.DebugRegisters->dr6 = Debug.DR6;
    CPU.DebugRegisters->dr7 = Debug.DR7;
    std::copy(Debug.DR.begin(), Debug.DR.end(), CPU.DebugRegisters->db);
    X64ContinuationStateV6 Native;
    bool Fault = false;
    ASSERT_NO_FATAL_FAILURE(CPU.execute(RAM.Memory.front().Bytes, State,
                                        Controls.CR0, 0, Native, Fault, true,
                                        nullptr, Controls.CR4, true, false));
    ASSERT_TRUE(Fault);
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    auto Options = watchOptions(Debug);
    if (RepeatDebug)
      Options.RepeatDebug = *RepeatDebug;
    X64PagedExecution Context(*Memory, Options);
    Shadows Shadow;
    Producer P;
    P.Ready = 0;
    uint64_t Budget = BudgetValue, Polls = BudgetValue;
    ASSERT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context), &Budget,
                        &Shadow, &Polls, Producer::peek, Producer::accepted,
                        &P),
              7u)
        << Context.lastError();
    EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
    expectPagedSegmentCPU(State, Native, *Memory, CPU);
    EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
    EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
    EXPECT_EQ(Debug.PendingData, 0);
    for (unsigned I = 0; I != 4; ++I)
      EXPECT_EQ(Debug.DR[I], CPU.DebugRegisters->db[I]);
    EXPECT_EQ(Shadow.SSBlocked, 0u);
    const auto Actual =
        Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
    const auto Difference =
        std::mismatch(Actual.begin(), Actual.end(), CPU.RAM);
    EXPECT_EQ(Difference.first, Actual.end())
        << "RAM differs at " << std::hex << (Difference.first - Actual.begin());
  }
#endif
};

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedWatchpointTest, OriginalRangesAndSimultaneousCauses) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  CPU.PrefaultMemory = true;
  const std::vector<std::vector<uint8_t>> Forms{{0x8a, 0x07},
                                                {0x66, 0x8b, 0x07},
                                                {0x8b, 0x07},
                                                {0x48, 0x8b, 0x07},
                                                {0x88, 0x07},
                                                {0x66, 0x89, 0x07},
                                                {0x89, 0x07},
                                                {0x48, 0x89, 0x07},
                                                {0x48, 0xff, 0x07},
                                                {0x48, 0x83, 0x3f, 0x00},
                                                {0x48, 0x8d, 0x07},
                                                {0x64, 0x48, 0x8b, 0x07},
                                                {0x65, 0x48, 0x89, 0x07}};
  std::vector<uint8_t> Code(Forms.size() * 16, 0xf4);
  std::vector<uint64_t> PCs;
  for (unsigned I = 0; I != Forms.size(); ++I) {
    std::copy(Forms[I].begin(), Forms[I].end(), Code.begin() + I * 16);
    Code[I * 16 + Forms[I].size()] = 0xcd;
    Code[I * 16 + Forms[I].size() + 1] = 64;
    PCs.push_back(0x1000 + I * 16);
    PCs.push_back(PCs.back() + Forms[I].size());
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
        for (unsigned I = 0; I != Forms.size(); ++I)
          for (unsigned Len = 0; Len != 4; ++Len)
            for (unsigned Setup = 0; Setup != 12; ++Setup) {
              SCOPED_TRACE(testing::Message()
                           << Compiler << " llvm=" << LLVM << Optimization
                           << " form=" << I << " len=" << Len
                           << " setup=" << Setup);
              resetDebug(Code, Setup == 9 ? 3 : 0);
              auto &Core = State.Core.Core.Core.Core;
              Core.PC = 0x1000 + I * 16;
              Core.RFlags =
                  0x202 | (Setup == 6 ? 0x100 : 0) | (Setup == 7 ? 0x10000 : 0);
              Core.FSBase = 0x1000;
              Core.GSBase = 0x2000;
              if (I == 11)
                Core.GPR[7] -= Core.FSBase;
              if (I == 12)
                Core.GPR[7] -= Core.GSBase;
              const unsigned Slot = Setup % 4;
              X64PagedDebugState Debug;
              Debug.DR6 = 0xffffafff;
              Debug.DR7 = 0x400 |
                          (uint64_t(Setup == 2   ? 0
                                    : Setup == 3 ? 2
                                                 : 1)
                           << (Slot * 2)) |
                          (uint64_t((Len << 2) | (Setup == 4 ? 1 : 3))
                           << (16 + Slot * 4));
              Debug.DR[Slot] = Setup == 0   ? 0x1ffff
                               : Setup == 1 ? 0x20008
                                            : 0x20003;
              if (Setup == 5)
                Debug.DR[Slot] = 0x30000; // Physical alias must not match.
              if (Setup == 8) {
                const unsigned Next = (Slot + 1) % 4;
                Debug.DR7 |= uint64_t{1} << (Next * 2);
                Debug.DR[Next] = Core.PC + Forms[I].size();
              }
              if (Setup == 10)
                put(0x13100, 0); // Fault discards speculative matches.
              if (Setup == 11) {
                Core.GPR[7] += 0xfff;
                Debug.DR[Slot] = 0x20fff;
                put(0x13108, 0);
              }
              ASSERT_NO_FATAL_FAILURE(compare(CPU, Scheduled, Debug, 2));
              if (HasFailure())
                return;
              ++Comparisons;
            }
      }
  RecordProperty("original_comparisons", Comparisons);
}

TEST_F(X64PagedWatchpointTest, OriginalColdStackShadowRetainsSoftwareTrap) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x8e, 0x17, 0xcd, 64};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compileDebug(Code, LLVM, Driver, Scheduled, {0x1000, 0x1002}));
    // This must be the fresh vCPU's first instruction sequence. A prior run
    // would hide cold EPT faults during the software interrupt's IDT entry.
    X64PagedNativeCPU CPU;
    ASSERT_NO_FATAL_FAILURE(CPU.initialize());
    if (CPU.KVM < 0)
      GTEST_SKIP() << "KVM unavailable";
    if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
      GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
    CPU.PrefaultMemory = true;
    resetDebug(Code);
    State.Core.Core.Core.Core.RFlags = 0x202;
    put(0x30000, 16);
    put(0x5018, 0x00af9b000000ffffULL);
    put(0x6400, 0x4400 | (uint64_t{24} << 16) | (uint64_t{0x8e} << 40));
    X64PagedDebugState Debug;
    Debug.DR7 = 0xf0401;
    Debug.DR[0] = 0x20000;
    ASSERT_NO_FATAL_FAILURE(compare(CPU, Scheduled, Debug, 2));
    EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x4010u);
  }
  RecordProperty("original_comparisons", 2);
}

TEST_F(X64PagedWatchpointTest, OriginalStackShadowAndEventAccesses) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  CPU.PrefaultMemory = true;
  const std::vector<std::vector<std::vector<uint8_t>>> Sequences{
      {{0x8e, 0x17}, {0x90}, {0xcd, 64}},
      {{0x8e, 0x17}, {0xcd, 64}},
      {{0x8e, 0x17}, {0xcc}},
      {{0x8e, 0x17}, {0xcd, 1}},
      {{0x8e, 0x17}, {0x0f, 0x0b}},
      {{0x8e, 0x17}, {0xf1}},
      {{0x8e, 0x17}, {0x8e, 0x17}, {0xcd, 64}},
      {{0xcd, 64}},
      {{0x0f, 0x0b}},
      {{0x48, 0xcf}},
      {{0x48, 0xcb}}};
  unsigned Comparisons = 0;
  for (bool LLVM : {false, true})
    for (unsigned Case = 0; Case != Sequences.size(); ++Case) {
      std::vector<uint8_t> Code;
      std::vector<uint64_t> PCs;
      for (const auto &Bytes : Sequences[Case]) {
        PCs.push_back(0x1000 + Code.size());
        Code.insert(Code.end(), Bytes.begin(), Bytes.end());
      }
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled, PCs));
      for (unsigned TF = 0; TF != 2; ++TF)
        for (unsigned Target = 0; Target != 12; ++Target) {
          SCOPED_TRACE(testing::Message()
                       << "llvm=" << LLVM << " case=" << Case << " tf=" << TF
                       << " target=" << Target);
          // A breakpoint on INT 1's own IDT/GDT input recurs during #DB.
          // The dedicated shutdown test below covers that different endpoint.
          if (Case == 3 && (Target == 2 || Target == 3))
            continue;
          resetDebug(Code);
          auto &Core = State.Core.Core.Core.Core;
          Core.RFlags = 0x202 | (TF ? 0x100 : 0);
          put(0x30000, 0x10);
          // The original software/fault handler has a distinct descriptor so
          // its read can trigger once without recursively matching #DB entry.
          put(0x5018, 0x00af9b000000ffffULL);
          const unsigned V = Case == 2                  ? 3
                             : Case == 3                ? 1
                             : (Case == 4 || Case == 8) ? 6
                                                        : 64;
          put(0x6000 + V * 16, (0x4000 + V * 16) | (uint64_t(24) << 16) |
                                   (uint64_t(0x8e) << 40));
          if (Case >= 9) {
            put(0x1e000, 0x1800);
            put(0x1e008, 8);
            put(0x1e010, 0x202);
            put(0x1e018, 0x1d000);
            put(0x1e020, 16);
            RAM.Memory.front().Bytes[0x1800] = 0xcd;
            RAM.Memory.front().Bytes[0x1801] = 64;
            // Returns are compared at the original target via a second entry
            // below only when an operand/TF cause traps immediately.
            if (Target < 9 && !TF)
              continue;
          }
          X64PagedDebugState Debug;
          Debug.DR7 = 0xf0401;
          Debug.DR[0] =
              std::array<uint64_t, 12>{0x20000, 0x5010,  0x6000 + V * 16ull,
                                       0x5018,  0x13008, 0x1dff8,
                                       0x1dfe8, 0x1dfd8, 0x20008,
                                       0x1e000, 0x1e008, 0x1e010}[Target];
          // Nonmatching returns would need a generated target dispatcher.
          if (Case == 10 && Target == 11 && !TF)
            continue;
          ASSERT_NO_FATAL_FAILURE(
              compare(CPU, Scheduled, Debug, Sequences[Case].size()));
          if (HasFailure())
            return;
          ++Comparisons;
        }
    }
  RecordProperty("original_comparisons", Comparisons);
}
#endif

TEST_F(X64PagedWatchpointTest, DataCauseSurvivesDeliveryBackingStop) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0xff, 0x07};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled));
    resetDebug(Code);
    State.Core.Core.Core.Core.RFlags = 0x202;
    put(0x13030, 0x50067);
    X64PagedDebugState Debug;
    Debug.DR7 = 0x10401;
    Debug.DR[0] = 0x20000;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory, watchOptions(Debug));
    uint64_t Budget = 1;
    ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u)
        << Context.lastError();
    ASSERT_TRUE(Context.lastResult().InstructionCompleted);
    ASSERT_TRUE(Context.lastResult().Delivery->Continuation);
    EXPECT_EQ(Debug.DR6, 0xffff0ff1u);
    ASSERT_EQ(Memory->physicalMemory()
                  .storeScalar(0x13030, GuestScalarWidth::I64, 0x6067)
                  .Status,
              GuestMemoryAccessStatus::Completed);
    auto R = Context.resume(State, *Context.lastResult().Delivery->Continuation,
                            0xffff);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Exit, 7u);
    EXPECT_EQ(Debug.DR6, 0xffff0ff1u);
    EXPECT_EQ(Memory->physicalMemory()
                  .loadScalar(0x30000, GuestScalarWidth::I64)
                  .Value,
              0x8877665544332212ULL);
    EXPECT_EQ(
        Memory->physicalMemory()
            .loadScalar(State.Core.Core.Core.Core.GPR[4], GuestScalarWidth::I64)
            .Value,
        0x1003u);
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedWatchpointTest, OriginalRecursiveDebugEntryShutsDown) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  CPU.PrefaultMemory = true;
  const std::vector<uint8_t> Code{0xcd, 1};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled));
    resetDebug(Code);
    State.Core.Core.Core.Core.RFlags = 0x202;
    X64PagedDebugState Debug;
    Debug.DR7 = 0x30401;
    Debug.DR[0] = 0x6010;
    CPU.DebugRegisters.emplace();
    CPU.DebugRegisters->dr6 = Debug.DR6;
    CPU.DebugRegisters->dr7 = Debug.DR7;
    CPU.DebugRegisters->db[0] = Debug.DR[0];
    X64ContinuationStateV6 Native;
    bool Fault = false;
    ASSERT_NO_FATAL_FAILURE(CPU.execute(
        RAM.Memory.front().Bytes, State, Controls.CR0, Code.size(), Native,
        Fault, true, nullptr, Controls.CR4, true, false, true, true));
    ASSERT_EQ(CPU.Run->exit_reason, unsigned(KVM_EXIT_SHUTDOWN));
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory, watchOptions(Debug));
    uint64_t Budget = 1;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              19u)
        << Context.lastError();
    EXPECT_TRUE(Context.lastResult().InstructionCompleted);
    EXPECT_TRUE(Context.lastResult().Delivery->SoftwareCompleted);
  }
  // Shutdown is the endpoint under test; post-shutdown architectural state is
  // undefined and is not included in the full-state comparison count.
  RecordProperty("original_shutdowns", 2);
}

TEST_F(X64PagedWatchpointTest, OriginalReturnAndSelectorByteFootprints) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  CPU.PrefaultMemory = true;
  const std::vector<std::vector<uint8_t>> Forms{
      {0x48, 0xcf}, {0x48, 0xcb}, {0x8e, 0x17, 0x90}, {0x0f, 0xa1}};
  unsigned Comparisons = 0;
  for (bool LLVM : {false, true})
    for (unsigned Form = 0; Form != Forms.size(); ++Form) {
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      const std::vector<uint64_t> PCs =
          Form == 2 ? std::vector<uint64_t>{0x1000, 0x1002}
                    : std::vector<uint64_t>{0x1000};
      ASSERT_NO_FATAL_FAILURE(
          compileDebug(Forms[Form], LLVM, Driver, Scheduled, PCs));
      for (unsigned Offset = 0; Offset != 40; ++Offset) {
        SCOPED_TRACE(testing::Message() << "llvm=" << LLVM << " form=" << Form
                                        << " byte=" << Offset);
        resetDebug(Forms[Form]);
        put(0x30000, 0x10);
        put(0x1e000, Form >= 2 ? 0x10 : 0x1800);
        put(0x1e008, 8);
        put(0x1e010, 0x202);
        put(0x1e018, 0x1d000);
        put(0x1e020, 16);
        X64PagedDebugState Debug;
        Debug.DR7 = 0x30401;
        Debug.DR[0] = (Form == 2 ? 0x20000 : 0x1e000) + Offset;
        ASSERT_NO_FATAL_FAILURE(
            compare(CPU, Scheduled, Debug, Form == 2 ? 2 : 1));
        if (HasFailure())
          return;
        ++Comparisons;
      }
    }
  RecordProperty("original_comparisons", Comparisons);
}

TEST_F(X64PagedWatchpointTest, OriginalRepeatElementsRetainCausesAndRestartPC) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  CPU.PrefaultMemory = true;
  const auto *GroupText = std::getenv("NEVERD_TEST_REPEAT_DEBUG_GROUP_BYTES");
  if (!GroupText)
    GTEST_SKIP() << "REP watchpoint oracle requires an explicit group boundary";
  unsigned GroupBytes = std::string(GroupText) == "1" ? 1 : 16;
  ASSERT_TRUE(std::string(GroupText) == "1" || std::string(GroupText) == "16");
  X86RepeatDebugBinding Group;
  Group.Context = &GroupBytes;
  Group.EndGroup = [](void *Context, const X86RepeatDebugStep &Step) {
    if (Step.Compare || !Step.DestinationIndex || (Step.Flags & 0x400))
      return true;
    return Step.Destination % *static_cast<unsigned *>(Context) == 0;
  };
  unsigned Comparisons = 0;
  for (bool LLVM : {false, true})
    for (uint8_t Opcode : {0xa4, 0xa6, 0xaa, 0xac, 0xae}) {
      const std::vector<uint8_t> Code{0xf3, Opcode, 0xcd, 64};
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(
          compileDebug(Code, LLVM, Driver, Scheduled, {0x1000, 0x1002}));
      for (unsigned Setup = 0; Setup != 10; ++Setup) {
        SCOPED_TRACE(testing::Message()
                     << "llvm=" << LLVM << " opcode=" << unsigned(Opcode)
                     << " setup=" << Setup);
        resetDebug(Code);
        auto &Core = State.Core.Core.Core.Core;
        Core.GPR[0] = 0x11;
        Core.GPR[1] = Setup == 0 ? 0 : 3;
        Core.GPR[6] = 0x20000;
        Core.GPR[7] = 0x22000;
        Core.RFlags =
            0x202 | (Setup == 1 ? 0x100 : 0) | (Setup == 2 ? 0x10000 : 0);
        put(0x30000, 0x111111);
        put(0x22000, 0x111111);
        X64PagedDebugState Debug;
        Debug.DR7 = 0x30401;
        Debug.DR[0] = (Setup < 5 ? 0x20000 : 0x22000) + (Setup % 3);
        ASSERT_NO_FATAL_FAILURE(compare(CPU, Scheduled, Debug, 4, &Group));
        if (HasFailure())
          return;
        ++Comparisons;
      }
    }
  RecordProperty("original_comparisons", Comparisons);
}

TEST_F(X64PagedWatchpointTest, OriginalRepeatGroupsKeepAllCompletedEffects) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const auto *GroupText = std::getenv("NEVERD_TEST_REPEAT_DEBUG_GROUP_BYTES");
  if (!GroupText)
    GTEST_SKIP() << "REP watchpoint oracle requires an explicit group boundary";
  ASSERT_TRUE(std::string(GroupText) == "1" || std::string(GroupText) == "16");
  unsigned GroupBytes = std::string(GroupText) == "1" ? 1 : 16;
  X86RepeatDebugBinding Group;
  Group.Context = &GroupBytes;
  Group.EndGroup = [](void *Context, const X86RepeatDebugStep &Step) {
    return (Step.Flags & 0x400) ||
           Step.Destination % *static_cast<unsigned *>(Context) == 0;
  };
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  CPU.PrefaultMemory = true;
  unsigned Comparisons = 0;
  for (bool LLVM : {false, true})
    for (unsigned Width : {1, 2, 4, 8})
      for (bool Move : {false, true}) {
        std::vector<uint8_t> Code{0xf3};
        if (Width == 2)
          Code.push_back(0x66);
        if (Width == 8)
          Code.push_back(0x48);
        Code.push_back((Move ? 0xa4 : 0xaa) + (Width != 1));
        const auto NextPC = 0x1000 + Code.size();
        Code.insert(Code.end(), {0xcd, 64});
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(
            compileDebug(Code, LLVM, Driver, Scheduled, {0x1000, NextPC}));
        for (unsigned Count : {3, 17, 33})
          for (unsigned Offset : {0, 1, 15, 16, 32})
            for (bool Backwards : {false, true}) {
              SCOPED_TRACE(testing::Message()
                           << "llvm=" << LLVM << " width=" << Width << " move="
                           << Move << " count=" << Count << " offset=" << Offset
                           << " backwards=" << Backwards);
              resetDebug(Code);
              auto &Core = State.Core.Core.Core.Core;
              Core.RFlags = 0x202 | (Backwards ? 0x400 : 0);
              Core.GPR[1] = Count;
              Core.GPR[6] = 0x20000 + (Backwards ? (Count - 1) * Width : 0);
              Core.GPR[7] = 0x22000 + (Backwards ? (Count - 1) * Width : 0);
              X64PagedDebugState Debug;
              Debug.DR7 = 0x30401;
              Debug.DR[0] = (Move ? 0x20000 : 0x22000) + Offset * Width;
              ASSERT_NO_FATAL_FAILURE(
                  compare(CPU, Scheduled, Debug, Count + 1, &Group));
              if (HasFailure())
                return;
              ++Comparisons;
            }
      }
  RecordProperty("original_comparisons", Comparisons);
}
#endif

TEST_F(X64PagedWatchpointTest, RepeatGroupsRetainCausesAcrossHostStops) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0xf3, 0xa4};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled));
    for (unsigned Case = 0; Case != 4; ++Case) {
      SCOPED_TRACE(testing::Message() << "llvm=" << LLVM << " case=" << Case);
      resetDebug(Code);
      auto &Core = State.Core.Core.Core.Core;
      Core.RFlags = Case == 3 ? 0x302 : 0x202;
      Core.GPR[1] = 3;
      Core.GPR[6] = 0x20000;
      Core.GPR[7] = 0x22000;
      X64PagedDebugState Debug;
      Debug.DR7 = 0x30401;
      Debug.DR[0] = 0x20000;
      auto O = watchOptions(Debug);
      std::vector<X86RepeatDebugStep> Steps;
      O.RepeatDebug->Context = &Steps;
      O.RepeatDebug->EndGroup = [](void *Context,
                                   const X86RepeatDebugStep &Step) {
        static_cast<std::vector<X86RepeatDebugStep> *>(Context)->push_back(
            Step);
        return false;
      };
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, O);
      auto Advance = [&](uint64_t &Budget) {
        return Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
      };
      uint64_t Budget = 1;
      ASSERT_EQ(Advance(Budget), Case == 3 ? 7u : 9u) << Context.lastError();
      EXPECT_EQ(Core.GPR[1], 2u);
      if (Case == 3) {
        EXPECT_TRUE(Steps.empty());
        EXPECT_EQ(Debug.PendingData, 0);
        EXPECT_EQ(Debug.DR6, 0xffff4ff1u);
        continue;
      }
      ASSERT_EQ(Steps.size(), 1u);
      EXPECT_EQ(Steps[0].PC, 0x1000u);
      EXPECT_EQ(Steps[0].Flags, 0x202u);
      EXPECT_EQ(Steps[0].Count, 2u);
      EXPECT_EQ(Steps[0].Source, 0x20001u);
      EXPECT_EQ(Steps[0].Destination, 0x22001u);
      EXPECT_EQ(Steps[0].AddressBytes, 8);
      EXPECT_EQ(Steps[0].ElementBytes, 1);
      EXPECT_TRUE(Steps[0].SourceIndex && Steps[0].DestinationIndex);
      EXPECT_FALSE(Steps[0].Compare);
      EXPECT_EQ(Debug.PendingData, 1);
      EXPECT_EQ(Debug.DR6, 0xffff0ff0u);
      if (Case == 1 || Case == 2) {
        auto Store = Memory->physicalMemory().storeScalar(
            0x13100, GuestScalarWidth::I64, Case == 1 ? 0x40067 : 0);
        ASSERT_EQ(Store.Status, GuestMemoryAccessStatus::Completed);
        Budget = 1;
        ASSERT_EQ(Advance(Budget), Case == 1 ? 18u : 7u) << Context.lastError();
        EXPECT_EQ(Steps.size(), 1u);
        EXPECT_EQ(Core.GPR[1], 2u);
        if (Case == 2) {
          EXPECT_EQ(Debug.PendingData, 0);
          EXPECT_EQ(Debug.DR6, 0xffff0ff0u);
          ASSERT_TRUE(Context.lastResult().Delivery);
          EXPECT_EQ(Context.lastResult().Delivery->DeliveredVector, 14);
          continue;
        }
        EXPECT_EQ(Budget, 1u);
        EXPECT_EQ(Debug.PendingData, 1);
        ASSERT_EQ(Memory->physicalMemory()
                      .storeScalar(0x13100, GuestScalarWidth::I64, 0x30067)
                      .Status,
                  GuestMemoryAccessStatus::Completed);
      }
      Budget = 1;
      EXPECT_EQ(Advance(Budget), 9u) << Context.lastError();
      EXPECT_EQ(Debug.PendingData, 1);
      Budget = 1;
      EXPECT_EQ(Advance(Budget), 7u) << Context.lastError();
      EXPECT_EQ(Steps.size(), 2u);
      EXPECT_EQ(Core.GPR[1], 0u);
      EXPECT_EQ(Core.GPR[6], 0x20003u);
      EXPECT_EQ(Core.GPR[7], 0x22003u);
      EXPECT_EQ(Debug.PendingData, 0);
      EXPECT_EQ(Debug.DR6, 0xffff0ff1u);
    }
  }
}

TEST_F(X64PagedWatchpointTest, RepeatRequiresGroupingBeforeOperandEffects) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0xf3, 0xa4};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled));
    for (unsigned Case = 0; Case != 4; ++Case) {
      resetDebug(Code);
      auto &Core = State.Core.Core.Core.Core;
      Core.RFlags = 0x202;
      Core.GPR[1] = Case & 1 ? 0 : 3;
      X64PagedDebugState Debug;
      auto O = watchOptions(Debug);
      if (Case < 2)
        O.RepeatDebug.reset();
      else
        O.RepeatDebug->EndGroup = nullptr;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      const auto Before = State;
      const auto BeforeRAM = Memory->physicalMemory().snapshotMemoryRegions();
      X64PagedExecution Context(*Memory, O);
      uint64_t Budget = 1;
      EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                5u);
      EXPECT_NE(Context.lastError().find("debug-group binding"),
                std::string::npos);
      EXPECT_EQ(Budget, 1u);
      EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
      EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
                BeforeRAM.front().Bytes);
    }
  }
}

TEST_F(X64PagedWatchpointTest, PortTrapsUseTheDeviceTransferReceipt) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true})
    for (bool Input : {false, true})
      for (bool Repeat : {false, true}) {
        const std::vector<uint8_t> Code =
            Repeat ? std::vector<uint8_t>{0xf3, uint8_t(Input ? 0x6d : 0x6f)}
                   : std::vector<uint8_t>{uint8_t(Input ? 0xed : 0xef)};
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled));
        for (unsigned Case = 0; Case != 5; ++Case) {
          resetDebug(Code);
          auto &Core = State.Core.Core.Core.Core;
          Core.RFlags = 0x202;
          Core.GPR[1] = Case == 2 ? 0 : 3;
          Core.GPR[2] = Case == 4 ? 0xffff : 0x2ff;
          Core.GPR[6] = Core.GPR[7] = 0x20000;
          Controls.CR4 |= 8;
          X64PagedDebugState Debug;
          Debug.DR7 = 0x20401;
          Debug.DR[0] = Case == 4 ? 0x10000 : 0x300;
          PagedPortTape Tape;
          Tape.Ready = Case != 3;
          auto O = watchOptions(Debug);
          O.PortIO = X64PagedPortIOBinding{
              X64PagedPortIOProfile::InputAfterWriteChecks,
              PagedPortTape::transfer, &Tape};
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(*Memory, O);
          uint64_t Budget = 1;
          const bool Zero = Repeat && Case == 2;
          const auto Exit =
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
          EXPECT_EQ(Exit, Zero        ? 9u
                          : Case == 3 ? 22u
                                      : 7u)
              << Context.lastError();
          if (Zero || Case == 3) {
            EXPECT_TRUE(Tape.Transfers.empty());
            EXPECT_EQ(Debug.DR6, 0xffff0ff0u);
          } else {
            ASSERT_EQ(Tape.Transfers.size(), 1u);
            EXPECT_TRUE(Context.lastResult().DeviceTransferCompleted);
            EXPECT_EQ(Debug.DR6, 0xffff0ff1u);
            EXPECT_EQ(Core.GPR[1], Repeat ? 2u : Case == 2 ? 0u : 3u);
            EXPECT_EQ(Context.lastResult().InstructionCompleted, !Repeat);
          }
        }
      }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedWatchpointTest, OriginalReturnSelectorEndsAtPageBoundary) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  CPU.PrefaultMemory = true;
  unsigned Comparisons = 0;
  for (bool LLVM : {false, true})
    for (unsigned Form = 0; Form != 3; ++Form) {
      const bool Interrupt = Form == 0, Outer = Form == 2;
      std::vector<uint8_t> Code(0x802, 0x90);
      Code[0] = 0x48;
      Code[1] = Interrupt ? 0xcf : 0xcb;
      Code[0x800] = 0xcd;
      Code[0x801] = 64;
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(
          compileDebug(Code, LLVM, Driver, Scheduled, {0x1000, 0x1800}));
      resetDebug(Code);
      State.Core.Core.Core.Core.RFlags = 0x202;
      const uint64_t SP = Interrupt ? 0x1efde : Outer ? 0x1efe6 : 0x1eff6;
      State.Core.Core.Core.Core.GPR[4] = SP;
      put(SP, 0x1800);
      put(SP + 8, Outer ? 27 : 8);
      if (Interrupt) {
        put(SP + 16, 0x202);
        put(SP + 24, 0x1d000);
        put(SP + 32, 16);
      }
      if (Outer) {
        put(SP + 16, 0x1d000);
        put(SP + 24, 35);
      }
      put(0x130f8, 0); // Only unused selector-slot padding is unmapped.
      put(0x6400, 0x4400 | (uint64_t{8} << 16) | (uint64_t{0xee} << 40));
      X64PagedDebugState Debug;
      CPU.DebugRegisters.emplace();
      CPU.DebugRegisters->dr6 = Debug.DR6;
      CPU.DebugRegisters->dr7 = Debug.DR7;
      X64ContinuationStateV6 Native;
      bool Fault = false;
      ASSERT_NO_FATAL_FAILURE(CPU.execute(RAM.Memory.front().Bytes, State,
                                          Controls.CR0, 2, Native, Fault, true,
                                          nullptr, Controls.CR4, true, false));
      ASSERT_EQ(Native.Core.Core.Core.Core.PC, 0x4400u);
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, watchOptions(Debug));
      uint64_t Budget = 1;
      ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                6u)
          << Context.lastError();
      EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x1800u);
      Budget = 1;
      ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                7u)
          << Context.lastError();
      expectPagedSegmentCPU(State, Native, *Memory, CPU);
      const auto Actual =
          Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
      const auto Difference =
          std::mismatch(Actual.begin(), Actual.end(), CPU.RAM);
      EXPECT_EQ(Difference.first, Actual.end())
          << "RAM differs at " << std::hex
          << (Difference.first - Actual.begin());
      if (HasFailure())
        return;
      ++Comparisons;
    }
  RecordProperty("original_comparisons", Comparisons);
}
#endif

TEST_F(X64PagedWatchpointTest, EventAccessMatchesSurviveBackingResume) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true})
    for (unsigned Case = 0; Case != 3; ++Case) {
      const std::vector<uint8_t> Code =
          Case ? std::vector<uint8_t>{0xcd, 64}
               : std::vector<uint8_t>{0x8e, 0x17, 0xcd, 64};
      const std::vector<uint64_t> PCs =
          Case ? std::vector<uint64_t>{0x1000}
               : std::vector<uint64_t>{0x1000, 0x1002};
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled, PCs));
      resetDebug(Code);
      State.Core.Core.Core.Core.RFlags = 0x202;
      put(0x30000, 16);
      put(Case ? 0x13028 : 0x13030, 0x50067);
      X64PagedDebugState Debug;
      Debug.DR7 = 0x30401;
      Debug.DR[0] = Case ? 0x6400 : 0x20000;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, watchOptions(Debug));
      Shadows Shadow;
      Producer P;
      P.Ready = 0;
      uint64_t Budget = Case ? 1 : 2, Polls = Budget;
      ASSERT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                          &Budget, &Shadow, &Polls, Producer::peek,
                          Producer::accepted, &P),
                18u)
          << Context.lastError();
      ASSERT_TRUE(Context.lastResult().Delivery->Continuation);
      EXPECT_EQ(Context.lastResult().Delivery->Continuation->DataBreakpoints,
                Case ? 1 : 0);
      EXPECT_EQ(Debug.PendingData, Case ? 0 : 1);
      ASSERT_EQ(Memory->physicalMemory()
                    .storeScalar(Case ? 0x13028 : 0x13030,
                                 GuestScalarWidth::I64, Case ? 0x5067 : 0x6067)
                    .Status,
                GuestMemoryAccessStatus::Completed);
      if (Case == 2)
        ASSERT_EQ(Memory->physicalMemory()
                      .storeScalar(0x6010, GuestScalarWidth::I64,
                                   0x4010 | (uint64_t{8} << 16) |
                                       (uint64_t{0x0e} << 40))
                      .Status,
                  GuestMemoryAccessStatus::Completed);
      auto R = Context.resume(
          State, *Context.lastResult().Delivery->Continuation, 0xffff);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Exit, 7u);
      ASSERT_TRUE(R->Delivery);
      EXPECT_TRUE(R->Delivery->SoftwareCompleted);
      EXPECT_EQ(R->Delivery->DeliveredVector, Case == 2 ? 11 : 1);
      EXPECT_EQ(Debug.DR6, Case ? 0xffff0ff1u : 0xffff0ff0u);
      EXPECT_EQ(Debug.PendingData, 0);
      const auto SP = State.Core.Core.Core.Core.GPR[4];
      EXPECT_EQ(Memory->physicalMemory()
                    .loadScalar(SP + (Case == 2 ? 8 : 0), GuestScalarWidth::I64)
                    .Value,
                0x4400u);
    }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedWatchpointTest, OriginalRegisterWriteArmsFollowingOperand) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  CPU.PrefaultMemory = true;
  const std::vector<uint8_t> Code{0x0f, 0x23, 0xf8, 0x48, 0x89, 0x07, 0xcd, 64};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compileDebug(Code, LLVM, Driver, Scheduled, {0x1000, 0x1003, 0x1006}));
    resetDebug(Code);
    State.Core.Core.Core.Core.RFlags = 0x202;
    State.Core.Core.Core.Core.GPR[0] = 0x30401;
    X64PagedDebugState Debug;
    Debug.DR[0] = 0x20000;
    ASSERT_NO_FATAL_FAILURE(compare(CPU, Scheduled, Debug, 3));
    EXPECT_EQ(Debug.DR7, 0x30401u);
    EXPECT_EQ(Debug.DR6, 0xffff0ff1u);
  }
  RecordProperty("original_comparisons", 2);
}
#endif

TEST_F(X64PagedWatchpointTest, FixedISTRecursionYieldsAndResumesItsNextEvent) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    const std::vector<uint8_t> Code{0xcd, 1};
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled));
    resetDebug(Code);
    put(0x6010, 0x4010 | (uint64_t{8} << 16) | (uint64_t{1} << 32) |
                    (uint64_t{0x8e} << 40));
    put(0x8024, 0x1f000);
    X64PagedDebugState Debug;
    Debug.DR7 = 0x30401;
    Debug.DR[0] = 0x6010;
    auto O = watchOptions(Debug);
    O.EventDelivery.MaxAttempts = 2;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory, O);
    uint64_t Budget = 1;
    ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              uint64_t(X64PagedExit::EventBudgetExhausted))
        << Context.lastError();
    EXPECT_EQ(Budget, 0u);
    EXPECT_TRUE(Context.lastResult().InstructionCompleted);
    const auto &First = Context.lastResult().Delivery;
    ASSERT_TRUE(First && First->Continuation);
    EXPECT_TRUE(First->InputAccepted);
    EXPECT_TRUE(First->SoftwareCompleted);
    EXPECT_EQ(First->DeliveryAttempts, 2u);
    EXPECT_FALSE(First->Continuation->EventAccepted);
    EXPECT_TRUE(First->Continuation->CompletedAccesses.empty());
    EXPECT_EQ(First->Continuation->DataBreakpoints, 0u);
    auto &Core = State.Core.Core.Core.Core;
    EXPECT_EQ(Core.PC, 0x4010u);
    EXPECT_EQ(Core.GPR[4], 0x1efd8u);
    const auto Stable = State;
    const auto StableRAM = Memory->physicalMemory().snapshotMemoryRegions();
    auto Pending = *First->Continuation;
    for (unsigned I = 0; I != 3; ++I) {
      auto R = Context.resume(State, Pending, 0xffbf);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Exit, uint64_t(X64PagedExit::EventBudgetExhausted));
      ASSERT_TRUE(R->Delivery && R->Delivery->Continuation);
      EXPECT_EQ(R->Delivery->DeliveryAttempts, 2u);
      EXPECT_TRUE(R->Delivery->SoftwareCompleted);
      EXPECT_FALSE(R->InstructionAttempted);
      EXPECT_FALSE(R->Delivery->Continuation->EventAccepted);
      EXPECT_EQ(std::memcmp(&State, &Stable, sizeof(State)), 0);
      EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
                StableRAM.front().Bytes);
      Pending = *R->Delivery->Continuation;
    }
    Debug.DR7 &= ~uint64_t{3};
    auto R = Context.resume(State, Pending, 0xffbf);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_TRUE(R->Delivery);
    EXPECT_EQ(R->Delivery->Status, X64PagedEventStatus::Delivered);
    EXPECT_EQ(R->Delivery->DeliveredVector, 1u);
    EXPECT_EQ(R->Delivery->DeliveryAttempts, 1u);
    EXPECT_FALSE(R->Delivery->Continuation);
    EXPECT_EQ(Core.PC, 0x4010u);
    EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
              StableRAM.front().Bytes);
  }
}

TEST_F(X64PagedWatchpointTest, EventBudgetRetainsFaultChainAndRejectsZero) {
  resetDebug({0xcd, 64});
  put(0x6400, 0x4400 | (uint64_t{8} << 16) | (uint64_t{0x0e} << 40));
  put(0x60b0, 0x40b0 | (uint64_t{8} << 16) | (uint64_t{0x0e} << 40));
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  auto Reference =
      llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  const auto Original = State;
  auto Expected = State;
  X64ContinuationEventV1 Event;
  Event.Kind = X64ContinuationEventKind::Software;
  Event.Vector = 64;
  Event.SavedPC = 0x1002;
  auto Complete = deliverX64PagedEvent(Expected, *Reference, Event, 0xffbf);
  ASSERT_TRUE(bool(Complete)) << llvm::toString(Complete.takeError());
  ASSERT_EQ(Complete->Status, X64PagedEventStatus::Delivered);
  ASSERT_EQ(Complete->DeliveredVector, 8u);
  ASSERT_EQ(Complete->DeliveryAttempts, 3u);
  X86EventDeliveryOptions Work;
  Work.MaxAttempts = 0;
  auto Invalid =
      deliverX64PagedEvent(State, *Memory, Event, 0xffbf, nullptr,
                           X86InstructionDebugProfile::InstructionStep, Work);
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  EXPECT_EQ(std::memcmp(&State, &Original, sizeof(State)), 0);
  EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.Memory.front().Bytes);
  for (bool Access : {false, true}) {
    X64PagedEventContinuation InvalidPending;
    InvalidPending.Event = Event;
    InvalidPending.EventAccepted = false;
    if (Access)
      InvalidPending.CompletedAccesses.push_back({0x6400, 0, 8, false});
    else
      InvalidPending.DataBreakpoints = 1;
    auto InvalidResume =
        resumeX64PagedEvent(State, *Memory, InvalidPending, 0xffbf);
    ASSERT_FALSE(bool(InvalidResume));
    llvm::consumeError(InvalidResume.takeError());
    EXPECT_EQ(std::memcmp(&State, &Original, sizeof(State)), 0);
    EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
              RAM.Memory.front().Bytes);
  }
  Work.MaxAttempts = 1;
  auto R =
      deliverX64PagedEvent(State, *Memory, Event, 0xffbf, nullptr,
                           X86InstructionDebugProfile::InstructionStep, Work);
  for (unsigned Attempt = 0; Attempt != 3; ++Attempt) {
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_TRUE(R->InputAccepted);
    EXPECT_FALSE(R->SoftwareCompleted);
    EXPECT_EQ(R->DeliveryAttempts, 1u);
    if (Attempt == 2) {
      EXPECT_EQ(R->Status, X64PagedEventStatus::Delivered);
      EXPECT_EQ(R->DeliveredVector, 8u);
      break;
    }
    ASSERT_EQ(R->Status, X64PagedEventStatus::BudgetExhausted);
    ASSERT_TRUE(R->Continuation);
    EXPECT_EQ(R->Continuation->Event.Vector, Attempt ? 8 : 11);
    EXPECT_FALSE(R->Continuation->EventAccepted);
    auto Pending = *R->Continuation;
    R = resumeX64PagedEvent(State, *Memory, Pending, 0xffbf, nullptr,
                            X86InstructionDebugProfile::InstructionStep, Work);
  }
  EXPECT_EQ(std::memcmp(&State, &Expected, sizeof(State)), 0);
  EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            Reference->physicalMemory().snapshotMemoryRegions().front().Bytes);
}
