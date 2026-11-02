//===- X64PagedSegmentMoveBoundaryTests.inc - Selector restart ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Address formation, admission, publication and original-PC selector retry.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, PagedSegmentMovesUseOldBasesAndWrappedAddresses) {
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
    unsigned Prefix, Kind, Length;
    bool StackReload;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (bool Read : {false, true})
    for (unsigned Segment = 0; Segment != 6; ++Segment) {
      if (!Read && Segment == 1)
        continue;
      for (unsigned Prefix : {0x64, 0x65})
        for (unsigned Kind = 0; Kind != 4; ++Kind) {
          const unsigned Start = Code.size();
          Addresses.push_back(0x1000 + Start);
          Code.insert(Code.end(), {uint8_t(Prefix), 0x67});
          if (Kind == 3)
            Code.push_back(0x43);
          Code.push_back(Read ? 0x8c : 0x8e);
          Code.push_back((Segment << 3) | (Kind == 0   ? 7
                                           : Kind == 1 ? 5
                                           : Kind == 2 ? 4
                                                       : 0x44));
          if (Kind == 1) {
            const uint32_t Disp = -2 - (0x1000 + Code.size() + 4);
            for (unsigned I = 0; I != 4; ++I)
              Code.push_back(Disp >> (I * 8));
          } else if (Kind == 2) {
            Code.push_back(0x24);
          } else if (Kind == 3) {
            Code.insert(Code.end(), {0xa5, 0xfe}); // R13+R12*4-2.
          }
          const bool StackReload = !Read && Segment == 2;
          Forms.push_back({Prefix, Kind, unsigned(Code.size() - Start),
                           StackReload, 0x1000 + Start});
          if (StackReload)
            Addresses.push_back(0x1000 + Code.size());
          Code.resize(Start + 16, 0x90);
        }
    }
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  unsigned Comparisons = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, Addresses,
                                        Compiler, 0x1000, &Scheduled,
                                        X64NullSegmentBasePolicy::Clear));
        for (const auto &F : Forms)
          for (unsigned Case = 0; Case != 4; ++Case) {
            SCOPED_TRACE(llvm::formatv("{0} llvm={1} {2} PC={3:x} case={4}",
                                       Compiler, LLVM, O, F.PC, Case)
                             .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = F.PC;
            const auto OldBase = Case == 3 ? 0x7fff00000002ULL : 0x20002ULL;
            Core.FSBase = F.Prefix == 0x64 ? OldBase : 0x12345678;
            Core.GSBase = F.Prefix == 0x65 ? OldBase : 0x12345678;
            Core.GPR[F.Kind == 2 ? 4 : 7] = 0xabcdef01fffffffeULL;
            Core.GPR[13] = 0xabcdef01fffffff8ULL;
            Core.GPR[12] = 2;
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
            put(0x8024, 0x1c000);
            for (unsigned V : {8, 10, 11, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            put(0x11020, 0x14007);
            put(0x14000, 0x15007);
            put(0x15100, Case == 2 ? 0 : 0x31007);
            put(0x5028, 0x00cf92567800ffffULL);
            put(0x31000, Case == 1 ? 0 : 40);
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(
                CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                            F.Length + unsigned(F.StackReload), Native, Handler,
                            false, nullptr, Controls.CR4, true, false));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(
                *Memory, {.SegmentMoveProfile =
                              X64PagedSegmentMoveProfile::SelectorFirst});
            uint64_t Budget = F.StackReload ? 2 : 1, Polls = Budget;
            Shadows Shadow;
            Producer Events;
            Events.Ready = 0;
            EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                                &Budget, &Shadow, &Polls, Producer::peek,
                                Producer::accepted, &Events),
                      Handler ? 7u : 9u);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            expectPagedSegmentCPU(State, Native, *Memory, CPU, 0x16000);
            ++Comparisons;
            if (HasFailure())
              return;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionTest, PagedSegmentMoveAdmissionAndPublication) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Checks = 0;
  for (unsigned Kind = 0; Kind != 4; ++Kind)
    for (bool LLVM : {false, true})
      for (const auto Policy :
           {std::optional<X64NullSegmentBasePolicy>{},
            std::optional(X64NullSegmentBasePolicy::Preserve)}) {
        const std::vector<uint8_t> Code =
            Kind == 0   ? std::vector<uint8_t>{0x8e, 0x27}
            : Kind == 1 ? std::vector<uint8_t>{0x0f, 0xa1}
            : Kind == 2 ? std::vector<uint8_t>{0x8c, 0x1f}
                        : std::vector<uint8_t>{0x8e, 0x17};
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                        NEVERD_TEST_CLANG, 0x1000, &Scheduled,
                                        Policy));
        for (unsigned Case = 0; Case != 11; ++Case) {
          SCOPED_TRACE(llvm::formatv("kind={0} llvm={1} preserve={2} case={3}",
                                     Kind, LLVM, bool(Policy), Case)
                           .str());
          if (Kind == 2 && (Case == 4 || Case >= 5 && Case <= 9))
            continue;
          reset(Code);
          auto &Core = State.Core.Core.Core.Core;
          Core.FSBase = 0x12345678;
          if (Kind == 1)
            Core.GPR[4] = 0x20000;
          State.DS = State.Core.SS;
          put(0x5028, 0x00cf92000000ffffULL);
          put(0x30000, Case >= 7 && Case <= 9 ? 0 : 40);
          X64PagedExecutionOptions Options;
          if (Case)
            Options.SegmentMoveProfile =
                X64PagedSegmentMoveProfile::SelectorFirst;
          if (Case == 1)
            RAM.Memory.front().Bytes[0x1000] = 0x90;
          if (Case == 2)
            put(0x13008, 0);
          if (Case == 3)
            put(0x13100, 0x50007);
          if (Case == 4)
            put(0x13028, 0x50007);
          if (Case == 5 || Case == 6) {
            put(0x30000, 0x40);
            if (Case == 5)
              put(0x13030, 0x50007);
            else
              State.IDTR.Limit = 0;
          }
          if (Case == 8)
            put(0x13028, 0x50007);
          if (Case == 9)
            put(0x13100, 0);
          if (Case == 10) {
            put(0x10000, 0x11027);
            put(0x11000, 0x12027);
            put(0x12000, 0x13027);
            for (unsigned I = 0; I != 64; ++I)
              put(0x13000 + I * 8, I * 4096 | 0x67);
            put(0x13100, 0x30067);
            RAM.Memory.front().Permissions = MemoryPermission::Read;
          }
          const auto Old = State;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(*Memory, Options);
          if (Kind == 3) {
            uint64_t UnscheduledBudget = 1;
            EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context),
                             &UnscheduledBudget),
                      5u);
            EXPECT_EQ(UnscheduledBudget, 1u);
            EXPECT_EQ(std::memcmp(&State, &Old, sizeof(State)), 0);
            EXPECT_EQ(
                Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
                RAM.Memory.front().Bytes);
          }
          Shadows Shadow;
          Shadow.SSBlocked = 1;
          Producer Events;
          Events.Ready = 0;
          uint64_t Budget = 1, Polls = 1;
          const bool NullRefused =
              Case >= 7 && Case <= 8 && Kind < 2 && !Policy;
          const bool Attempt = Case == 2 || (Case >= 5 && Case <= 9);
          const unsigned Expected = Case == 0 || NullRefused ? 5
                                    : Case == 1              ? 20
                                    : Case == 2 || Case == 9 ? 7
                                    : Case == 6              ? 19
                                    : Case >= 7 && Case <= 8 ? 9
                                                             : 18;
          EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                              &Budget, &Shadow, &Polls, Producer::peek,
                              Producer::accepted, &Events),
                    Expected);
          EXPECT_EQ(Budget, Attempt && !NullRefused ? 0u : 1u);
          EXPECT_EQ(Shadow.SSBlocked, Attempt && !NullRefused ? 0u : 1u);
          if (Case == 0)
            EXPECT_NE(Context.lastError().find("bound access order"),
                      std::string::npos);
          else
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          if (!Attempt || NullRefused)
            EXPECT_EQ(std::memcmp(&State, &Old, sizeof(State)), 0);
          if (Case == 10) {
            ASSERT_TRUE(Context.lastResult().BackingFault);
            EXPECT_EQ(Context.lastResult().BackingFault->Exit.Address,
                      Kind == 2 ? 0x30000u : 0x5028u);
            EXPECT_EQ(Context.lastResult().BackingFault->AccessSize,
                      Kind == 2 ? 2u : 8u);
          }
          if (Expected == 9) {
            EXPECT_EQ(Core.PC, 0x1000 + Code.size());
            EXPECT_EQ(Core.GPR[4],
                      Old.Core.Core.Core.Core.GPR[4] + (Kind == 1 ? 8 : 0));
            EXPECT_EQ(Core.FSBase, Old.Core.Core.Core.Core.FSBase);
            EXPECT_EQ(Kind == 3 ? State.Core.SS.Selector : State.FS.Selector,
                      0);
            EXPECT_TRUE(State.Unusable & (Kind == 3 ? 2 : 16));
          }
          ++Checks;
        }
      }
  RecordProperty("policy_backing_checks", Checks);
}

TEST_F(X64PagedExecutionTest,
       PagedSelectorStoreDoesNotPublishBeforeAlignmentFault) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x8c, 0x1f};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
    for (auto Priority : {X64PagedAlignmentPriority::BeforePaging,
                          X64PagedAlignmentPriority::AfterPaging})
      for (bool Present : {false, true}) {
        reset(Code);
        auto &Core = State.Core.Core.Core.Core;
        Core.GPR[7] = 0x20001;
        Core.RFlags |= 0x40000;
        Controls.CR0 |= 0x40000;
        State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
        State.Core.SS = State.DS = {0, UINT32_MAX, 35, 0xc0f3};
        State.Core.Core.CR2 = 0x1234;
        if (!Present)
          put(0x13100, 0);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Context(
            *Memory,
            {.AlignmentPriority = Priority,
             .SegmentMoveProfile = X64PagedSegmentMoveProfile::SelectorFirst});
        uint64_t Budget = 1;
        EXPECT_EQ(
            Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget), 7u);
        EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
        EXPECT_EQ(Budget, 0u);
        const bool AC =
            Present || Priority == X64PagedAlignmentPriority::BeforePaging;
        EXPECT_EQ(Core.PC, AC ? 0x4110u : 0x40e0u);
        EXPECT_EQ(State.Core.Core.CR2, AC ? 0x1234u : 0x20001u);
        EXPECT_EQ(Memory->physicalMemory()
                      .loadScalar(0x30000, GuestScalarWidth::I64)
                      .Value,
                  0x8877665544332211ULL);
      }
  }
}

TEST_F(X64PagedExecutionTest, PagedSegmentMoveRejectsUnauthenticatedForms) {
  reset({0x90});
  const auto Before = State;
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  for (const auto &Code :
       std::vector<std::vector<uint8_t>>{{},
                                         {0x8e},
                                         {0x8e, 0xc8},
                                         {0x8e, 0xf0},
                                         {0x8c, 0xf8},
                                         {0x8e, 0x04},
                                         {0x8e, 0x05, 0, 0},
                                         {0x8e, 0xc0, 0x90},
                                         {0xf0, 0x8e, 0x07},
                                         {0xf3, 0x8e, 0xc0},
                                         {0x66, 0x66, 0x8e, 0xc0},
                                         {0x48, 0x66, 0x8e, 0xc0},
                                         {0x64, 0x65, 0x0f, 0xa1},
                                         {0x67, 0x67, 0x0f, 0xa9},
                                         {0x0f, 0xa1, 0x90},
                                         {0x17},
                                         {0x1f},
                                         {0x07}}) {
    auto R = moveX64PagedSegment(State, *Memory, Code,
                                 X64PagedAlignmentPriority::BeforePaging,
                                 X64PagedSegmentMoveProfile::SelectorFirst,
                                 X64NullSegmentBasePolicy::Clear);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
    EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
              RAM.Memory.front().Bytes);
  }
  X64ContinuationSourceOptions Options;
  Options.Profile = X64ContinuationProfile::KernelEventsNoFaultV6;
  Options.MXCSRMask = 0xffff;
  Options.PagingAddressBits = 48;
  Options.PagedMemory = true;
  const auto Image = image({0x8e, 0xd0});
  auto Units = buildX64ContinuationSource(Image, {0x1000}, Options);
  ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
  EXPECT_TRUE(Units->front().RequiresNativeExecution);
  Options.ScheduledEvents = true;
  Units = buildX64ContinuationSource(Image, {0x1000}, Options);
  ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
  EXPECT_FALSE(Units->front().RequiresNativeExecution);
  EXPECT_TRUE(Units->front().IsMovSS);
}

TEST_F(X64PagedExecutionTest, PagedMovSSKeepsTheUnboundNativeRequest) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x8e, 0xd0};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                    NEVERD_TEST_CLANG, 0x1000, nullptr,
                                    std::nullopt, true));
    reset(Code);
    const auto Before = State;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory);
    uint64_t Budget = 1;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              uint64_t(X64ContinuationExitV1::NativeInstruction));
    EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
    EXPECT_EQ(Budget, 1u);
    EXPECT_FALSE(Context.lastResult().InstructionAttempted);
    EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
    const auto After = Memory->physicalMemory().snapshotMemoryRegions();
    // Execute-page A bits may change; ordinary RAM and operand backing cannot.
    EXPECT_EQ(std::memcmp(After.front().Bytes.data(),
                          RAM.Memory.front().Bytes.data(), 0x10000),
              0);
    EXPECT_EQ(std::memcmp(After.front().Bytes.data() + 0x14000,
                          RAM.Memory.front().Bytes.data() + 0x14000, 0x2c000),
              0);
  }
}

TEST_F(X64PagedExecutionTest, PagedSegmentMoveHandlerRepairsMappingAndRetries) {
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
  unsigned Comparisons = 0;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  for (unsigned Kind = 0; Kind != 5; ++Kind)
    for (const auto &Compiler : Compilers)
      for (bool LLVM : {false, true}) {
        std::vector<uint8_t> Code(0x1012, 0x90);
        Code[0] = Kind >= 3 ? 0x0f : Kind == 0 ? 0x8c : 0x8e;
        Code[1] = Kind == 0   ? 0x1f
                  : Kind == 1 ? 0x17
                  : Kind == 2 ? 0x27
                  : Kind == 3 ? 0xa1
                              : 0xa9;
        Code[0x1010] = 0x48;
        Code[0x1011] = 0xcf;
        const uint8_t HandlerCode[] = {0x48, 0xc7, 0x04, 0x25, 0x00, 0x31,
                                       0x01, 0x00, 0x07, 0x00, 0x03, 0x00,
                                       0x48, 0x83, 0xc4, 0x08};
        std::copy(std::begin(HandlerCode), std::end(HandlerCode),
                  Code.begin() + 0x1000);
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(
            Code, LLVM, "-O2", Driver, {0x1000, 0x1002, 0x2000, 0x200c, 0x2010},
            Compiler, 0x1000, &Scheduled));
        reset(Code);
        State.NMIBlocked = 1;
        State.DS = State.ES = State.Core.SS;
        State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
        if (Kind >= 3)
          State.Core.Core.Core.Core.GPR[4] = 0x20000;
        put(0x30000, 40);
        put(0x5028, 0x00cf92000000ffffULL);
        put(0x13100, 0);
        put(0x8024, 0x1c000);
        put(0x60e0, 0x2000 | (uint64_t(8) << 16) | (uint64_t(1) << 32) |
                        (uint64_t(0x8e) << 40));
        X64ContinuationStateV6 Native;
        bool Handler = false;
        const unsigned Length = Kind == 1 ? 3 : 2;
        ASSERT_NO_FATAL_FAILURE(CPU.execute(
            RAM.Memory.front().Bytes, State, Controls.CR0, Length, Native,
            Handler, false, nullptr, Controls.CR4, true, false));
        ASSERT_FALSE(Handler);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Context(
            *Memory,
            {.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst,
             .SegmentMoveProfile = X64PagedSegmentMoveProfile::SelectorFirst});
        Shadows Shadow;
        Producer Events;
        Events.Ready = 0;
        for (unsigned Phase = 0; Phase != 3; ++Phase) {
          uint64_t Budget = Phase == 1 ? 3 : Phase == 2 && Kind == 1 ? 2 : 1;
          uint64_t Polls = Budget;
          EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                              &Budget, &Shadow, &Polls, Producer::peek,
                              Producer::accepted, &Events),
                    Phase == 0   ? 7u
                    : Phase == 1 ? 6u
                                 : 9u);
          EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          EXPECT_EQ(Budget, 0u);
          EXPECT_EQ(State.NMIBlocked, Phase == 0 ? 1 : 0);
          EXPECT_EQ(State.Core.Core.Core.Core.PC, Phase == 0 ? 0x2000u
                                                  : Phase == 1
                                                      ? 0x1000u
                                                      : 0x1000u + Length);
          if (Phase == 1 && Kind >= 3)
            EXPECT_EQ(State.Core.Core.Core.Core.GPR[4], 0x20000u);
        }
        expectPagedSegmentCPU(State, Native, *Memory, CPU);
        ++Comparisons;
        if (HasFailure())
          return;
      }
  RecordProperty("cpu_comparisons", Comparisons);
}
#endif
