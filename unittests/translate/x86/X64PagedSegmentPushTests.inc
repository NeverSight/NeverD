//===- X64PagedSegmentPushTests.inc - Selector stack publication ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original PUSH FS/GS widths, stack faults, round trips and event boundaries.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, PagedSegmentPushMatchesOriginalCPU) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  RecordProperty("processor", CPU.ReturnProcessor);
  RecordProperty("core_type", CPU.ReturnCoreType);
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  struct Form {
    unsigned Width, Length;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (unsigned Segment : {4, 5})
    for (unsigned Kind = 0; Kind != 6; ++Kind) {
      const unsigned Start = Code.size();
      Addresses.push_back(0x1000 + Start);
      if (Kind >= 3) {
        Code.push_back(Kind == 3 ? 0x64 : Kind == 4 ? 0x65 : 0x36);
        Code.push_back(0x67);
      }
      if (Kind == 1 || Kind == 2 || Kind == 4)
        Code.push_back(0x66);
      if (Kind == 2 || Kind == 5)
        Code.push_back(Kind == 2 ? 0x48 : 0x49);
      Code.insert(Code.end(), {0x0f, uint8_t(Segment == 4 ? 0xa0 : 0xa8)});
      Forms.push_back({Kind == 1 || Kind == 4 ? 2u : 8u,
                       unsigned(Code.size() - Start), 0x1000 + Start});
      Code.resize(Start + 16, 0x90);
    }
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  unsigned Comparisons = 0, Faults = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(
            compile(Code, LLVM, O, Driver, Addresses, Compiler));
        for (const auto &F : Forms)
          for (unsigned Case = 0; Case != 32; ++Case) {
            SCOPED_TRACE(llvm::formatv("{0} llvm={1} {2} PC={3:x} case={4}",
                                       Compiler, LLVM, O, F.PC, Case)
                             .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = F.PC;
            const unsigned CPL = Case < 16 ? Case / 4
                                 : Case == 22 || Case == 23 || Case == 24 ||
                                         Case == 29 || Case == 30
                                     ? 3
                                     : 0;
            State.Core.CS = {0, UINT32_MAX, uint16_t(24 | CPL),
                             uint16_t(0xa09b | (CPL << 5))};
            State.Core.SS = {0, UINT32_MAX, uint16_t(32 | CPL),
                             uint16_t(0xc093 | (CPL << 5))};
            State.DS = State.ES = State.Core.SS;
            const uint16_t Selectors[] = {0, 0xffff, uint16_t(40 | CPL), 16};
            State.FS = {UINT32_MAX, Selectors[Case & 3],
                        State.Core.SS.Attributes};
            State.GS = {UINT32_MAX, uint16_t(Selectors[Case & 3] ^ 0x28),
                        State.Core.SS.Attributes};
            if (Case < 16 && (Case & 3) == 3)
              State.Unusable |= 0x30;
            Core.FSBase = 0x12345678;
            Core.GSBase = 0x23456789;
            put(0x8024, 0x1c000);
            for (unsigned V : {8, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            uint64_t Target = Case == 18 || Case == 19   ? 0x20fff
                              : Case == 20               ? 0x21000 - F.Width
                              : Case == 21               ? 0x800000000000ULL
                              : Case == 22 || Case == 23 ? 0x20001
                              : Case == 29 || Case == 30 ? 0x20002
                                                         : 0x20000;
            if (Case == 16)
              put(0x13100, 0x30005);
            if (Case == 17 || Case == 23 || Case == 30)
              put(0x13100, 0);
            if (Case == 18 || Case == 20)
              put(0x13108, 0);
            if (Case == 19)
              put(0x13108, 0x31005);
            if (Case == 24)
              put(0x13100, 0x30003);
            if (Case == 22 || Case == 23 || Case == 29 || Case == 30) {
              Controls.CR0 |= 0x40000;
              Core.RFlags |= 0x40000;
            }
            if (Case == 25)
              Core.RFlags |= 0x4000;
            if (Case == 26)
              Core.RFlags |= 0x10000;
            if (Case == 27 || Case == 28) {
              Target = (Case == 27 ? 0x800000000000ULL : uint64_t(0)) - F.Width;
              put(Case == 27 ? 0x107f8 : 0x10ff8, 0x14007);
              put(0x14ff8, 0x15007);
              put(0x15ff8, 0x16007);
              put(0x16ff8, 0x30007);
            }
            if (Case == 31)
              State.GDTR.Base = 0x60000; // PUSH must not read any descriptor.
            Core.GPR[4] = Target + F.Width;
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0, F.Length, Native,
                Handler, false, nullptr, Controls.CR4, true, false));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(
                *Memory,
                {.AlignmentPriority = X64PagedAlignmentPriority::BeforePaging,
                 .SegmentMoveProfile =
                     X64PagedSegmentMoveProfile::SelectorFirst});
            uint64_t Budget = 1;
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                Handler ? 7u : 9u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            if (!Handler)
              EXPECT_EQ(Core.GPR[4], Target);
            expectPagedSegmentCPU(State, Native, *Memory, CPU,
                                  Case == 27 || Case == 28 ? 0x17000 : 0x14000);
            ++Comparisons;
            Faults += Handler;
            if (HasFailure())
              return;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("segment_push_fault_cpu_comparisons", Faults);
}

TEST_F(X64PagedExecutionTest, PagedSegmentPushPopAndPendingEvents) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  RecordProperty("processor", CPU.ReturnProcessor);
  RecordProperty("core_type", CPU.ReturnCoreType);
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  unsigned Comparisons = 0;
  for (unsigned Segment : {4, 5})
    for (bool Word : {false, true})
      for (const auto &Compiler : Compilers)
        for (bool LLVM : {false, true}) {
          std::vector<uint8_t> Code;
          if (Word)
            Code.push_back(0x66);
          Code.insert(Code.end(), {0x0f, uint8_t(Segment == 4 ? 0xa0 : 0xa8)});
          const unsigned Length = Code.size();
          if (Word)
            Code.push_back(0x66);
          Code.insert(Code.end(), {0x0f, uint8_t(Segment == 4 ? 0xa1 : 0xa9)});
          Run Driver = nullptr;
          Schedule Scheduled = nullptr;
          ASSERT_NO_FATAL_FAILURE(compile(
              Code, LLVM, "-O2", Driver, {0x1000, 0x1000 + Length}, Compiler,
              0x1000, &Scheduled, X64NullSegmentBasePolicy::Clear));
          for (unsigned Case = 0; Case != 9; ++Case) {
            SCOPED_TRACE(
                llvm::formatv("segment={0} word={1} {2} llvm={3} case={4}",
                              Segment, Word, Compiler, LLVM, Case)
                    .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            const unsigned CPL = Case < 4 ? Case : 0;
            State.Core.CS = {0, UINT32_MAX, uint16_t(24 | CPL),
                             uint16_t(0xa09b | (CPL << 5))};
            State.Core.SS = {0, UINT32_MAX, uint16_t(32 | CPL),
                             uint16_t(0xc093 | (CPL << 5))};
            State.DS = State.ES = State.Core.SS;
            State.FS =
                State.GS = {UINT32_MAX, uint16_t(Case == 4 ? 0 : 40 | CPL),
                            State.Core.SS.Attributes};
            Core.RFlags = 0x202;
            Core.FSBase = 0x12345678;
            Core.GSBase = 0x23456789;
            put(0x5028, 0x00cf92567800ffffULL | (uint64_t(CPL) << 45));
            const bool Pending = Case >= 5;
            X64ContinuationEventV1 Event{};
            Event.Kind = Case & 1 ? X64ContinuationEventKind::NMI
                                  : X64ContinuationEventKind::Interrupt;
            Event.Vector = Case & 1 ? 2 : 65;
            Event.SSBlocked = Case == 5 || Case == 6;
            Event.STIBlocked = Case == 7 || Case == 8;
            Event.NMIBlockedBySTI = 1;
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0, Code.size(),
                Native, Handler, Pending, Pending ? &Event : nullptr,
                Controls.CR4, true, false));
            EXPECT_EQ(Handler, Pending);
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(
                *Memory, {.SegmentMoveProfile =
                              X64PagedSegmentMoveProfile::SelectorFirst});
            uint64_t Budget = 2, Polls = 2;
            Shadows Shadow;
            Shadow.SSBlocked = Event.SSBlocked;
            Shadow.STIBlocked = Event.STIBlocked;
            Shadow.NMIBlockedBySTI = 1;
            Producer Events;
            Events.Ready = Pending;
            Events.Candidate = {Event.Vector, uint8_t(Event.Kind)};
            EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                                &Budget, &Shadow, &Polls, Producer::peek,
                                Producer::accepted, &Events),
                      Pending ? 7u : 9u);
            EXPECT_EQ(Budget, Pending ? 1u : 0u);
            EXPECT_EQ(Events.Acks, unsigned(Pending));
            EXPECT_EQ(Shadow.SSBlocked, 0);
            EXPECT_EQ(Shadow.STIBlocked, 0);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            if (!Pending)
              EXPECT_EQ(Core.GPR[4], 0x1e000u);
            expectPagedSegmentCPU(State, Native, *Memory, CPU);
            ++Comparisons;
            if (HasFailure())
              return;
          }
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionTest, PagedSegmentPushAdmissionAndBacking) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  unsigned Checks = 0;
  for (bool Word : {false, true})
    for (const auto &Compiler : Compilers)
      for (bool LLVM : {false, true}) {
        std::vector<uint8_t> Code;
        if (Word)
          Code.push_back(0x66);
        Code.insert(Code.end(), {0x0f, 0xa8});
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                        Compiler, 0x1000, &Scheduled));
        for (unsigned Case = 0; Case != 5; ++Case) {
          SCOPED_TRACE(llvm::formatv("word={0} {1} llvm={2} case={3}", Word,
                                     Compiler, LLVM, Case)
                           .str());
          reset(Code);
          const unsigned Width = Word ? 2 : 8;
          auto &Core = State.Core.Core.Core.Core;
          Core.GPR[4] = (Case == 3 ? 0x20fff : 0x20000) + Width;
          State.GS.Selector = 0xabcd;
          if (Case == 1)
            RAM.Memory.front().Bytes[0x1000] = 0x90;
          if (Case == 2 || Case == 3)
            put(Case == 2 ? 0x13100 : 0x13108, 0x50007);
          if (Case == 4) {
            // Pre-set every A/D bit so the rejected write is the operand,
            // not an earlier page-walk update in the read-only RAM binding.
            put(0x10000, 0x11027);
            put(0x11000, 0x12027);
            put(0x12000, 0x13027);
            for (unsigned I = 0; I != 64; ++I)
              put(0x13000 + I * 8, I * 4096 | 0x67);
            put(0x13100, 0x30067);
            RAM.Memory.front().Permissions = MemoryPermission::Read;
          }
          const auto Before = State;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecutionOptions Options;
          if (Case)
            Options.SegmentMoveProfile =
                X64PagedSegmentMoveProfile::SelectorFirst;
          X64PagedExecution Context(*Memory, Options);
          Shadows Shadow;
          Shadow.SSBlocked = 1;
          Producer Events;
          Events.Ready = 0;
          uint64_t Budget = 1, Polls = 1;
          EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                              &Budget, &Shadow, &Polls, Producer::peek,
                              Producer::accepted, &Events),
                    Case == 0   ? 5u
                    : Case == 1 ? 20u
                                : 18u);
          EXPECT_EQ(Budget, 1u);
          EXPECT_EQ(Shadow.SSBlocked, 1);
          EXPECT_EQ(Events.Acks, 0u);
          EXPECT_FALSE(Context.lastResult().InstructionAttempted);
          EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
          if (Case == 0)
            EXPECT_NE(Context.lastError().find("bound access order"),
                      std::string::npos);
          else
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          if (Case >= 2) {
            ASSERT_TRUE(Context.lastResult().BackingFault);
            EXPECT_EQ(Context.lastResult().BackingFault->Exit.Address,
                      Case == 4 ? 0x30000u : 0x50000u);
          }
          const auto After = Memory->physicalMemory().snapshotMemoryRegions();
          EXPECT_EQ(std::memcmp(After.front().Bytes.data(),
                                RAM.Memory.front().Bytes.data(), 0x10000),
                    0);
          EXPECT_EQ(std::memcmp(After.front().Bytes.data() + 0x14000,
                                RAM.Memory.front().Bytes.data() + 0x14000,
                                0x2c000),
                    0);
          ++Checks;
        }
      }
  RecordProperty("policy_backing_checks", Checks);
}
#endif
