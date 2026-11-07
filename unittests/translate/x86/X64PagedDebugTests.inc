//===- X64PagedDebugTests.inc - Faultable instruction debug ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included after the paged fixture. Real #DB handlers observe original bytes
/// with KVM guest debugging disabled, independently of both generated routes.
//===----------------------------------------------------------------------===//

class X64PagedDebugTest : public X64PagedExecutionTest {
protected:
  void compileDebug(const std::vector<uint8_t> &Code, bool LLVM, Run &Driver,
                    Schedule &Scheduled,
                    const std::vector<uint64_t> &Addresses = {0x1000},
                    const std::string &Compiler = NEVERD_TEST_CLANG,
                    const std::string &Optimization = "-O2") {
    compile(Code, LLVM, Optimization, Driver, Addresses, Compiler, 0x1000,
            &Scheduled, X64NullSegmentBasePolicy::Clear, false, true);
  }

  void resetDebug(const std::vector<uint8_t> &Code, unsigned CPL = 0) {
    reset(Code);
    if (CPL) {
      const unsigned CodeSelector = CPL == 3 ? 24 : CPL == 1 ? 40 : 56;
      const unsigned StackSelector = CodeSelector + 8;
      State.Core.CS = {0, UINT32_MAX, uint16_t(CodeSelector | CPL),
                       uint16_t(0xa09b | (CPL << 5))};
      State.Core.SS = {0, UINT32_MAX, uint16_t(StackSelector | CPL),
                       uint16_t(0xc093 | (CPL << 5))};
      put(0x5000 + CodeSelector, 0x00af9b000000ffffULL | (uint64_t(CPL) << 45));
      put(0x5000 + StackSelector,
          0x00cf93000000ffffULL | (uint64_t(CPL) << 45));
      State.GDTR.Limit = 0x5f;
      State.TR.Selector = 0x50;
      State.Core.Core.Core.Core.GPR[4] = 0x1d800;
    }
    State.DS = State.ES = State.Core.SS;
    State.FS = State.GS = {State.Core.SS.Limit, State.Core.SS.Selector,
                           State.Core.SS.Attributes};
    State.Core.Core.Core.Core.RFlags = 0x302;
    // Preseed page A/D bits, so the oracle's handler HLT fetch adds no state
    // absent from the runtime's stop-at-handler-entry contract. All RAM,
    // including these page tables and debug frames, is compared below.
    put(0x10000, 0x11027);
    put(0x11000, 0x12027);
    put(0x12000, 0x13027);
    for (unsigned I = 0; I != 64; ++I)
      put(0x13000 + I * 8, I * 4096 | 0x67);
    put(0x13100, 0x30067);
    put(0x13108, 0x31067);
  }

  X64PagedExecutionOptions options(X64PagedDebugState &Debug) {
    X64PagedExecutionOptions O;
    O.Debug = &Debug;
    O.AlignmentPriority = X64PagedAlignmentPriority::BeforePaging;
    O.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
    O.FarReturnProfile = X64PagedFarReturnProfile::SelectorFirst;
    O.FarPointerProfile = X64PagedFarPointerProfile::OffsetFirst;
    O.SegmentMoveProfile = X64PagedSegmentMoveProfile::SelectorFirst;
    O.RepeatProfile = X64PagedRepeatProfile::ClearLodsCountHigh;
    return O;
  }
};

TEST_F(X64PagedDebugTest, BindingAdmissionPrecedesPollingAndFetch) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug({0x90}, LLVM, Driver, Scheduled));
    for (unsigned Case = 0; Case != 8; ++Case) {
      SCOPED_TRACE(Case);
      resetDebug({0x90});
      X64PagedDebugState Debug;
      auto O = options(Debug);
      switch (Case) {
      case 0:
        O.Debug = nullptr;
        break;
      case 1:
        Debug.DR7 |= 1;
        break;
      case 2:
        Debug.DR7 |= 0x2000;
        break;
      case 3:
        Debug.DebugCTL = 2;
        break;
      case 4:
        Debug.DR6 &= ~0x10;
        break;
      case 5:
        Debug.DR6 |= 0x1000;
        break;
      case 6:
        Debug.DR6 |= uint64_t{1} << 32;
        break;
      case 7:
        Debug.DR7 |= uint64_t{1} << 32;
        break;
      }
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
                5u);
      EXPECT_EQ(Budget, 1u);
      EXPECT_EQ(Polls, 1u);
      EXPECT_EQ(P.Polls, 0u);
      EXPECT_FALSE(Context.lastError().empty());
      EXPECT_EQ(std::memcmp(&Before, &State, sizeof(State)), 0);
      EXPECT_EQ(std::memcmp(&BeforeDebug, &Debug, sizeof(Debug)), 0);
      EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
                RAM.Memory.front().Bytes);
    }
  }
}

TEST_F(X64PagedDebugTest, CompletedTrapRetainsStateAcrossBackingStop) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0xff, 0x07}; // INC [rdi].
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled));
    resetDebug(Code);
    put(0x13030, 0x50067); // IDT page has no physical backing.
    X64PagedDebugState Debug;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory, options(Debug));
    uint64_t Budget = 1;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u);
    EXPECT_EQ(Budget, 0u);
    EXPECT_TRUE(Context.lastResult().InstructionCompleted);
    ASSERT_TRUE(Context.lastResult().Delivery);
    EXPECT_TRUE(Context.lastResult().Delivery->InputAccepted);
    EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x1003u);
    EXPECT_EQ(Debug.DR6, 0xffff4ff0u);
    EXPECT_EQ(Memory->physicalMemory()
                  .loadScalar(0x30000, GuestScalarWidth::I64)
                  .Value,
              0x8877665544332212ULL);
    auto Stored = Memory->physicalMemory().storeScalar(
        0x13030, GuestScalarWidth::I64, 0x6067);
    ASSERT_EQ(Stored.Status, GuestMemoryAccessStatus::Completed);
    ASSERT_TRUE(Context.lastResult().Delivery->Continuation);
    auto R = Context.resume(State, *Context.lastResult().Delivery->Continuation,
                            0xffff);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Exit, 7u);
    EXPECT_FALSE(R->InstructionAttempted);
    EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x4010u);
    EXPECT_EQ(Debug.DR6, 0xffff4ff0u);
    EXPECT_EQ(Memory->physicalMemory()
                  .loadScalar(0x30000, GuestScalarWidth::I64)
                  .Value,
              0x8877665544332212ULL);
  }
}

TEST_F(X64PagedDebugTest, UnexecutedStepsAndExternalEventsRetainDebugStatus) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0x8b, 0x07};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled));
    for (unsigned Case = 0; Case != 6; ++Case) {
      SCOPED_TRACE(Case);
      resetDebug(Code);
      X64PagedDebugState Debug{0xffffefff};
      const auto Before = State;
      if (Case == 0)
        RAM.Memory.front().Bytes[0x1000] = 0x90;
      if (Case == 1)
        put(0x13008, 0); // Guest fetch fault.
      if (Case == 2)
        put(0x13100, 0x50067); // Operand backing stop.
      if (Case == 3)
        put(0x13100, 0); // Guest operand fault.
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, options(Debug));
      Shadows Shadow;
      Producer P;
      P.Ready = Case >= 4;
      if (Case == 5)
        P.Candidate = {2, 3}; // NMI wins before the instruction starts.
      uint64_t Budget = 1, Polls = 1;
      EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                          &Budget, &Shadow, &Polls, Producer::peek,
                          Producer::accepted, &P),
                Case == 0   ? 20u
                : Case == 2 ? 18u
                            : 7u);
      EXPECT_EQ(Debug.DR6, 0xffffefffu);
      EXPECT_FALSE(Context.lastResult().InstructionCompleted);
      EXPECT_EQ(Budget, Case == 1 || Case == 3 ? 0u : 1u);
      EXPECT_EQ(P.Acks, Case >= 4 ? 1u : 0u);
      if (Case == 0 || Case == 2)
        EXPECT_EQ(std::memcmp(&Before, &State, sizeof(State)), 0);
    }
  }
}

TEST_F(X64PagedDebugTest, SourceAdmissionKeepsOtherExecutionContractsExplicit) {
  for (bool LLVM : {false, true}) {
    auto Export = exportX64Continuation(
        image({0x90}),
        R"({"profile":"kernel-events-v6","mxcsr_mask":65535,
            "paging_address_bits":48,"paged_debug":true,
            "paged_memory":false,"instructions":[4096]})",
        LLVM);
    ASSERT_FALSE(bool(Export));
    EXPECT_NE(llvm::toString(Export.takeError()).find("paged_debug"),
              std::string::npos);
    Export = exportX64Continuation(
        image({0xc3}),
        R"({"profile":"kernel-events-v6","mxcsr_mask":65535,
            "paging_address_bits":48,"paged_debug":true,"paged_memory":true,
            "entries":[4096],"follow_internal_calls":true,
            "function_context":true})",
        LLVM);
    ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
    const auto Manifest = llvm::cantFail(llvm::json::parse(Export->Manifest));
    const auto *Function =
        Manifest.getAsObject()->getObject("function_context");
    ASSERT_NE(Function, nullptr);
    EXPECT_EQ(Function->getString("context_type"),
              "neverd_x64_function_context_v2");
    EXPECT_EQ(Function->getString("transfer_receipt"),
              "neverd_x64_paged_transfer_v1");
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedDebugTest, OriginalHandlersReturnAndRepairBeforeSingleStep) {
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
        Case == 0   ? std::vector<uint8_t>{0x90}
        : Case == 1 ? std::vector<uint8_t>{0xf3, 0xa4}
                    : std::vector<uint8_t>{0x48, 0x8b, 0x07};
    std::copy(Entry.begin(), Entry.end(), Code.begin());
    Code[Entry.size()] = 0xcc;
    const std::vector<uint8_t> Count{0x48, 0xff, 0x04, 0x25, 0, 0x20, 2, 0};
    std::copy(Count.begin(), Count.end(), Code.begin() + 0x3010);
    std::vector<uint64_t> PCs{0x1000, 0x1000 + Entry.size(), 0x4010, 0x4018};
    unsigned End = 0x3018;
    if (Case == 0) {
      const std::vector<uint8_t> ClearTF{0x48, 0x81, 0x64, 0x24, 0x10,
                                         0xff, 0xfe, 0xff, 0xff};
      std::copy(ClearTF.begin(), ClearTF.end(), Code.begin() + End);
      End += ClearTF.size();
      PCs.push_back(0x1000 + End);
    }
    Code[End] = 0x48;
    Code[End + 1] = 0xcf;
    if (Case == 2) {
      const std::vector<uint8_t> Repair{0x48, 0xc7, 0x04, 0x25, 0x00, 0x31,
                                        0x01, 0,    0x67, 0x00, 0x03, 0,
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
            for (bool RF : {false, true}) {
              SCOPED_TRACE(Case);
              SCOPED_TRACE(CPL);
              SCOPED_TRACE(RF);
              SCOPED_TRACE(Compiler + Optimization +
                           (LLVM ? "-llvm" : "-high"));
              resetDebug(Code, CPL);
              auto &Core = State.Core.Core.Core.Core;
              if (RF)
                Core.RFlags |= 0x10000;
              Core.GPR[1] = 3;
              Core.GPR[6] = 0x21000;
              if (Case == 2)
                put(0x13100, 0);
              RAM.Memory.front().Bytes[0x6035] |= 0x60;
              X64PagedDebugState Debug;
              auto O = options(Debug);
              if (CPU.ReturnCoreType == 0x20)
                O.RepeatProfile = X64PagedRepeatProfile::PreserveLodsCountHigh;
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              X64PagedExecution Context(*Memory, O);
              X64ContinuationStateV6 Native;
              bool Fault = false;
              CPU.DebugRegisters.emplace();
              CPU.DebugRegisters->dr6 = Debug.DR6;
              CPU.DebugRegisters->dr7 = Debug.DR7;
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
                    << Exit << " " << Context.lastError();
                Stops += Exit == 7;
              }
              EXPECT_EQ(Stops, Case == 0 ? 2u : Case == 1 ? 4u : 3u);
              EXPECT_EQ(Memory->physicalMemory()
                            .loadScalar(0x22000, GuestScalarWidth::I64)
                            .Value,
                        Case == 1 ? 3u : 1u);
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

TEST_F(X64PagedDebugTest, OriginalCPUInstructionAndFaultMatrix) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned Kind = 0;
    std::vector<unsigned> ExtraPCs;
  };
  const Case Cases[] = {
      {"nop", {0x90}},
      {"increment", {0x48, 0xff, 0xc0}},
      {"store", {0x48, 0x89, 0x07}},
      {"rmw", {0x48, 0xff, 0x07}},
      {"load-fault", {0x48, 0x8b, 0x07}, 1},
      {"store-fault", {0x48, 0x89, 0x07}, 1},
      {"ud2", {0x0f, 0x0b}},
      {"divide-fault", {0x48, 0xf7, 0xf1}, 2},
      {"icebp", {0xf1}},
      {"int1", {0xcd, 1}, 3},
      {"int3", {0xcc}, 3},
      {"int3-imm", {0xcd, 3}, 3},
      {"popfq-clear", {0x9d}, 4},
      {"popfq-set", {0x9d, 0x90}, 5, {1}},
      {"pushfq", {0x9c}},
      {"movss-nop", {0x8e, 0xd0, 0x90}, 6, {2}},
      {"movss-movss", {0x8e, 0xd0, 0x8e, 0xd0, 0x90}, 6, {2, 4}},
      {"movss-icebp", {0x8e, 0xd0, 0xf1}, 6, {2}},
      {"lss", {0x48, 0x0f, 0xb2, 0x07}, 7},
      {"rep-movs", {0xf3, 0xa4}, 8},
      {"rep-cmps", {0xf3, 0xa6}, 8},
      {"rep-zero", {0xf3, 0xa4}, 9},
      {"rep-final", {0xf3, 0xa4}, 10},
      {"iret-clear", {0x48, 0xcf}, 11},
      {"iret-set", {0x48, 0xcf}, 12},
      {"retf", {0x48, 0xcb}, 13},
      {"near-ret", {0xc3}, 14},
      {"jump", {0xff, 0xe0}, 15},
      {"call", {0xff, 0xd0}, 15},
      {"cli", {0xfa}},
      {"sti", {0xfb}, 16},
      {"debug-gate-absent", {0x90}, 17},
      {"debug-stack-fault", {0x90}, 18},
  };
  std::vector<uint8_t> Code(0x802, 0x90);
  std::vector<uint64_t> PCs{0x1800};
  for (unsigned I = 0; I != std::size(Cases); ++I) {
    const auto Offset = I * 32;
    std::copy(Cases[I].Bytes.begin(), Cases[I].Bytes.end(),
              Code.begin() + Offset);
    PCs.push_back(0x1000 + Offset);
    for (auto Extra : Cases[I].ExtraPCs)
      PCs.push_back(0x1000 + Offset + Extra);
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
            for (bool RF : {false, true})
              for (uint64_t DR6 :
                   {0xffff0ff0u, 0xffffafffu, 0xffffefffu, 0xffffeff0u}) {
                const auto &C = Cases[I];
                if (C.Kind == 18 && !CPL)
                  continue;
                SCOPED_TRACE(Compiler + Optimization +
                             (LLVM ? "-llvm-" : "-high-") + C.Name);
                SCOPED_TRACE(CPL);
                SCOPED_TRACE(RF);
                SCOPED_TRACE(DR6);
                resetDebug(Code, CPL);
                auto &Core = State.Core.Core.Core.Core;
                Core.PC = 0x1000 + I * 32;
                if (RF)
                  Core.RFlags |= 0x10000;
                switch (C.Kind) {
                case 1:
                  put(0x13100, 0);
                  break;
                case 2:
                  Core.GPR[1] = 0;
                  break;
                case 3:
                  RAM.Memory.front()
                      .Bytes[0x6000 + (C.Bytes.back() == 1 ? 1 : 3) * 16 + 5] |=
                      0x60;
                  break;
                case 4:
                  put(Core.GPR[4], 0x3202);
                  break;
                case 5:
                  Core.RFlags &= ~uint64_t{0x100};
                  put(Core.GPR[4], 0x102);
                  break;
                case 6:
                  Core.GPR[0] = State.Core.SS.Selector;
                  break;
                case 7:
                  put(0x30000, 0x12345678);
                  put(0x30008, State.Core.SS.Selector);
                  break;
                case 8:
                case 9:
                case 10:
                  Core.GPR[1] = C.Kind == 8 ? 3 : C.Kind == 9 ? 0 : 1;
                  Core.GPR[6] = 0x21000;
                  put(0x31000, 0x8877665544332211ULL);
                  break;
                case 11:
                case 12:
                case 13:
                  put(Core.GPR[4], 0x1800);
                  put(Core.GPR[4] + 8, State.Core.CS.Selector);
                  if (C.Kind != 13) {
                    put(Core.GPR[4] + 16, C.Kind == 12 ? 0x10302 : 0x10202);
                    put(Core.GPR[4] + 24, 0x1c000);
                    put(Core.GPR[4] + 32, State.Core.SS.Selector);
                  }
                  if (C.Kind == 12)
                    Core.RFlags &= ~uint64_t{0x100};
                  break;
                case 14:
                  put(Core.GPR[4], 0x1800);
                  break;
                case 15:
                  Core.GPR[0] = 0x1800;
                  break;
                case 16:
                  Core.RFlags &= ~uint64_t{0x200};
                  break;
                case 17:
                  RAM.Memory.front().Bytes[0x6015] &= ~0x80;
                  break;
                case 18:
                  put(0x130e8,
                      0); // Ring-zero debug stack is absent; #PF uses IST.
                  RAM.Memory.front().Bytes[0x6000 + 14 * 16 + 4] = 1;
                  break;
                }
                X64PagedDebugState Debug{DR6};
                auto O = options(Debug);
                if (CPU.ReturnCoreType == 0x20) {
                  O.RepeatProfile =
                      X64PagedRepeatProfile::PreserveLodsCountHigh;
                  O.FarReturnProfile =
                      X64PagedFarReturnProfile::SelectorFirstTruncatedIP;
                }
                Memory = llvm::cantFail(
                    X86PagedMemory::create(RAM, Features, Controls));
                X64PagedExecution Context(*Memory, O);
                X64ContinuationStateV6 Native;
                bool Fault = false;
                CPU.DebugRegisters.emplace();
                CPU.DebugRegisters->dr6 = DR6;
                CPU.DebugRegisters->dr7 = 0x400;
                CPU.DebugRegisters->db[0] = Core.PC;
                CPU.DebugRegisters->db[1] = Core.PC + C.Bytes.size();
                ASSERT_NO_FATAL_FAILURE(
                    CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                                C.Bytes.size(), Native, Fault, true, nullptr,
                                Controls.CR4, true, false));
                ASSERT_TRUE(Fault);
                Shadows Shadow;
                Producer P;
                P.Ready = 0;
                uint64_t Budget = 5, Polls = 5;
                auto Exit = Scheduled(
                    &State, reinterpret_cast<uint8_t *>(&Context), &Budget,
                    &Shadow, &Polls, Producer::peek, Producer::accepted, &P);
                if (C.Kind == 12) {
                  ASSERT_EQ(Exit, 6u);
                  Exit = Scheduled(
                      &State, reinterpret_cast<uint8_t *>(&Context), &Budget,
                      &Shadow, &Polls, Producer::peek, Producer::accepted, &P);
                }
                EXPECT_EQ(Exit, 7u);
                EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
                ASSERT_TRUE(Context.lastResult().Delivery);
                EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
                EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
                EXPECT_EQ(Shadow.SSBlocked, bool(CPU.InterruptShadow &
                                                 KVM_X86_SHADOW_INT_MOV_SS));
                EXPECT_EQ(Shadow.STIBlocked,
                          bool(CPU.InterruptShadow & KVM_X86_SHADOW_INT_STI));
                ASSERT_NO_FATAL_FAILURE(
                    expectPagedSegmentCPU(State, Native, *Memory, CPU));
                const auto Actual = Memory->physicalMemory()
                                        .snapshotMemoryRegions()
                                        .front()
                                        .Bytes;
                if (!std::equal(Actual.begin(), Actual.end(), CPU.RAM)) {
                  const auto Difference =
                      std::mismatch(Actual.begin(), Actual.end(), CPU.RAM);
                  ADD_FAILURE() << "full RAM differs at "
                                << (Difference.first - Actual.begin());
                }
                if (HasFailure())
                  return;
                ++Comparisons;
              }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("compiler_count", Compilers.size());
  RecordProperty("core_type", CPU.ReturnCoreType);
}
#endif
