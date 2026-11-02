//===- X64PagedScheduleTests.inc - Faultable IRQ/NMI scheduling -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included after the paged source fixture; compares generated scheduling with
/// independent original CPU entry and checks producer consumption on faults.
//===----------------------------------------------------------------------===//

TEST_F(X64PagedExecutionTest, ScheduledEventsPreserveBudgetsAndShadows) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0xfb, 0x90, 0x90, 0xfa, 0xfb, 0x90};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compile(Code, LLVM, "-O2", Driver,
                {0x1000, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005},
                NEVERD_TEST_CLANG, 0x1000, &Scheduled));
    reset(Code);
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory);
    auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
    Shadows Shadow;
    Producer P;
    for (unsigned Step = 0; Step != 3; ++Step) {
      uint64_t Budget = 1, Polls = 1;
      EXPECT_EQ(Scheduled(&State, Opaque, &Budget, &Shadow, &Polls,
                          Producer::peek, Producer::accepted, &P),
                Step == 2 ? 7u : 9u);
      EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
      EXPECT_EQ(Budget, Step == 2 ? 1u : 0u);
      EXPECT_EQ(Polls, 0u);
      EXPECT_EQ(P.Polls, Step + 1);
      EXPECT_EQ(P.Acks, Step == 2 ? 1u : 0u);
      EXPECT_EQ(Shadow.STIBlocked, Step == 0 ? 1 : 0);
      EXPECT_EQ(State.Core.Core.Core.Core.PC,
                Step == 2 ? 0x4410u : 0x1001u + Step);
    }
    EXPECT_TRUE(Context.lastResult().Delivery->InputAccepted);
    EXPECT_FALSE(Context.lastResult().InstructionAttempted);
  }
}

TEST_F(X64PagedExecutionTest, ScheduledAdmissionPrecedesPollingAndRAM) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile({0x90}, LLVM, "-O2", Driver, {0x1000},
                                    NEVERD_TEST_CLANG, 0x1000, &Scheduled));
    for (unsigned Case = 0; Case != 11; ++Case) {
      SCOPED_TRACE(Case);
      reset({0x90});
      Shadows Shadow;
      Producer P;
      uint64_t Budget = 1, Polls = 1, Expected = 5;
      switch (Case) {
      case 0:
        State.Core.Core.Core.Core.RFlags = 0;
        break;
      case 1:
        Controls.CR4 |= uint64_t(1) << 32;
        break;
      case 2:
        State.Core.Core.Core.Core.PC = 0x2000;
        Expected = 8;
        break;
      case 3:
        Budget = 0;
        Expected = 9;
        break;
      case 4:
        Polls = 0;
        Expected = 16;
        break;
      case 5:
        P.Ready = 2;
        break;
      case 6:
        P.Candidate.Kind = 4;
        break;
      case 7:
        P.Candidate.Kind = 3;
        P.Candidate.Vector = 3;
        break;
      case 8:
        Shadow.Reserved[3] = 1;
        break;
      case 9:
        State.NMIBlocked = 2;
        break;
      case 10:
        Shadow.STIBlocked = 1;
        break; // IF must be set.
      }
      const auto Before = State;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory);
      EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                          &Budget, &Shadow, &Polls, Producer::peek,
                          Producer::accepted, &P),
                Expected);
      EXPECT_EQ(P.Polls, Case >= 5 && Case <= 7 ? 1u : 0u);
      EXPECT_EQ(P.Acks, 0u);
      EXPECT_EQ(Budget, Case == 3 ? 0u : 1u);
      EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
      EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
                RAM.Memory.front().Bytes);
    }
  }
}

TEST_F(X64PagedExecutionTest, ScheduledConsumptionSurvivesFailedEntry) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0x8b, 0x07};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                    NEVERD_TEST_CLANG, 0x1000, &Scheduled));
    for (unsigned Case = 0; Case != 8; ++Case) {
      SCOPED_TRACE(Case);
      reset(Code);
      State.Core.Core.Core.Core.RFlags |= 512;
      Shadows Shadow;
      Producer P;
      if (Case == 1) // IRQ's gate is absent; #NP has a usable gate.
        RAM.Memory.front().Bytes[0x6415] &= ~0x80;
      if (Case == 2 || Case == 3) {
        std::memmove(RAM.Memory.front().Bytes.data() + 0x6c00,
                     RAM.Memory.front().Bytes.data() + 0x6000, 4096);
        State.IDTR.Base = 0x6c00;
        put(0x13038, Case == 2 ? 0 : 0x50007); // Gate PF or backing failure.
      }
      if (Case == 4)
        State.IDTR.Limit = 0; // #GP -> #GP -> #DF -> shutdown.
      if (Case >= 5) {
        Shadow.SSBlocked = 1; // External event remains pending.
        if (Case == 5) {
          put(0x13100, 0); // Instruction faults, then IDT backing fails.
          put(0x13030, 0x50007);
        } else if (Case == 6) {
          RAM.Memory.front().Bytes[0x1000] = 0x90;
        } else {
          put(0x13100, 0x50007); // Operand backing fails before any fault.
        }
      }
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory);
      uint64_t Budget = 1, Polls = 1;
      EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                          &Budget, &Shadow, &Polls, Producer::peek,
                          Producer::accepted, &P),
                Case == 3 || Case == 5 || Case == 7 ? 18u
                : Case == 4                         ? 19u
                : Case == 6                         ? 20u
                                                    : 7u);
      EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
      EXPECT_EQ(Budget, Case == 5 ? 0u : 1u);
      EXPECT_EQ(P.Polls, 1u);
      EXPECT_EQ(P.Acks, Case < 5 ? 1u : 0u);
      EXPECT_EQ(Shadow.SSBlocked, Case >= 6 ? 1 : 0);
      if (Case < 3) {
        ASSERT_TRUE(Context.lastResult().Delivery);
        EXPECT_TRUE(Context.lastResult().Delivery->InputAccepted);
        EXPECT_EQ(Context.lastResult().Delivery->DeliveredVector, Case == 0 ? 65
                                                                  : Case == 1
                                                                      ? 11
                                                                      : 14);
      }
      if (Case == 5) {
        // The ordinary paged dispatcher charges this same attempted fault.
        reset(Code);
        put(0x13100, 0);
        put(0x13030, 0x50007);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Plain(*Memory);
        Budget = 1;
        EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Plain), &Budget),
                  18u);
        EXPECT_EQ(Budget, 0u);
      }
    }
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, ScheduledEventsMatchOriginalCPU) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  unsigned Comparisons = 0, Accepted = 0, Secondary = 0;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  const std::vector<uint8_t> Code{0x90, 0x90};
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, {0x1000, 0x1001},
                                        Compiler, 0x1000, &Scheduled));
        for (unsigned Input = 0; Input != 257; ++Input)
          for (unsigned Variant = 0; Variant != 8; ++Variant) {
            const bool NMI = Input == 256;
            const unsigned Vector = NMI ? 2 : Input;
            // Keep the #NP handler valid; undefined double-fault frames have
            // their own qualification and cannot be compared as defined state.
            if (Variant == 2 && (Vector == 11 || Vector == 13))
              continue;
            SCOPED_TRACE(testing::Message()
                         << Compiler << ' ' << LLVM << ' ' << O
                         << " vector=" << Input << " variant=" << Variant);
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.RFlags |= 512;
            Core.GPR[4] -= Variant;
            Shadows Shadow;
            Producer P;
            P.Candidate = {uint8_t(Vector), uint8_t(NMI ? 3 : 2)};
            if (Variant == 1)
              Core.RFlags &= ~uint64_t(512);
            if (Variant == 2)
              RAM.Memory.front().Bytes[0x6005 + Vector * 16] &= ~0x80;
            if (Variant == 3)
              put(0x13008, 0); // IRQ/NMI precedes fetch #PF.
            if (Variant == 4)
              Shadow.SSBlocked = 1;
            if (Variant == 5) {
              State.NMIBlocked = 1;
              Core.RFlags |= 0x10000;
            }
            if (Variant == 6) {
              State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
              State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
            }
            if (Variant == 7) {
              RAM.Memory.front().Bytes[0x6005 + Vector * 16] |= 1; // Trap gate.
              Core.RFlags |= 0x40000;
            }
            X64ContinuationEventV1 Event;
            Event.Kind = NMI ? X64ContinuationEventKind::NMI
                             : X64ContinuationEventKind::Interrupt;
            Event.Vector = Vector;
            Event.SSBlocked = Shadow.SSBlocked;
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(RAM.Memory.front().Bytes, State,
                                                Controls.CR0, 1, Native,
                                                Handler, false, &Event));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(*Memory);
            uint64_t Budget = Variant == 4 ? 2 : 1, Polls = Budget;
            const auto Exit = Scheduled(
                &State, reinterpret_cast<uint8_t *>(&Context), &Budget, &Shadow,
                &Polls, Producer::peek, Producer::accepted, &P);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Exit, Handler ? 7u : 9u);
            EXPECT_EQ(Budget, Handler ? 1u : 0u);
            EXPECT_EQ(P.Acks, Handler ? 1u : 0u);
            EXPECT_EQ(P.Polls, Variant == 4 ? 2u : 1u);
            const auto &Expected = Native.Core.Core.Core.Core;
            EXPECT_EQ(Core.PC, Expected.PC);
            EXPECT_EQ(Core.RFlags, Expected.RFlags);
            for (unsigned I = 0; I != 16; ++I)
              EXPECT_EQ(Core.GPR[I], Expected.GPR[I]) << I;
            EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
            EXPECT_EQ(State.NMIBlocked, Native.NMIBlocked);
            EXPECT_EQ(State.Core.CS.Selector, Native.Core.CS.Selector);
            EXPECT_EQ(State.Core.SS.Selector, Native.Core.SS.Selector);
            const auto Actual =
                Memory->physicalMemory().snapshotMemoryRegions();
            EXPECT_EQ(
                std::memcmp(Actual.front().Bytes.data(), CPU.RAM, 0x10000), 0);
            EXPECT_EQ(std::memcmp(Actual.front().Bytes.data() + 0x14000,
                                  CPU.RAM + 0x14000,
                                  X64PagedNativeCPU::Size - 0x14000),
                      0);
            ++Comparisons;
            Accepted += P.Acks;
            Secondary +=
                Context.lastResult().Delivery &&
                Context.lastResult().Delivery->LastException.has_value();
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("accepted_cpu_comparisons", Accepted);
  RecordProperty("secondary_fault_cpu_comparisons", Secondary);
}
TEST_F(X64PagedExecutionTest, PagedCliStiMatchPrivilegeAndVirtualFlags) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  const std::vector<uint8_t> Code{0xfa, 0xfb};
  unsigned Comparisons = 0;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, {0x1000, 0x1001},
                                        Compiler, 0x1000, &Scheduled));
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (unsigned IOPL = 0; IOPL != 4; ++IOPL)
            for (unsigned Flags = 0; Flags != 8; ++Flags)
              for (unsigned PVI = 0; PVI != 2; ++PVI)
                for (unsigned Set = 0; Set != 2; ++Set) {
                  SCOPED_TRACE(testing::Message()
                               << Compiler << ' ' << LLVM << ' ' << O << " CPL="
                               << CPL << " IOPL=" << IOPL << " flags=" << Flags
                               << " PVI=" << PVI << " STI=" << Set);
                  reset(Code);
                  auto &Core = State.Core.Core.Core.Core;
                  Core.PC += Set;
                  Core.RFlags |= (IOPL << 12) | ((Flags & 1) ? 512 : 0) |
                                 ((Flags & 2) ? 0x80000 : 0) |
                                 ((Flags & 4) ? 0x100000 : 0);
                  Controls.CR4 |= PVI << 1;
                  State.Core.CS = {0, UINT32_MAX, uint16_t(40 | CPL),
                                   uint16_t(0xa09b | (CPL << 5))};
                  State.Core.SS = {0, UINT32_MAX, uint16_t(48 | CPL),
                                   uint16_t(0xc093 | (CPL << 5))};
                  put(0x5028, 0x00af9b000000ffffULL | (uint64_t(CPL) << 45));
                  put(0x5030, 0x00cf93000000ffffULL | (uint64_t(CPL) << 45));
                  X64ContinuationStateV6 Native;
                  bool Handler = false;
                  ASSERT_NO_FATAL_FAILURE(CPU.execute(
                      RAM.Memory.front().Bytes, State, Controls.CR0, 1, Native,
                      Handler, false, nullptr, Controls.CR4));
                  Memory = llvm::cantFail(
                      X86PagedMemory::create(RAM, Features, Controls));
                  X64PagedExecution Context(*Memory);
                  uint64_t Budget = 1, Polls = 1;
                  Shadows Shadow;
                  Producer P;
                  P.Ready = 0;
                  const auto Exit = Scheduled(
                      &State, reinterpret_cast<uint8_t *>(&Context), &Budget,
                      &Shadow, &Polls, Producer::peek, Producer::accepted, &P);
                  EXPECT_TRUE(Context.lastError().empty())
                      << Context.lastError();
                  EXPECT_EQ(Exit, Handler ? 7u : 9u);
                  EXPECT_EQ(Budget, 0u);
                  EXPECT_EQ(P.Acks, 0u);
                  EXPECT_EQ(P.Polls, 1u);
                  EXPECT_EQ(Shadow.STIBlocked,
                            !Handler && Set && !(Flags & 1) && IOPL >= CPL ? 1
                                                                           : 0);
                  const auto &Expected = Native.Core.Core.Core.Core;
                  EXPECT_EQ(Core.PC, Expected.PC);
                  EXPECT_EQ(Core.RFlags, Expected.RFlags);
                  for (unsigned I = 0; I != 16; ++I)
                    EXPECT_EQ(Core.GPR[I], Expected.GPR[I]);
                  EXPECT_EQ(State.Core.CS.Selector, Native.Core.CS.Selector);
                  EXPECT_EQ(State.Core.SS.Selector, Native.Core.SS.Selector);
                  const auto Actual =
                      Memory->physicalMemory().snapshotMemoryRegions();
                  EXPECT_EQ(std::memcmp(Actual.front().Bytes.data(), CPU.RAM,
                                        0x10000),
                            0);
                  EXPECT_EQ(std::memcmp(Actual.front().Bytes.data() + 0x14000,
                                        CPU.RAM + 0x14000,
                                        X64PagedNativeCPU::Size - 0x14000),
                            0);
                  ++Comparisons;
                }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}
TEST_F(X64PagedExecutionTest, VirtualInterruptFaultAndExternalEventPriority) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  unsigned Comparisons = 0;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile({0x90}, LLVM, O, Driver, {0x1000},
                                        Compiler, 0x1000, &Scheduled));
        for (unsigned EventKind = 0; EventKind != 3; ++EventKind)
          for (bool IF : {false, true})
            for (bool Present : {false, true}) {
              SCOPED_TRACE(testing::Message()
                           << Compiler << ' ' << LLVM << ' ' << O
                           << " event=" << EventKind << " IF=" << IF
                           << " present=" << Present);
              reset({0x90});
              auto &Core = State.Core.Core.Core.Core;
              Core.RFlags |= 0x180000 | (IF ? 512 : 0);
              Controls.CR4 |= 2;
              State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
              State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
              if (!Present)
                put(0x13008, 0);
              X64ContinuationEventV1 Event;
              Event.Kind = EventKind == 2 ? X64ContinuationEventKind::NMI
                                          : X64ContinuationEventKind::Interrupt;
              Event.Vector = EventKind == 2 ? 2 : 65;
              X64ContinuationStateV6 Native;
              bool Handler = false;
              ASSERT_NO_FATAL_FAILURE(CPU.execute(
                  RAM.Memory.front().Bytes, State, Controls.CR0, 1, Native,
                  Handler, false, EventKind ? &Event : nullptr, Controls.CR4));
              ASSERT_TRUE(Handler);
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              X64PagedExecution Context(*Memory);
              uint64_t Budget = 1, Polls = 1;
              Shadows Shadow;
              Producer P;
              P.Ready = EventKind != 0;
              P.Candidate = {Event.Vector, uint8_t(Event.Kind)};
              EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                                  &Budget, &Shadow, &Polls, Producer::peek,
                                  Producer::accepted, &P),
                        7u);
              EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
              const bool Accepted = EventKind == 2 || (EventKind == 1 && IF);
              EXPECT_EQ(P.Acks, Accepted ? 1u : 0u);
              EXPECT_EQ(Budget, Accepted ? 1u : 0u);
              const auto &Expected = Native.Core.Core.Core.Core;
              EXPECT_EQ(Core.PC, Expected.PC);
              EXPECT_EQ(Core.RFlags, Expected.RFlags);
              EXPECT_EQ(State.NMIBlocked, Native.NMIBlocked);
              EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
              for (unsigned I = 0; I != 16; ++I)
                EXPECT_EQ(Core.GPR[I], Expected.GPR[I]);
              const auto Actual =
                  Memory->physicalMemory().snapshotMemoryRegions();
              EXPECT_EQ(
                  std::memcmp(Actual.front().Bytes.data(), CPU.RAM, 0x10000),
                  0);
              EXPECT_EQ(std::memcmp(Actual.front().Bytes.data() + 0x14000,
                                    CPU.RAM + 0x14000,
                                    X64PagedNativeCPU::Size - 0x14000),
                        0);
              ++Comparisons;
            }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}
TEST_F(X64PagedExecutionTest, PagedSchedulingMatchesNativeInstructionWindows) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  struct Program {
    std::vector<uint8_t> Bytes;
    std::vector<uint64_t> Offsets;
    unsigned Completed;
    uint8_t InitialShadow;
    bool NMI;
  };
  const std::vector<Program> Programs{
      {{0xfb, 0x90, 0x90}, {0, 1, 2}, 2, 0, false},
      {{0xfb, 0xfb, 0x90}, {0, 1, 2}, 2, 0, false},
      {{0xfb, 0xfa, 0xfb, 0x90, 0x90}, {0, 1, 2, 3, 4}, 4, 0, false},
      {{0xfb, 0xeb, 0, 0x90}, {0, 1, 3}, 2, 0, false},
      {{0xfb, 0xe8, 1, 0, 0, 0, 0x90, 0x90}, {0, 1, 6, 7}, 2, 0, false},
      {{0x90, 0x90}, {0, 1}, 1, 1, false},
      {{0x90, 0x90}, {0, 1}, 1, 2, false},
      {{0x90, 0x90}, {0, 1}, 1, 2, true},
      {{0x90, 0x90}, {0, 1}, 1, 1, true}};
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses, Starts;
  for (const auto &P : Programs) {
    const auto Start = 0x1000 + Code.size();
    Starts.push_back(Start);
    for (auto Offset : P.Offsets)
      Addresses.push_back(Start + Offset);
    Code.insert(Code.end(), P.Bytes.begin(), P.Bytes.end());
    Code.resize(Code.size() + 16, 0x90);
  }
  unsigned Comparisons = 0;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, Addresses,
                                        Compiler, 0x1000, &Scheduled));
        for (unsigned Test = 0; Test != Programs.size(); ++Test)
          for (unsigned Variant = 0; Variant != 32; ++Variant) {
            SCOPED_TRACE(testing::Message()
                         << Compiler << ' ' << LLVM << ' ' << O
                         << " sequence=" << Test << " variant=" << Variant);
            const auto &Program = Programs[Test];
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = Starts[Test];
            Core.GPR[4] -= Variant & 15;
            Core.RFlags |=
                (Variant & 1 ? 0x10000 : 0) | (Program.InitialShadow ? 512 : 0);
            const uint8_t Vector = Program.NMI ? 2 : 65;
            if (Variant & 16)
              RAM.Memory.front().Bytes[0x6005 + Vector * 16] |= 1;
            X64ContinuationEventV1 Event;
            Event.Vector = Vector;
            Event.Kind = Program.NMI ? X64ContinuationEventKind::NMI
                                     : X64ContinuationEventKind::Interrupt;
            Event.STIBlocked = Program.InitialShadow == 1;
            Event.SSBlocked = Program.InitialShadow == 2;
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0,
                Program.Bytes.size(), Native, Handler, false, &Event));
            ASSERT_TRUE(Handler);
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(*Memory);
            uint64_t Budget = Program.Completed + 1, Polls = Budget;
            Shadows Shadow;
            Shadow.STIBlocked = Event.STIBlocked;
            Shadow.SSBlocked = Event.SSBlocked;
            Shadow.NMIBlockedBySTI = 1; // Qualified host policy.
            Producer P;
            P.Candidate = {Vector, uint8_t(Event.Kind)};
            EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                                &Budget, &Shadow, &Polls, Producer::peek,
                                Producer::accepted, &P),
                      7u);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Budget, 1u);
            EXPECT_EQ(Polls, 0u);
            EXPECT_EQ(P.Polls, Program.Completed + 1);
            EXPECT_EQ(P.Acks, 1u);
            EXPECT_EQ(Shadow.STIBlocked, 0);
            EXPECT_EQ(Shadow.SSBlocked, 0);
            const auto &Expected = Native.Core.Core.Core.Core;
            EXPECT_EQ(Core.PC, Expected.PC);
            EXPECT_EQ(Core.RFlags, Expected.RFlags);
            EXPECT_EQ(State.NMIBlocked, Native.NMIBlocked);
            EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
            for (unsigned I = 0; I != 16; ++I)
              EXPECT_EQ(Core.GPR[I], Expected.GPR[I]);
            const auto Actual =
                Memory->physicalMemory().snapshotMemoryRegions();
            EXPECT_EQ(
                std::memcmp(Actual.front().Bytes.data(), CPU.RAM, 0x10000), 0);
            EXPECT_EQ(std::memcmp(Actual.front().Bytes.data() + 0x14000,
                                  CPU.RAM + 0x14000,
                                  X64PagedNativeCPU::Size - 0x14000),
                      0);
            ++Comparisons;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}
TEST_F(X64PagedExecutionTest, SequentialFetchFaultSavesNoncanonicalPC) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  constexpr uint64_t Base = 0x7ffffffff000;
  constexpr uint64_t Start = Base + 4095;
  const std::vector<uint8_t> Code(4097, 0x90);
  unsigned Comparisons = 0;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver,
                                        {Start, Start + 1}, Compiler, Base,
                                        &Scheduled));
        reset(Code);
        auto &Core = State.Core.Core.Core.Core;
        Core.PC = Start;
        put(0x107f8, 0x14007);
        put(0x14ff8, 0x15007);
        put(0x15ff8, 0x16007);
        put(0x16ff8, 0x1007);
        X64ContinuationStateV6 Native;
        bool Handler = false;
        // Let the CPU execute NOP and attempt its sequential fetch. A host
        // breakpoint at the noncanonical next PC cannot be the stop oracle.
        ASSERT_NO_FATAL_FAILURE(CPU.execute(RAM.Memory.front().Bytes, State,
                                            Controls.CR0, 1, Native, Handler,
                                            true));
        ASSERT_TRUE(Handler);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Context(*Memory);
        uint64_t Budget = 2, Polls = 2;
        Shadows Shadow;
        Producer P;
        P.Ready = 0;
        EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                            &Budget, &Shadow, &Polls, Producer::peek,
                            Producer::accepted, &P),
                  7u);
        EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
        EXPECT_EQ(Budget, 0u);
        EXPECT_EQ(P.Polls, 2u);
        EXPECT_EQ(P.Acks, 0u);
        const auto &Expected = Native.Core.Core.Core.Core;
        EXPECT_EQ(Core.PC, Expected.PC);
        EXPECT_EQ(Core.RFlags, Expected.RFlags);
        for (unsigned I = 0; I != 16; ++I)
          EXPECT_EQ(Core.GPR[I], Expected.GPR[I]);
        EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
        const auto Actual = Memory->physicalMemory().snapshotMemoryRegions();
        EXPECT_EQ(std::memcmp(Actual.front().Bytes.data(), CPU.RAM, 0x10000),
                  0);
        EXPECT_EQ(std::memcmp(Actual.front().Bytes.data() + 0x17000,
                              CPU.RAM + 0x17000,
                              X64PagedNativeCPU::Size - 0x17000),
                  0);
        ++Comparisons;
      }
  RecordProperty("cpu_comparisons", Comparisons);
}
TEST_F(X64PagedExecutionTest, EventFramesPreserveFaultingAndInterruptedPCData) {
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  unsigned Comparisons = 0;
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    reset({0x90});
    auto &Core = State.Core.Core.Core.Core;
    X64ContinuationEventV1 Event;
    Event.Kind = X64ContinuationEventKind::Fault;
    Event.Vector = 13;
    if (Case < 2) {
      State.Core.CS = {0, 0x1000, 40, uint16_t(Case ? 0x409b : 0x009b)};
      put(0x5028, Case ? 0x00409b0000001000ULL : 0x00009b0000001000ULL);
      Event.SavedPC = 0x1001;
    } else {
      // POPFQ enables an IRQ without STI inhibition. KVM selects the next
      // boundary after the last canonical instruction byte independently.
      Core.PC = 0x7fffffffffff;
      put(0x107f8, 0x14007);
      put(0x14ff8, 0x15007);
      put(0x15ff8, 0x16007);
      put(0x16ff8, 0x1007);
      RAM.Memory.front().Bytes[0x1fff] = 0x9d;
      put(Core.GPR[4], 0x202);
      Event.Kind = X64ContinuationEventKind::Interrupt;
      Event.Vector = 65;
      Event.SavedPC = Core.PC + 1;
    }
    X64ContinuationStateV6 Native;
    bool Handler = false;
    ASSERT_NO_FATAL_FAILURE(CPU.execute(RAM.Memory.front().Bytes, State,
                                        Controls.CR0, 1, Native, Handler, true,
                                        Case == 2 ? &Event : nullptr));
    ASSERT_TRUE(Handler);
    Core.PC = Event.SavedPC;
    if (Case == 2) {
      Core.RFlags = 0x202;
      Core.GPR[4] += 8;
    }
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    auto Result = deliverX64PagedEvent(State, *Memory, Event, 0xffff);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Status, X64PagedEventStatus::Delivered);
    EXPECT_TRUE(Result->InputAccepted);
    EXPECT_EQ(Result->DeliveredVector, Event.Vector);
    const auto &Expected = Native.Core.Core.Core.Core;
    EXPECT_EQ(Core.PC, Expected.PC);
    EXPECT_EQ(Core.RFlags, Expected.RFlags);
    for (unsigned I = 0; I != 16; ++I)
      EXPECT_EQ(Core.GPR[I], Expected.GPR[I]);
    EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
    const auto Actual = Memory->physicalMemory().snapshotMemoryRegions();
    EXPECT_EQ(std::memcmp(Actual.front().Bytes.data(), CPU.RAM, 0x10000), 0);
    EXPECT_EQ(std::memcmp(Actual.front().Bytes.data() + 0x17000,
                          CPU.RAM + 0x17000, X64PagedNativeCPU::Size - 0x17000),
              0);
    ++Comparisons;
  }
  RecordProperty("cpu_comparisons", Comparisons);
}
#endif
