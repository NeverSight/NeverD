//===- X64PagedRepeatedStringBoundaryTests.inc - REP interruption ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Budget, backing and asynchronous boundaries preserve completed elements.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, PagedRepeatedTransfersRejectUnboundForms) {
  const std::vector<std::vector<uint8_t>> Forms{
      {0xf2, 0xa4},       {0xf3, 0xf3, 0xa4}, {0xf2, 0xf3, 0xa6},
      {0xf0, 0xf3, 0xa4}, {0x48, 0xf3, 0xa5}, {0xf3, 0x67, 0x67, 0xa4},
      {0xf2, 0x6c},       {0xf2, 0x6d},       {0xf2, 0x6e},
      {0xf2, 0x6f}};
  X64ContinuationSourceOptions Options;
  Options.Profile = X64ContinuationProfile::KernelEventsNoFaultV6;
  Options.MXCSRMask = 0xffff;
  Options.PagingAddressBits = 48;
  Options.PagedMemory = true;
  for (const auto &Bytes : Forms) {
    auto Units = buildX64ContinuationSource(image(Bytes), {0x1000}, Options);
    if (!Units) {
      llvm::consumeError(Units.takeError());
      continue;
    }
    ASSERT_EQ(Units->size(), 1u);
    EXPECT_TRUE(Units->front().RequiresNativeExecution)
        << Units->front().Diagnostic;
  }
}

TEST_F(X64PagedExecutionTest, PagedRepeatedTransfersSuspendForEvents) {
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
    unsigned Kind, Width, Address, Length;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (unsigned Kind = 0; Kind != 7; ++Kind)
    for (unsigned Width : {1, 2, 4, 8})
      for (unsigned Address : {4, 8}) {
        const unsigned Start = Code.size();
        Addresses.push_back(0x1000 + Start);
        Code.push_back(Kind == 4 || Kind == 6 ? 0xf2 : 0xf3);
        if (Address == 4)
          Code.push_back(0x67);
        if (Width == 2)
          Code.push_back(0x66);
        if (Width == 8)
          Code.push_back(0x48);
        Code.push_back((Kind == 0   ? 0xa4
                        : Kind == 1 ? 0xaa
                        : Kind == 2 ? 0xac
                        : Kind < 5  ? 0xa6
                                    : 0xae) |
                       unsigned(Width != 1));
        Addresses.push_back(0x1000 + Code.size());
        Forms.push_back({Kind, Width, Address, unsigned(Code.size() - Start),
                         0x1000 + Start});
        Code.resize(Start + 16, 0x90);
      }
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  unsigned Comparisons = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true}) {
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, Addresses,
                                      Compiler, 0x1000, &Scheduled));
      for (const auto &F : Forms)
        for (unsigned CPL : {0, 3})
          for (unsigned ShadowKind = 0; ShadowKind != 4; ++ShadowKind)
            for (unsigned Count : {0, 1, 4}) {
              SCOPED_TRACE(
                  llvm::formatv(
                      "{0} llvm={1} pc={2:x} cpl={3} shadow={4} count={5}",
                      Compiler, LLVM, F.PC, CPL, ShadowKind, Count)
                      .str());
              reset(Code);
              auto &Core = State.Core.Core.Core.Core;
              Core.PC = F.PC;
              Core.RFlags = 0xad7 | (F.Width == 2 ? 0x400 : 0);
              Core.GPR[1] = Count;
              Core.GPR[6] = 0x20080;
              Core.GPR[7] = 0x22080;
              if (F.Address == 4) {
                Core.GPR[1] |= 0x1234567800000000ULL;
                Core.GPR[6] |= 0x2345678900000000ULL;
                Core.GPR[7] |= 0x3456789a00000000ULL;
              }
              State.Core.CS = {0, UINT32_MAX, uint16_t(24 | CPL),
                               uint16_t(0xa09b | (CPL << 5))};
              State.Core.SS = {0, UINT32_MAX, uint16_t(32 | CPL),
                               uint16_t(0xc093 | (CPL << 5))};
              State.DS = State.ES = State.Core.SS;
              State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
              put(0x13110, 0x32007);
              for (unsigned I = 0; I != 0x3000; ++I)
                RAM.Memory.front().Bytes[0x30000 + I] =
                    F.Kind >= 3
                        ? ((F.Kind == 4 || F.Kind == 6) && I >= 0x2000 ? 0x22
                                                                       : 0x11)
                        : I * 31 + 7;
              if (F.Kind >= 5)
                Core.GPR[0] = 0x1111111111111111ULL;
              X64ContinuationEventV1 Event{};
              Event.Kind = ShadowKind & 1 ? X64ContinuationEventKind::NMI
                                          : X64ContinuationEventKind::Interrupt;
              Event.Vector = ShadowKind & 1 ? 2 : 65;
              Event.SSBlocked = ShadowKind < 2;
              Event.STIBlocked = ShadowKind >= 2;
              Event.NMIBlockedBySTI = 1;
              X64ContinuationStateV6 Native;
              bool Handler = false;
              ASSERT_NO_FATAL_FAILURE(CPU.execute(
                  RAM.Memory.front().Bytes, State, Controls.CR0, F.Length,
                  Native, Handler, true, &Event, Controls.CR4, true, false));
              ASSERT_TRUE(Handler);
              // Fast-string groups can expose a later interruption boundary
              // than the first element. Replay the observed architectural
              // boundary; do not infer hardware arrival timing from this CPU.
              const auto &NativeCore = Native.Core.Core.Core.Core;
              uint64_t SavedPC = 0;
              for (unsigned Byte = 0; Byte != 8; ++Byte)
                SavedPC |= uint64_t(CPU.RAM[NativeCore.GPR[4] + Byte])
                           << (8 * Byte);
              const uint64_t Remaining = F.Address == 4
                                             ? uint32_t(NativeCore.GPR[1])
                                             : NativeCore.GPR[1];
              const uint64_t Attempts = Count ? Count - Remaining : 1;
              ASSERT_GT(Attempts, 0u);
              struct BoundaryProducer : Producer {
                uint64_t PC, Count;
                static int peek(const X64ContinuationStateV6 *S,
                                const Shadows *, Pending *P, void *Context) {
                  auto &Self = *static_cast<BoundaryProducer *>(Context);
                  ++Self.Polls;
                  *P = Self.Candidate;
                  const auto &Core = S->Core.Core.Core.Core;
                  return Core.PC == Self.PC && Core.GPR[1] == Self.Count;
                }
                static void accepted(const Pending *P, void *Context) {
                  Producer::accepted(
                      P, static_cast<Producer *>(
                             static_cast<BoundaryProducer *>(Context)));
                }
              };
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              X64PagedExecution Context(
                  *Memory,
                  {.RepeatProfile =
                       CPU.ReturnCoreType == 0x20
                           ? X64PagedRepeatProfile::PreserveLodsCountHigh
                           : X64PagedRepeatProfile::ClearLodsCountHigh});
              Shadows Shadow;
              Shadow.SSBlocked = Event.SSBlocked;
              Shadow.STIBlocked = Event.STIBlocked;
              Shadow.NMIBlockedBySTI = 1;
              BoundaryProducer Events;
              Events.Candidate = {Event.Vector, uint8_t(Event.Kind)};
              Events.PC = SavedPC;
              Events.Count = NativeCore.GPR[1];
              uint64_t Budget = Attempts + 1, Polls = Attempts + 1;
              EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                                  &Budget, &Shadow, &Polls,
                                  BoundaryProducer::peek,
                                  BoundaryProducer::accepted, &Events),
                        7u);
              EXPECT_EQ(Budget, 1u);
              EXPECT_EQ(Polls, 0u);
              EXPECT_EQ(Events.Acks, 1u);
              EXPECT_EQ(Shadow.SSBlocked, 0);
              EXPECT_EQ(Shadow.STIBlocked, 0);
              EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
              expectPagedSegmentCPU(State, Native, *Memory, CPU);
              ++Comparisons;
              if (HasFailure())
                return;
            }
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionTest, PagedRepeatedTransfersRetainProgressAcrossStops) {
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
  unsigned Comparisons = 0, Checks = 0;
  for (unsigned Opcode : {0xa4, 0xaa, 0xac, 0xa6, 0xae})
    for (unsigned Width : {1, 8})
      for (const auto &Compiler : Compilers)
        for (bool LLVM : {false, true}) {
          std::vector<uint8_t> Code{0xf3, 0x67};
          if (Width == 8)
            Code.push_back(0x48);
          Code.push_back(Opcode | unsigned(Width != 1));
          Run Driver = nullptr;
          ASSERT_NO_FATAL_FAILURE(
              compile(Code, LLVM, "-O2", Driver, {0x1000}, Compiler));
          for (unsigned Case = 0; Case != 7; ++Case) {
            SCOPED_TRACE(
                llvm::formatv("op={0:x} width={1} {2} llvm={3} case={4}",
                              Opcode, Width, Compiler, LLVM, Case)
                    .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.GPR[1] = 0x1234567800000004ULL;
            Core.GPR[6] = 0x21000 - 2 * Width;
            Core.GPR[7] = 0x23000 - 2 * Width;
            Core.GPR[0] = 0x1111111111111111ULL;
            Core.RFlags |= 0x8d5;
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
            put(0x13110, 0x32007);
            put(0x13118, 0x33007);
            for (unsigned I = 0; I != 0x4000; ++I)
              RAM.Memory.front().Bytes[0x30000 + I] =
                  Opcode == 0xa6 || Opcode == 0xae ? 0x11 : I * 37 + 13;
            const auto Before = State;
            const auto InitialBytes = RAM.Memory.front().Bytes;
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                InitialBytes, Before, Controls.CR0, Code.size(), Native,
                Handler, false, nullptr, Controls.CR4, true, false));
            ASSERT_FALSE(Handler);
            const bool DestinationOnly = Opcode == 0xaa || Opcode == 0xae;
            const unsigned PTE = DestinationOnly ? 0x13118 : 0x13108;
            const uint64_t Mapping = DestinationOnly ? 0x33007 : 0x31007;
            if (Case == 2)
              RAM.Memory.front().Bytes[0x1000] = 0x90;
            if (Case == 3 || Case == 4)
              put(Case == 3 ? PTE - 8 : PTE, 0x50007);
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecutionOptions Options;
            Options.RepeatProfile =
                CPU.ReturnCoreType == 0x20
                    ? X64PagedRepeatProfile::PreserveLodsCountHigh
                    : X64PagedRepeatProfile::ClearLodsCountHigh;
            if (Case == 0)
              Options.RepeatProfile.reset();
            if (Case == 1)
              Options.RepeatProfile = static_cast<X64PagedRepeatProfile>(255);
            X64PagedExecution Context(*Memory, Options);
            uint64_t Budget = Case >= 5 ? 2 : 4;
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                Case <= 1   ? 5u
                : Case == 2 ? 20u
                : Case <= 4 ? 18u
                            : 9u);
            EXPECT_EQ(Budget, Case <= 3 ? 4u : Case == 4 ? 2u : 0u);
            if (Case <= 1)
              EXPECT_NE(Context.lastError().find("profile"), std::string::npos);
            else
              EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Context.lastResult().InstructionAttempted, Case >= 5);
            EXPECT_FALSE(Context.lastResult().InstructionCompleted);
            if (Case <= 3)
              EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
            else {
              EXPECT_EQ(Core.GPR[1], 2u);
              EXPECT_EQ(Core.GPR[6], Before.Core.Core.Core.Core.GPR[6] +
                                         (DestinationOnly ? 0 : 2 * Width));
              EXPECT_EQ(Core.GPR[7], Before.Core.Core.Core.Core.GPR[7] +
                                         (Opcode == 0xac ? 0 : 2 * Width));
              EXPECT_EQ(Core.PC, 0x1000u);
              EXPECT_EQ(Core.RFlags,
                        Before.Core.Core.Core.Core.RFlags | 0x10000);
            }
            ++Checks;
            if (Case == 6) {
              const auto Paused = State;
              ASSERT_TRUE(Memory->physicalMemory()
                              .storeScalar(0x1000, GuestScalarWidth::I8, 0x90)
                              .Status == GuestMemoryAccessStatus::Completed);
              Budget = 2;
              EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context),
                               &Budget),
                        20u);
              EXPECT_EQ(Budget, 2u);
              EXPECT_EQ(std::memcmp(&State, &Paused, sizeof(State)), 0);
              ++Checks;
            }
            ASSERT_TRUE(Memory->physicalMemory()
                            .storeScalar(0x1000, GuestScalarWidth::I8, 0xf3)
                            .Status == GuestMemoryAccessStatus::Completed);
            if (Case == 3 || Case == 4)
              ASSERT_TRUE(
                  Memory->physicalMemory()
                      .storeScalar(Case == 3 ? PTE - 8 : PTE,
                                   GuestScalarWidth::I64,
                                   Case == 3 ? Mapping - 0x1000 : Mapping)
                      .Status == GuestMemoryAccessStatus::Completed);
            Options.RepeatProfile =
                CPU.ReturnCoreType == 0x20
                    ? X64PagedRepeatProfile::PreserveLodsCountHigh
                    : X64PagedRepeatProfile::ClearLodsCountHigh;
            // All restart state belongs to the architectural bank and RAM;
            // replacing the execution context must not lose a hidden cursor.
            X64PagedExecution Resumed(*Memory, Options);
            Budget = Case <= 3 ? 4 : 2;
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
                9u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_TRUE(Resumed.lastError().empty()) << Resumed.lastError();
            expectPagedSegmentCPU(State, Native, *Memory, CPU);
            ++Comparisons;
            if (HasFailure())
              return;
          }
        }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("policy_backing_checks", Checks);
}

TEST_F(X64PagedExecutionTest, PagedRepeatedTransfersRepairPageFaultAndResume) {
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
    unsigned Opcode, Width, Address, Length;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (unsigned Opcode : {0xa4, 0xaa, 0xac, 0xa6, 0xae})
    for (unsigned Width : {1, 2, 4, 8})
      for (unsigned Address : {4, 8}) {
        const unsigned Start = Code.size();
        Addresses.push_back(0x1000 + Start);
        Code.push_back(0xf3);
        if (Address == 4)
          Code.push_back(0x67);
        if (Width == 2)
          Code.push_back(0x66);
        if (Width == 8)
          Code.push_back(0x48);
        Code.push_back(Opcode | unsigned(Width != 1));
        Forms.push_back({Opcode, Width, Address, unsigned(Code.size() - Start),
                         0x1000 + Start});
        Code.resize(Start + 16, 0x90);
      }
  Code.resize(0x1012, 0x90);
  // MOV [RDX], RBX; ADD RSP, 8; IRETQ. The handler repairs the selected PTE
  // without overwriting string operands or the retained comparison flags.
  const uint8_t HandlerCode[] = {0x48, 0x89, 0x1a, 0x48, 0x83,
                                 0xc4, 0x08, 0x48, 0xcf};
  std::copy(std::begin(HandlerCode), std::end(HandlerCode),
            Code.begin() + 0x1000);
  Addresses.insert(Addresses.end(), {0x2000, 0x2003, 0x2007});
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  unsigned Comparisons = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true}) {
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, Addresses,
                                      Compiler, 0x1000, &Scheduled));
      for (const auto &F : Forms)
        for (unsigned Completed : {0, 2}) {
          SCOPED_TRACE(llvm::formatv("{0} llvm={1} pc={2:x} completed={3}",
                                     Compiler, LLVM, F.PC, Completed)
                           .str());
          reset(Code);
          auto &Core = State.Core.Core.Core.Core;
          const bool DestinationOnly = F.Opcode == 0xaa || F.Opcode == 0xae;
          const uint64_t PTE = DestinationOnly ? 0x13118 : 0x13108;
          const uint64_t Mapping = DestinationOnly ? 0x33007 : 0x31007;
          Core.PC = F.PC;
          Core.GPR[1] = 4;
          Core.GPR[2] = PTE;
          // The repair store reads RBX, leaving RAX available to LODS/SCAS.
          Core.GPR[3] = Mapping;
          Core.GPR[0] = 0x1111111111111111ULL;
          Core.GPR[6] = 0x21000 - Completed * F.Width;
          Core.GPR[7] = 0x23000 - Completed * F.Width;
          if (F.Address == 4) {
            Core.GPR[1] |= 0x1234567800000000ULL;
            Core.GPR[6] |= 0x2345678900000000ULL;
            Core.GPR[7] |= 0x3456789a00000000ULL;
          }
          Core.RFlags |= 0x8d5;
          State.NMIBlocked = 1;
          State.DS = State.ES = State.Core.SS;
          State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
          put(0x13110, 0x32007);
          put(0x13118, 0x33007);
          put(PTE, 0);
          for (unsigned I = 0; I != 0x4000; ++I)
            RAM.Memory.front().Bytes[0x30000 + I] = 0x11;
          put(0x8024, 0x1c000);
          put(0x60e0, 0x2000 | (uint64_t(8) << 16) | (uint64_t(1) << 32) |
                          (uint64_t(0x8e) << 40));
          X64ContinuationStateV6 Native;
          bool Handler = false;
          ASSERT_NO_FATAL_FAILURE(CPU.execute(
              RAM.Memory.front().Bytes, State, Controls.CR0, F.Length, Native,
              Handler, false, nullptr, Controls.CR4, true, false));
          ASSERT_FALSE(Handler);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(
              *Memory, {.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst,
                        .RepeatProfile =
                            CPU.ReturnCoreType == 0x20
                                ? X64PagedRepeatProfile::PreserveLodsCountHigh
                                : X64PagedRepeatProfile::ClearLodsCountHigh});
          Shadows Shadow;
          Producer Events;
          Events.Ready = 0;
          for (unsigned Phase = 0; Phase != 3; ++Phase) {
            uint64_t Budget = Phase == 0   ? Completed + 1
                              : Phase == 1 ? 3
                                           : 4 - Completed;
            uint64_t Polls = Budget;
            EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                                &Budget, &Shadow, &Polls, Producer::peek,
                                Producer::accepted, &Events),
                      Phase == 0   ? 7u
                      : Phase == 1 ? 6u
                                   : 9u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_EQ(Core.PC, Phase == 0   ? 0x2000
                               : Phase == 1 ? F.PC
                                            : F.PC + F.Length);
            EXPECT_EQ(State.NMIBlocked, Phase == 0 ? 1 : 0);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          }
          expectPagedSegmentCPU(State, Native, *Memory, CPU);
          ++Comparisons;
          if (HasFailure())
            return;
        }
    }
  RecordProperty("cpu_comparisons", Comparisons);
}
#endif
