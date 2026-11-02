//===- X64PagedSegmentMoveTests.inc - Faultable selector moves ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original-CPU comparisons for selector MOV/POP and their event boundary.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, PagedSegmentLoadsMatchOriginalCPU) {
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
  Features.SMAP = true;
  struct Form {
    unsigned Segment, Base, Prefix, Length, PopBytes;
    bool Register;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (unsigned Segment : {0, 2, 3, 4, 5})
    for (unsigned Kind = 0; Kind != 12; ++Kind) {
      const unsigned Start = Code.size();
      const bool Register = Kind < 3;
      const unsigned Base = Kind == 0                ? 0
                            : Kind == 1              ? 13
                            : Kind == 2              ? 3
                            : Kind == 4 || Kind == 8 ? 4
                            : Kind == 5 || Kind == 9 ? 12
                            : Kind == 6              ? 5
                            : Kind == 7              ? 13
                                                     : 7;
      const unsigned Prefix = Kind >= 10  ? Kind - 10 + 0x64
                              : Kind >= 8 ? 0x36
                                          : 0;
      Addresses.push_back(0x1000 + Start);
      if (Prefix)
        Code.push_back(Prefix);
      if (Kind == 2)
        Code.push_back(0x66);
      if (Base >= 8)
        Code.push_back(Kind == 1 ? 0x49 : 0x41);
      Code.push_back(0x8e);
      const unsigned Mod = Register ? 3 : (Base & 7) == 5 ? 1 : 0;
      Code.push_back((Mod << 6) | (Segment << 3) | (Base & 7));
      if (!Register && (Base & 7) == 4)
        Code.push_back(0x24);
      if (Mod == 1)
        Code.push_back(0);
      const unsigned Length = Code.size() - Start;
      Forms.push_back(
          {Segment, Base, Prefix, Length, 0, Register, 0x1000 + Start});
      // A following NOP consumes MOV-SS inhibition before the native stop.
      if (Segment == 2)
        Addresses.push_back(0x1000 + Code.size());
      Code.resize(Start + 16, 0x90);
    }
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
      if (Kind == 2)
        Code.push_back(0x48);
      if (Kind == 5)
        Code.push_back(0x49);
      Code.insert(Code.end(), {0x0f, uint8_t(Segment == 4 ? 0xa1 : 0xa9)});
      Forms.push_back({Segment, 4, 0, unsigned(Code.size() - Start),
                       Kind == 1 || Kind == 4 ? 2u : 8u, false,
                       0x1000 + Start});
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
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, Addresses,
                                        Compiler, 0x1000, &Scheduled,
                                        X64NullSegmentBasePolicy::Clear));
        for (const auto &F : Forms)
          for (unsigned Case = 0; Case != 128; ++Case) {
            if ((F.Register && Case >= 119 && Case <= 124) ||
                (Case == 127 && !CPU.SMAP))
              continue;
            SCOPED_TRACE(
                llvm::formatv("{0} llvm={1} {2} PC={3:x} segment={4} case={5}",
                              Compiler, LLVM, O, F.PC, F.Segment, Case)
                    .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = F.PC;
            State.NMIBlocked = 1;
            unsigned CPL = Case < 64 ? Case >> 4 : 0;
            unsigned DPL = Case < 64 ? (Case >> 2) & 3 : 0;
            unsigned RPL = Case < 64 ? Case & 3 : 0;
            if (Case >= 96 && Case < 112) {
              CPL = (Case - 96) >> 2;
              RPL = Case & 3;
            }
            if (Case == 119 || Case == 120 || Case == 126)
              CPL = DPL = RPL = 3;
            State.Core.CS = {0, UINT32_MAX, uint16_t(24 | CPL),
                             uint16_t(0xa09b | (CPL << 5))};
            State.Core.SS = {0, UINT32_MAX, uint16_t(32 | CPL),
                             uint16_t(0xc093 | (CPL << 5))};
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, State.Core.SS.Selector,
                                   State.Core.SS.Attributes};
            Core.FSBase = 0x120;
            Core.GSBase = 0x340;
            put(0x8024, 0x1c000);
            for (unsigned V : {8, 10, 11, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            uint64_t Descriptor = 0x00cf92000000ffffULL | (uint64_t(DPL) << 45);
            if (Case >= 64 && Case < 96)
              Descriptor = (Descriptor & ~(uint64_t(15) << 40)) |
                           (uint64_t(Case & 15) << 40);
            if (Case >= 80 && Case < 96)
              Descriptor &= ~(uint64_t(1) << 47);
            uint16_t Selector = 40 | RPL;
            if (Case >= 96 && Case < 112)
              Selector = RPL;
            put(0x5028, Descriptor);
            if (Case == 112)
              Selector = 0x40;
            if (Case == 113 || Case == 114 || Case == 125) {
              Selector = 12;
              State.LDTR = {Case == 125 ? 0x7ffffffffff8ULL : 0x9000, 0x1f, 56,
                            0x82};
              if (Case != 114)
                State.Unusable &= ~64;
              put(0x9008, Descriptor);
            }
            if (Case == 115)
              put(0x5028, Descriptor & ~(uint64_t(1) << 47));
            if (Case == 116 || Case == 117) {
              put(0x13028, 0x5005);
              if (Case == 117)
                put(0x5028, Descriptor | (uint64_t(1) << 40));
            }
            if (Case == 118 || Case == 127) {
              std::memmove(RAM.Memory.front().Bytes.data() + 0x7000,
                           RAM.Memory.front().Bytes.data() + 0x6000, 4096);
              State.IDTR.Base = 0x7000;
              State.GDTR.Limit = 0x1007;
              Selector = 0x1000;
              put(0x6000, Descriptor);
              put(0x13030, Case == 127 ? 0x6007 : 0);
            }
            uint64_t Linear = Case == 122   ? 0x20fff
                              : Case == 123 ? 0x20ffe
                              : Case == 124 ? 0x800000000000ULL
                                            : 0x20000;
            if (Case == 119 || Case == 120) {
              ++Linear;
              Controls.CR0 |= 0x40000;
              Core.RFlags |= 0x40000;
            }
            if (Case == 120 || Case == 121)
              put(0x13100, 0);
            if (Case == 122 || Case == 123)
              put(0x13108, 0);
            if (Case == 126)
              put(0x13100, 0x30003);
            if (Case == 127) {
              Controls.CR4 |= 0x200000;
              Core.RFlags |= 0x40000;
              for (unsigned I = 0; I != 64; ++I)
                if (I != 6)
                  put(0x13000 + I * 8, I * 4096 | 3);
              put(0x13100, 0x30003);
            }
            if (F.Register)
              Core.GPR[F.Base] = 0xabcdef0123450000ULL | Selector;
            else {
              Core.GPR[F.Base] = Linear - (F.Prefix == 0x64   ? Core.FSBase
                                           : F.Prefix == 0x65 ? Core.GSBase
                                                              : 0);
              if (Linear < 0x30000)
                put(0x30000 + Linear - 0x20000,
                    0x9876543212340000ULL | Selector);
            }
            X64ContinuationStateV6 Native;
            bool Handler = false;
            const bool StackReload = F.Segment == 2;
            ASSERT_NO_FATAL_FAILURE(
                CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                            F.Length + unsigned(StackReload), Native, Handler,
                            false, nullptr, Controls.CR4, true, false));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(
                *Memory,
                {.AlignmentPriority = X64PagedAlignmentPriority::BeforePaging,
                 .SegmentMoveProfile =
                     X64PagedSegmentMoveProfile::SelectorFirst});
            uint64_t Budget = StackReload ? 2 : 1, Polls = Budget;
            Shadows Shadow;
            Producer Events;
            Events.Ready = 0;
            const auto Exit = Scheduled(
                &State, reinterpret_cast<uint8_t *>(&Context), &Budget, &Shadow,
                &Polls, &Producer::peek, &Producer::accepted, &Events);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Exit, Handler ? 7u : 9u);
            EXPECT_EQ(Budget, Handler && StackReload ? 1u : 0u);
            EXPECT_EQ(Shadow.SSBlocked, 0);
            expectPagedSegmentCPU(State, Native, *Memory, CPU);
            if (F.PopBytes && !Handler)
              EXPECT_EQ(Core.GPR[4], Linear + F.PopBytes);
            ++Comparisons;
            Faults += Handler;
            if (HasFailure())
              return;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("segment_load_fault_cpu_comparisons", Faults);
}

TEST_F(X64PagedExecutionTest,
       PagedSelectorReadsPreserveWidthsAndFaultingStores) {
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
    unsigned Base, Prefix, Length;
    bool Memory;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (unsigned Segment = 0; Segment != 6; ++Segment)
    for (unsigned Kind = 0; Kind != 21; ++Kind) {
      const unsigned Start = Code.size();
      const bool Memory = Kind >= 12;
      const unsigned Registers[] = {0, 3, 4, 12};
      const unsigned Bases[] = {7, 4, 12, 5, 13, 7, 4, 4, 12};
      const unsigned Base = Memory ? Bases[Kind - 12] : Registers[Kind % 4];
      const unsigned Width = Memory ? 2 : Kind < 4 ? 2 : Kind < 8 ? 4 : 8;
      const unsigned Prefix = Kind == 17   ? 0x64
                              : Kind == 18 ? 0x65
                              : Kind >= 19 ? 0x36
                                           : 0;
      Addresses.push_back(0x1000 + Start);
      if (Prefix)
        Code.push_back(Prefix);
      if (Width == 2 && !Memory)
        Code.push_back(0x66);
      const unsigned Rex = (Width == 8 ? 8 : 0) | (Base >= 8 ? 1 : 0);
      if (Rex)
        Code.push_back(0x40 | Rex);
      Code.push_back(0x8c);
      const unsigned Mod = !Memory ? 3 : (Base & 7) == 5 ? 1 : 0;
      Code.push_back((Mod << 6) | (Segment << 3) | (Base & 7));
      if (Memory && (Base & 7) == 4)
        Code.push_back(0x24);
      if (Mod == 1)
        Code.push_back(0);
      Forms.push_back({Base, Prefix, unsigned(Code.size() - Start), Memory,
                       0x1000 + Start});
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
          for (unsigned Case = 0; Case != (F.Memory ? 13u : 4u); ++Case) {
            SCOPED_TRACE(llvm::formatv("{0} llvm={1} {2} PC={3:x} case={4}",
                                       Compiler, LLVM, O, F.PC, Case)
                             .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = F.PC;
            const unsigned CPL = Case < 4 ? Case : Case >= 9 ? 3 : 0;
            const uint16_t Data = 0xc093 | (CPL << 5);
            State.Core.CS = {0, UINT32_MAX, uint16_t(24 | CPL),
                             uint16_t(0xa09b | (CPL << 5))};
            State.Core.SS = {0, UINT32_MAX, uint16_t(32 | CPL), Data};
            State.DS = {0, UINT32_MAX, uint16_t(0x50 | CPL), Data};
            State.ES = {0, UINT32_MAX, uint16_t(0x48 | CPL), Data};
            State.FS = {UINT32_MAX, uint16_t(0x58 | CPL), Data};
            State.GS = {UINT32_MAX, uint16_t(0x60 | CPL), Data};
            Core.FSBase = 0x120;
            Core.GSBase = 0x340;
            put(0x8024, 0x1c000);
            for (unsigned V : {8, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            if (F.Memory) {
              uint64_t Linear = Case == 6   ? 0x800000000000ULL
                                : Case == 7 ? 0x20ffe
                                : Case == 8 ? 0x20fff
                                            : 0x20000;
              if (Case == 9 || Case == 10 || Case == 12) {
                ++Linear;
                Controls.CR0 |= 0x40000;
                Core.RFlags |= 0x40000;
              }
              Core.GPR[F.Base] = Linear - (F.Prefix == 0x64   ? Core.FSBase
                                           : F.Prefix == 0x65 ? Core.GSBase
                                                              : 0);
              if (Case == 4 || Case == 10)
                put(0x13100, 0);
              if (Case == 5 || Case == 12)
                put(0x13100, 0x30005);
              if (Case == 7 || Case == 8)
                put(0x13108, 0);
              if (Case == 11)
                put(0x13100, 0x30003);
            } else {
              Core.GPR[F.Base] = 0xabcdef0123456789ULL;
            }
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
            const auto Exit =
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Exit, Handler ? 7u : 9u);
            EXPECT_EQ(Budget, 0u);
            expectPagedSegmentCPU(State, Native, *Memory, CPU);
            ++Comparisons;
            Faults += Handler;
            if (HasFailure())
              return;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("segment_read_fault_cpu_comparisons", Faults);
}

TEST_F(X64PagedExecutionTest, PagedMovSSOwnsItsOriginalInhibitionBoundary) {
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
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"})
        for (unsigned Program = 0; Program != 4; ++Program) {
          std::vector<uint8_t> Code{0x8e, uint8_t(Program == 2 ? 0xd8 : 0xd0)};
          if (Program == 1)
            Code.insert(Code.end(), {0x48, 0x89, 0xdc}); // MOV RSP,RBX.
          else if (Program == 3)
            Code.insert(Code.end(), {0x8e, 0xd0}); // A second MOV SS.
          else
            Code.push_back(0x90);
          const unsigned End = Code.size();
          Code.push_back(0x90); // Selected boundary for the deferred event.
          Run Driver = nullptr;
          Schedule Scheduled = nullptr;
          ASSERT_NO_FATAL_FAILURE(compile(
              Code, LLVM, O, Driver, {0x17fe, 0x1800, 0x17fe + End}, Compiler,
              0x17fe, &Scheduled, X64NullSegmentBasePolicy::Clear));
          for (unsigned Case = 0; Case != (Program ? 1u : 9u); ++Case) {
            SCOPED_TRACE(llvm::formatv("{0} llvm={1} {2} program={3} case={4}",
                                       Compiler, LLVM, O, Program, Case)
                             .str());
            reset(Code);
            std::copy(Code.begin(), Code.end(),
                      RAM.Memory.front().Bytes.begin() + 0x17fe);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = 0x17fe;
            Core.RFlags = 0x202;
            Core.GPR[0] = Case == 5 ? 40 : Case == 6 ? 0 : 16;
            Core.GPR[3] = 0x1d000;
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
            const bool OldSS = Case == 1 || (Case >= 3 && Case <= 5);
            const bool OldSTI = Case == 2 || Case >= 7;
            const bool Pending = Case == 3 || Case == 4 || Case >= 7;
            const bool IRQ = Case == 4 || Case == 8;
            X64ContinuationEventV1 Event{};
            Event.STIBlocked = OldSTI;
            Event.SSBlocked = OldSS;
            Event.NMIBlockedBySTI = 1;
            Event.Kind = IRQ ? X64ContinuationEventKind::Interrupt
                             : X64ContinuationEventKind::NMI;
            Event.Vector = IRQ ? 65 : 2;
            X64ContinuationStateV6 Native;
            bool Handler = false;
            const unsigned Count = OldSS || Program == 2 ? 1 : 2;
            const unsigned NativeLength = Count == 1 ? 2 : End;
            ASSERT_NO_FATAL_FAILURE(
                CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                            NativeLength, Native, Handler, Pending, &Event,
                            Controls.CR4, true, false, Pending));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(
                *Memory, {.SegmentMoveProfile =
                              X64PagedSegmentMoveProfile::SelectorFirst});
            Shadows Shadow;
            Shadow.STIBlocked = OldSTI;
            Shadow.SSBlocked = OldSS;
            Shadow.NMIBlockedBySTI = 1;
            Producer Events;
            Events.Ready = Pending;
            Events.Candidate.Kind = IRQ ? 2 : 3;
            Events.Candidate.Vector = IRQ ? 65 : 2;
            uint64_t Budget = Count + unsigned(Pending), Polls = Budget + 1;
            if (Program == 0 && Case == 0) {
              uint64_t FirstBudget = 1;
              EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                                  &FirstBudget, &Shadow, &Polls, Producer::peek,
                                  Producer::accepted, &Events),
                        9u);
              EXPECT_EQ(FirstBudget, 0u);
              EXPECT_EQ(Shadow.SSBlocked, 1);
              EXPECT_EQ(Core.PC, 0x1800u);
              --Budget;
            }
            const auto Exit = Scheduled(
                &State, reinterpret_cast<uint8_t *>(&Context), &Budget, &Shadow,
                &Polls, &Producer::peek, &Producer::accepted, &Events);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Exit, Handler ? 7u : 9u);
            EXPECT_EQ(Events.Acks, unsigned(Pending));
            EXPECT_EQ(Shadow.SSBlocked, 0);
            EXPECT_EQ(CPU.InterruptShadow, 0);
            expectPagedSegmentCPU(State, Native, *Memory, CPU);
            ++Comparisons;
            if (HasFailure())
              return;
          }
        }
  RecordProperty("cpu_comparisons", Comparisons);
}
#endif
