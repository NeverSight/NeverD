//===- X64PagedExecutionBreakpointTests.inc - Original execute faults -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original hardware comparators, independent of KVM guest-debug stops.
//===----------------------------------------------------------------------===//

class X64PagedExecutionBreakpointTest : public X64PagedDebugTest {
protected:
  X64PagedExecutionOptions breakpointOptions(X64PagedDebugState &Debug) {
    auto O = options(Debug);
    O.DebugProfile = X86InstructionDebugProfile::ExecuteBreakpoints;
    return O;
  }
};

TEST_F(X64PagedExecutionBreakpointTest, BoundControlsAndBridgePrecedeEffects) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug({0x90}, LLVM, Driver, Scheduled));
    for (unsigned Case = 0; Case != 10; ++Case) {
      resetDebug({0x90});
      X64PagedDebugState Debug;
      Debug.DR[0] = 0x1000;
      Debug.DR7 = 0x401;
      auto O = breakpointOptions(Debug);
      if (Case == 0)
        O.DebugProfile = X86InstructionDebugProfile::InstructionStep;
      else if (Case == 1)
        O.DebugProfile = static_cast<X86InstructionDebugProfile>(255);
      else if (Case == 2)
        Debug.DR7 |= 0x10000; // Data write comparator.
      else if (Case == 3)
        Debug.DR7 |= 0x20000; // I/O comparator.
      else if (Case == 4)
        Debug.DR7 |= 0x40000; // Undefined execution length.
      else if (Case == 5)
        Debug.DR7 |= 0x2000; // GD.
      else if (Case == 6)
        Debug.DR7 |= 0x800; // RTM.
      else if (Case == 7)
        Debug.DR7 |= uint64_t{1} << 32; // PT redirection.
      else if (Case == 8)
        Debug.DebugCTL = 2; // BTF.
      else
        Debug.DR7 &= ~uint64_t{0x400};
      const auto Before = State;
      const auto BeforeDebug = Debug;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, O);
      Shadows Shadow;
      Producer P;
      uint64_t Budget = 1, Polls = 1;
      EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                          &Budget, &Shadow, &Polls, Producer::peek,
                          Producer::accepted, &P),
                5u)
          << Case;
      EXPECT_EQ(Budget, 1u);
      EXPECT_EQ(Polls, 1u);
      EXPECT_EQ(P.Polls, 0u);
      EXPECT_FALSE(Context.lastError().empty());
      EXPECT_EQ(std::memcmp(&Before, &State, sizeof(State)), 0);
      EXPECT_EQ(std::memcmp(&BeforeDebug, &Debug, sizeof(Debug)), 0);
      EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
                RAM.Memory.front().Bytes);
    }
    resetDebug({0x90});
    State.Core.Core.Core.Core.RFlags = 0x202;
    X64PagedDebugState Debug;
    Debug.DR7 = 0x401;
    Debug.DR[0] = 0x1000;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory, breakpointOptions(Debug));
    auto Valid = Context.validate(
        State, 0x1000, [](void *, void *) -> uint64_t { return 0; }, 48);
    ASSERT_FALSE(bool(Valid));
    llvm::consumeError(Valid.takeError());
  }
}

TEST_F(X64PagedExecutionBreakpointTest,
       BreakpointPrecedesFetchAndResumesAcceptedEntry) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compileDebug({0x48, 0xff, 0x07}, LLVM, Driver, Scheduled));
    resetDebug({0x90});    // Original byte mismatch is below the execute fault.
    put(0x13008, 0x50067); // Even instruction backing need not be present.
    put(0x13030, 0x51067); // Accepted #DB stops at its missing IDT backing.
    X64PagedDebugState Debug;
    Debug.DR7 = 0x401;
    Debug.DR[0] = 0x1000;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory, breakpointOptions(Debug));
    uint64_t Budget = 1;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u);
    EXPECT_EQ(Budget, 0u);
    EXPECT_TRUE(Context.lastResult().InstructionAttempted);
    EXPECT_FALSE(Context.lastResult().InstructionCompleted);
    EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x1000u);
    EXPECT_EQ(Debug.DR6, 0xffff0ff1u);
    ASSERT_TRUE(Context.lastResult().Delivery);
    ASSERT_TRUE(Context.lastResult().Delivery->Continuation);
    EXPECT_EQ(Context.lastResult().Delivery->Continuation->Event.Kind,
              X64ContinuationEventKind::InstructionBreakpoint);
    ASSERT_EQ(Memory->physicalMemory()
                  .storeScalar(0x13030, GuestScalarWidth::I64, 0x6067)
                  .Status,
              GuestMemoryAccessStatus::Completed);
    auto Resumed = Context.resume(
        State, *Context.lastResult().Delivery->Continuation, 0xffff);
    ASSERT_TRUE(bool(Resumed)) << llvm::toString(Resumed.takeError());
    EXPECT_EQ(Resumed->Exit, 7u);
    EXPECT_FALSE(Resumed->InstructionAttempted);
    EXPECT_EQ(Debug.DR6, 0xffff0ff1u);
    EXPECT_EQ(Memory->physicalMemory()
                  .loadScalar(0x30000, GuestScalarWidth::I64)
                  .Value,
              0x8877665544332211ULL);
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionBreakpointTest, OriginalComparatorsAndFaultPriority) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  enum class Setup {
    Ordinary,
    OperandFault,
    FetchFault,
    HighAddress,
    MissingGate,
    StackFault,
    VirtualInterrupt,
    InterruptReturn,
    ReturnWithRF,
    Repeat,
  };
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    std::vector<unsigned> PCs;
    unsigned BreakpointOffset = 0;
    Setup Condition = Setup::Ordinary;
  };
  const Case Cases[] = {
      {"nop", {0x90, 0xcc}, {0, 1}},
      {"increment", {0x48, 0xff, 0xc0, 0xcc}, {0, 3}},
      {"store", {0x48, 0x89, 0x07, 0xcc}, {0, 3}},
      {"ud2", {0x0f, 0x0b}, {0}},
      {"icebp", {0xf1}, {0}},
      {"operand-fault",
       {0x48, 0x8b, 0x07, 0xcc},
       {0, 3},
       0,
       Setup::OperandFault},
      {"fetch-fault", {0x90, 0xcc}, {0, 1}, 0, Setup::FetchFault},
      {"movss", {0x8e, 0xd0, 0x90, 0xcc}, {0, 2, 3}, 2},
      {"next-nop", {0x90, 0xcc}, {0, 1}, 1},
      {"middle-byte", {0x66, 0x90, 0xcc}, {0, 2}, 1},
      {"full-address", {0x90, 0xcc}, {0, 1}, 0, Setup::HighAddress},
      {"missing-debug-gate", {0x90, 0xcc}, {0, 1}, 0, Setup::MissingGate},
      {"consecutive-ss", {0x8e, 0xd0, 0x8e, 0xd0, 0x90, 0xcc}, {0, 2, 4, 5}, 4},
      {"next-increment", {0x48, 0xff, 0xc0, 0xcc}, {0, 3}, 3},
      {"next-branch", {0xeb, 0x00, 0xcc}, {0, 2}, 2},
      {"next-icebp", {0xf1, 0xcc}, {0, 1}, 1},
      {"debug-stack-fault", {0x90, 0xcc}, {0, 1}, 0, Setup::StackFault},
      {"virtual-interrupt", {0x90, 0xcc}, {0, 1}, 0, Setup::VirtualInterrupt},
      {"iret", {0x48, 0xcf}, {0}, 0, Setup::InterruptReturn},
      {"iret-rf", {0x48, 0xcf}, {0}, 0, Setup::ReturnWithRF},
      {"rep-movs", {0xf3, 0xa4, 0xcc}, {0, 2}, 0, Setup::Repeat},
  };
  std::vector<uint8_t> Code(0x802, 0xf4);
  Code[0x800] = 0x90;
  Code[0x801] = 0xcc;
  std::vector<uint64_t> PCs{0x1800, 0x1801};
  for (unsigned I = 0; I != std::size(Cases); ++I) {
    std::copy(Cases[I].Bytes.begin(), Cases[I].Bytes.end(),
              Code.begin() + I * 32);
    for (unsigned Offset : Cases[I].PCs)
      PCs.push_back(0x1000 + I * 32 + Offset);
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
        for (unsigned I = 0; I != std::size(Cases); ++I)
          for (unsigned CPL : {0u, 1u, 2u, 3u})
            for (unsigned Flags = 0; Flags != 4; ++Flags)
              for (unsigned Comparator = 0; Comparator != 13; ++Comparator) {
                const auto Condition = Cases[I].Condition;
                if (Condition == Setup::StackFault && !CPL)
                  continue;
                SCOPED_TRACE(testing::Message()
                             << Cases[I].Name << " cpl=" << CPL << " flags="
                             << Flags << " comparator=" << Comparator << " "
                             << Compiler << Optimization << " llvm=" << LLVM);
                resetDebug(Code, CPL);
                auto &Core = State.Core.Core.Core.Core;
                Core.PC = 0x1000 + I * 32;
                Core.RFlags = 0x202 | ((Flags & 1) ? 0x100 : 0) |
                              ((Flags & 2) ? 0x10000 : 0);
                Core.GPR[0] = State.Core.SS.Selector;
                RAM.Memory.front().Bytes[0x6035] |= 0x60;
                if (Condition == Setup::OperandFault)
                  put(0x13100, 0);
                if (Condition == Setup::FetchFault)
                  put(0x13008, 0);
                if (Condition == Setup::MissingGate)
                  RAM.Memory.front().Bytes[0x6015] &= ~0x80;
                if (Condition == Setup::StackFault) {
                  put(0x130e8, 0);
                  RAM.Memory.front().Bytes[0x60e4] = 1;
                }
                if (Condition == Setup::VirtualInterrupt) {
                  Controls.CR4 |= 2;
                  Core.RFlags |= 0x180000;
                }
                const bool Returning = Condition == Setup::InterruptReturn ||
                                       Condition == Setup::ReturnWithRF;
                if (Returning) {
                  put(Core.GPR[4], 0x1800);
                  put(Core.GPR[4] + 8, State.Core.CS.Selector);
                  put(Core.GPR[4] + 16,
                      Condition == Setup::ReturnWithRF ? 0x10202 : 0x202);
                  put(Core.GPR[4] + 24, 0x1c000);
                  put(Core.GPR[4] + 32, State.Core.SS.Selector);
                }
                if (Condition == Setup::Repeat) {
                  Core.GPR[1] = 3;
                  Core.GPR[6] = 0x21000;
                }
                X64PagedDebugState Debug;
                Debug.DR6 = Comparator & 1 ? 0xffffefff : 0xffff0ff0;
                Debug.DR7 =
                    0x400 | (Comparator == 12 ? 0xffu
                                              : (uint64_t{1 + Comparator % 3}
                                                 << (Comparator / 3 * 2)));
                if (Comparator & 1)
                  Debug.DR7 |= 0x300; // LE/GE preserve.
                Debug.DR.fill(
                    Returning
                        ? 0x1800
                        : Core.PC + Cases[I].BreakpointOffset +
                              (Condition == Setup::HighAddress
                                   ? (uint64_t{1} << (Comparator & 1 ? 47 : 32))
                                   : 0));
                Memory = llvm::cantFail(
                    X86PagedMemory::create(RAM, Features, Controls));
                X64PagedExecution Context(*Memory, breakpointOptions(Debug));
                X64ContinuationStateV6 Native;
                bool Fault = false;
                CPU.DebugRegisters.emplace();
                CPU.DebugRegisters->dr6 = Debug.DR6;
                CPU.DebugRegisters->dr7 = Debug.DR7;
                std::copy(Debug.DR.begin(), Debug.DR.end(),
                          CPU.DebugRegisters->db);
                ASSERT_NO_FATAL_FAILURE(
                    CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                                Cases[I].Bytes.size(), Native, Fault, true,
                                nullptr, Controls.CR4, true, false));
                ASSERT_TRUE(Fault);
                Shadows Shadow;
                Producer P;
                P.Ready = 0;
                uint64_t Budget = 8, Polls = 8;
                uint64_t Exit;
                do {
                  Exit = Scheduled(
                      &State, reinterpret_cast<uint8_t *>(&Context), &Budget,
                      &Shadow, &Polls, Producer::peek, Producer::accepted, &P);
                } while (Exit == 6 && Budget);
                ASSERT_EQ(Exit, 7u) << Context.lastError();
                EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
                EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
                ASSERT_NO_FATAL_FAILURE(
                    expectPagedSegmentCPU(State, Native, *Memory, CPU));
                const auto Actual = Memory->physicalMemory()
                                        .snapshotMemoryRegions()
                                        .front()
                                        .Bytes;
                const auto Diff =
                    std::mismatch(Actual.begin(), Actual.end(), CPU.RAM);
                EXPECT_EQ(Diff.first, Actual.end())
                    << "RAM difference at " << std::hex
                    << std::distance(Actual.begin(), Diff.first);
                EXPECT_EQ(Shadow.SSBlocked, bool(CPU.InterruptShadow &
                                                 KVM_X86_SHADOW_INT_MOV_SS));
                if (HasFailure())
                  return;
                ++Comparisons;
              }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionBreakpointTest, OriginalHandlerResumesAndRepairsOnce) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const auto *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Comparisons = 0;
  for (unsigned Case = 0; Case != 3; ++Case) {
    std::vector<uint8_t> Code(0x3100, 0xf4);
    const std::vector<uint8_t> Entry =
        Case == 0   ? std::vector<uint8_t>{0x48, 0xff, 0x07}
        : Case == 1 ? std::vector<uint8_t>{0xf3, 0xa4}
                    : std::vector<uint8_t>{0x48, 0x8b, 0x07};
    std::copy(Entry.begin(), Entry.end(), Code.begin());
    Code[Entry.size()] = 0xcc;
    // Set RF and clear TF in the saved flags, count the handler and IRETQ.
    const std::vector<uint8_t> Handler{
        0x48, 0x81, 0x4c, 0x24, 0x10, 0,    0,    1,    0,    0x48,
        0x81, 0x64, 0x24, 0x10, 0xff, 0xfe, 0xff, 0xff, 0x48, 0xff,
        0x04, 0x25, 0,    0x20, 2,    0,    0x48, 0xcf};
    std::copy(Handler.begin(), Handler.end(), Code.begin() + 0x3010);
    std::vector<uint64_t> PCs{
        0x1000, 0x1000 + Entry.size(), 0x4010, 0x4019, 0x4022, 0x402a};
    if (Case == 2) {
      const std::vector<uint8_t> Repair{0x48, 0xc7, 0x04, 0x25, 0,    0x31,
                                        1,    0,    0x67, 0,    3,    0,
                                        0x48, 0x83, 0xc4, 8,    0x48, 0xcf};
      std::copy(Repair.begin(), Repair.end(), Code.begin() + 0x30e0);
      PCs.insert(PCs.end(), {0x40e0, 0x40ec, 0x40f0});
    }
    for (const auto &Compiler : Compilers)
      for (bool LLVM : {false, true})
        for (const std::string Optimization : {"-O0", "-O2"}) {
          Run Driver = nullptr;
          Schedule Scheduled = nullptr;
          ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled,
                                               PCs, Compiler, Optimization));
          for (unsigned CPL : {0u, 1u, 2u, 3u})
            for (bool TF : {false, true})
              for (unsigned Comparator = 0; Comparator != 13; ++Comparator) {
                SCOPED_TRACE(testing::Message()
                             << "case=" << Case << " cpl=" << CPL << " tf="
                             << TF << " comparator=" << Comparator << ' '
                             << Compiler << Optimization << " llvm=" << LLVM);
                resetDebug(Code, CPL);
                auto &Core = State.Core.Core.Core.Core;
                Core.RFlags = TF ? 0x302 : 0x202;
                Core.GPR[1] = 3;
                Core.GPR[6] = 0x21000;
                if (Case == 2)
                  put(0x13100, 0);
                RAM.Memory.front().Bytes[0x6035] |= 0x60;
                X64PagedDebugState Debug;
                Debug.DR7 =
                    0x400 | (Comparator == 12 ? 0xffu
                                              : uint64_t{1 + Comparator % 3}
                                                    << (Comparator / 3 * 2));
                Debug.DR.fill(0x1000);
                Memory = llvm::cantFail(
                    X86PagedMemory::create(RAM, Features, Controls));
                X64PagedExecution Context(*Memory, breakpointOptions(Debug));
                CPU.DebugRegisters.emplace();
                CPU.DebugRegisters->dr6 = Debug.DR6;
                CPU.DebugRegisters->dr7 = Debug.DR7;
                std::copy(Debug.DR.begin(), Debug.DR.end(),
                          CPU.DebugRegisters->db);
                X64ContinuationStateV6 Native;
                bool Fault = false;
                ASSERT_NO_FATAL_FAILURE(CPU.execute(
                    RAM.Memory.front().Bytes, State, Controls.CR0, Entry.size(),
                    Native, Fault, true, nullptr, Controls.CR4, true, false));
                ASSERT_TRUE(Fault);
                ASSERT_EQ(Native.Core.Core.Core.Core.PC, 0x4030u);
                Shadows Shadow;
                Producer P;
                P.Ready = 0;
                uint64_t Budget = 32, Polls = 32;
                unsigned Stops = 0;
                while (Core.PC != 0x4030 && Budget && Stops != 8) {
                  const auto Exit = Scheduled(
                      &State, reinterpret_cast<uint8_t *>(&Context), &Budget,
                      &Shadow, &Polls, Producer::peek, Producer::accepted, &P);
                  ASSERT_TRUE(Exit == 7 || Exit == 6)
                      << Exit << ' ' << Context.lastError();
                  Stops += Exit == 7;
                }
                EXPECT_EQ(Stops, Case == 2 ? 3u : 2u);
                EXPECT_EQ(Memory->physicalMemory()
                              .loadScalar(0x22000, GuestScalarWidth::I64)
                              .Value,
                          1u);
                EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
                EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
                ASSERT_NO_FATAL_FAILURE(
                    expectPagedSegmentCPU(State, Native, *Memory, CPU));
                const auto Actual = Memory->physicalMemory()
                                        .snapshotMemoryRegions()
                                        .front()
                                        .Bytes;
                EXPECT_TRUE(std::equal(Actual.begin(), Actual.end(), CPU.RAM));
                if (HasFailure())
                  return;
                ++Comparisons;
              }
        }
  }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("round_trip_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionBreakpointTest, CompatibilityReturnMatchesLinearPC) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  std::vector<uint8_t> Code(0x52, 0xf4);
  std::vector<uint64_t> PCs;
  const unsigned Widths[]{2, 4, 8};
  for (unsigned I = 0; I != 6; ++I) {
    const auto Width = Widths[I % 3];
    unsigned Offset = I * 16;
    PCs.push_back(0x1000 + Offset);
    if (Width != 4)
      Code[Offset++] = Width == 2 ? 0x66 : 0x48;
    Code[Offset] = I < 3 ? 0xcf : 0xcb;
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
        for (unsigned I = 0; I != 6; ++I)
          for (unsigned CPL : {0u, 1u, 2u, 3u})
            for (bool Code32 : {false, true})
              for (uint32_t Base : {0u, 0x2000u, 0xfffff000u})
                for (bool RF : {false, true})
                  for (bool Linear : {false, true}) {
                    const auto Width = Widths[I % 3];
                    const bool Far = I >= 3;
                    SCOPED_TRACE(testing::Message()
                                 << "far=" << Far << " width=" << Width
                                 << " cpl=" << CPL << " code32=" << Code32
                                 << " base=" << std::hex << Base << " rf=" << RF
                                 << " linear=" << Linear << ' ' << Compiler
                                 << Optimization << " llvm=" << LLVM);
                    resetDebug(Code);
                    auto &Core = State.Core.Core.Core.Core;
                    Core.PC = 0x1000 + I * 16;
                    Core.RFlags = 0x302 | (RF ? 0x10000 : 0);
                    // Zero input high halves bind the compatible zero/preserve
                    // processor choices without masking any resulting state.
                    for (unsigned R = 0; R != 8; ++R)
                      Core.GPR[R] = uint32_t(Core.GPR[R]);
                    put(0x5018, (Code32 ? 0x00cf9b000000ffffULL
                                        : 0x008f9b000000ffffULL) |
                                    (uint64_t(CPL) << 45) |
                                    (uint64_t(Base & 0xffffff) << 16) |
                                    (uint64_t(Base >> 24) << 56));
                    put(0x5020, 0x00cf93000000ffffULL | (uint64_t(CPL) << 45));
                    const std::vector<uint64_t> Frame =
                        Far ? std::vector<uint64_t>{0x1800, 24u | CPL, 0x1c000,
                                                    32u | CPL}
                            : std::vector<uint64_t>{0x1800, 24u | CPL,
                                                    RF ? 0x10202u : 0x202u,
                                                    0x1c000, 32u | CPL};
                    for (unsigned F = 0; F != Frame.size(); ++F)
                      for (unsigned B = 0; B != Width; ++B)
                        RAM.Memory.front().Bytes[Core.GPR[4] + F * Width + B] =
                            Frame[F] >> (B * 8);
                    X64PagedDebugState Debug;
                    Debug.DR7 = 0x4ff;
                    Debug.DR.fill(Linear ? uint32_t(Base + 0x1800) : 0x1800);
                    auto O = breakpointOptions(Debug);
                    if (CPU.ReturnCoreType == 0x20)
                      O.FarReturnProfile =
                          X64PagedFarReturnProfile::SelectorFirstTruncatedIP;
                    Memory = llvm::cantFail(
                        X86PagedMemory::create(RAM, Features, Controls));
                    X64PagedExecution Context(*Memory, O);
                    CPU.DebugRegisters.emplace();
                    CPU.DebugRegisters->dr6 = Debug.DR6;
                    CPU.DebugRegisters->dr7 = Debug.DR7;
                    std::copy(Debug.DR.begin(), Debug.DR.end(),
                              CPU.DebugRegisters->db);
                    X64ContinuationStateV6 Native;
                    bool Fault = false;
                    ASSERT_NO_FATAL_FAILURE(CPU.execute(
                        RAM.Memory.front().Bytes, State, Controls.CR0,
                        Width == 4 ? 1 : 2, Native, Fault, true, nullptr,
                        Controls.CR4, true, false));
                    ASSERT_TRUE(Fault);
                    ASSERT_EQ(Native.Core.Core.Core.Core.PC, 0x4010u);
                    uint64_t Budget = 1;
                    ASSERT_EQ(Driver(&State,
                                     reinterpret_cast<uint8_t *>(&Context),
                                     &Budget),
                              7u)
                        << Context.lastError();
                    EXPECT_TRUE(Context.lastResult().InstructionCompleted);
                    EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
                    EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
                    ASSERT_NO_FATAL_FAILURE(
                        expectPagedSegmentCPU(State, Native, *Memory, CPU));
                    const auto Actual = Memory->physicalMemory()
                                            .snapshotMemoryRegions()
                                            .front()
                                            .Bytes;
                    EXPECT_TRUE(
                        std::equal(Actual.begin(), Actual.end(), CPU.RAM));
                    if (HasFailure())
                      return;
                    ++Comparisons;
                  }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionBreakpointTest, OriginalExternalEventPriority) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const auto *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Comparisons = 0;
  const std::vector<uint8_t> Code{0x48, 0xff, 0xc0, 0xcc};
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string Optimization : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled,
                                             {0x1000, 0x1003}, Compiler,
                                             Optimization));
        for (unsigned CPL : {0u, 1u, 2u, 3u})
          for (unsigned Flags = 0; Flags != 4; ++Flags)
            for (bool NMI : {false, true})
              for (bool Eligible : {false, true}) {
                SCOPED_TRACE(testing::Message()
                             << "cpl=" << CPL << " flags=" << Flags << " nmi="
                             << NMI << " eligible=" << Eligible << ' '
                             << Compiler << Optimization << " llvm=" << LLVM);
                resetDebug(Code, CPL);
                auto &Core = State.Core.Core.Core.Core;
                Core.RFlags = 0x202 | ((Flags & 1) ? 0x100 : 0) |
                              ((Flags & 2) ? 0x10000 : 0);
                if (NMI)
                  State.NMIBlocked = !Eligible;
                else if (!Eligible)
                  Core.RFlags &= ~uint64_t{0x200};
                RAM.Memory.front().Bytes[0x6035] |= 0x60;
                X64PagedDebugState Debug{0xffffefff, 0x4ff};
                Debug.DR.fill(0x1000);
                Memory = llvm::cantFail(
                    X86PagedMemory::create(RAM, Features, Controls));
                X64PagedExecution Context(*Memory, breakpointOptions(Debug));
                X64ContinuationEventV1 Event;
                Event.Kind = NMI ? X64ContinuationEventKind::NMI
                                 : X64ContinuationEventKind::Interrupt;
                Event.Vector = NMI ? 2 : 65;
                CPU.DebugRegisters.emplace();
                CPU.DebugRegisters->dr6 = Debug.DR6;
                CPU.DebugRegisters->dr7 = Debug.DR7;
                std::copy(Debug.DR.begin(), Debug.DR.end(),
                          CPU.DebugRegisters->db);
                X64ContinuationStateV6 Native;
                bool Fault = false;
                ASSERT_NO_FATAL_FAILURE(CPU.execute(
                    RAM.Memory.front().Bytes, State, Controls.CR0, 3, Native,
                    Fault, true, &Event, Controls.CR4, true, false));
                ASSERT_TRUE(Fault);
                Shadows Shadow;
                Producer P;
                P.Candidate = {Event.Vector, uint8_t(NMI ? 3 : 2)};
                uint64_t Budget = 4, Polls = 4;
                EXPECT_EQ(Scheduled(&State,
                                    reinterpret_cast<uint8_t *>(&Context),
                                    &Budget, &Shadow, &Polls, Producer::peek,
                                    Producer::accepted, &P),
                          7u)
                    << Context.lastError();
                EXPECT_EQ(P.Acks, Eligible ? 1u : 0u);
                EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
                EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
                ASSERT_NO_FATAL_FAILURE(
                    expectPagedSegmentCPU(State, Native, *Memory, CPU));
                const auto Actual = Memory->physicalMemory()
                                        .snapshotMemoryRegions()
                                        .front()
                                        .Bytes;
                EXPECT_TRUE(std::equal(Actual.begin(), Actual.end(), CPU.RAM));
                if (HasFailure())
                  return;
                ++Comparisons;
              }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}
#endif
