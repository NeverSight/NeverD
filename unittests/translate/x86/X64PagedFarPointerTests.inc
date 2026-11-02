//===- X64PagedFarPointerTests.inc - Faultable segment loads --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original-CPU comparisons for paged LSS/LFS/LGS through both C routes.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
void expectFarPointerCPU(const X64ContinuationStateV6 &State,
                         const X64ContinuationStateV6 &Native,
                         X86PagedMemory &Memory, const X64PagedNativeCPU &CPU,
                         unsigned PageTableEnd = 0x14000) {
  const auto &Core = State.Core.Core.Core.Core;
  EXPECT_EQ(State.NMIBlocked, Native.NMIBlocked);
  const auto &Want = Native.Core.Core.Core.Core;
  EXPECT_EQ(Core.PC, Want.PC);
  EXPECT_EQ(Core.RFlags, Want.RFlags);
  for (unsigned I = 0; I != 16; ++I)
    EXPECT_EQ(Core.GPR[I], Want.GPR[I]) << I;
  EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
  EXPECT_EQ(State.Unusable, Native.Unusable);
  const auto CheckSegment = [&](const X64ContinuationSegment &A,
                                const X64ContinuationSegment &B,
                                unsigned Unusable) {
    EXPECT_EQ(A.Selector, B.Selector);
    if (!(Native.Unusable & Unusable)) {
      EXPECT_EQ(A.Base, B.Base);
      EXPECT_EQ(A.Limit, B.Limit);
      EXPECT_EQ(A.Attributes, B.Attributes);
    }
  };
  CheckSegment(State.Core.CS, Native.Core.CS, 1);
  CheckSegment(State.Core.SS, Native.Core.SS, 2);
  CheckSegment(State.DS, Native.DS, 4);
  CheckSegment(State.ES, Native.ES, 8);
  CheckSegment(
      {Core.FSBase, State.FS.Limit, State.FS.Selector, State.FS.Attributes},
      {Want.FSBase, Native.FS.Limit, Native.FS.Selector, Native.FS.Attributes},
      16);
  CheckSegment(
      {Core.GSBase, State.GS.Limit, State.GS.Selector, State.GS.Attributes},
      {Want.GSBase, Native.GS.Limit, Native.GS.Selector, Native.GS.Attributes},
      32);
  EXPECT_EQ(Core.FSBase, Want.FSBase);
  EXPECT_EQ(Core.GSBase, Want.GSBase);
  auto Actual = Memory.physicalMemory().snapshotMemoryRegions().front().Bytes;
  auto WantRAM = std::vector<uint8_t>(CPU.RAM, CPU.RAM + CPU.Size);
  std::fill(Actual.begin() + 0x10000, Actual.begin() + PageTableEnd, 0);
  std::fill(WantRAM.begin() + 0x10000, WantRAM.begin() + PageTableEnd, 0);
  if (Actual != WantRAM) {
    const auto Difference =
        std::mismatch(Actual.begin(), Actual.end(), WantRAM.begin());
    ADD_FAILURE() << "RAM differs at " << (Difference.first - Actual.begin())
                  << " actual=" << unsigned(*Difference.first)
                  << " native=" << unsigned(*Difference.second);
  }
}

TEST_F(X64PagedExecutionTest, PagedFarPointerLoadsMatchOriginalCPU) {
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
  RecordProperty("smap_available", CPU.SMAP);
  struct Form {
    unsigned Width, Segment, Base, Destination, Length;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (unsigned Width : {2, 4, 8})
    for (unsigned Segment : {0xb2, 0xb4, 0xb5})
      for (unsigned Addressing = 0; Addressing != 11; ++Addressing) {
        const auto Start = Code.size();
        const unsigned Base = Addressing == 0 ? 7 : Addressing % 2 ? 4 : 12;
        const unsigned Destination = Addressing == 0 ? 0 : 13;
        Addresses.push_back(0x1000 + Start);
        if (Addressing >= 3) {
          const uint8_t IgnoredPrefixes[] = {0x26, 0x2e, 0x36, 0x3e};
          Code.push_back(IgnoredPrefixes[(Addressing - 3) / 2]);
        }
        if (Width == 2)
          Code.push_back(0x66);
        const unsigned Rex = (Width == 8 ? 8 : 0) | (Destination >= 8 ? 4 : 0) |
                             (Base >= 8 ? 1 : 0);
        if (Rex)
          Code.push_back(0x40 | Rex);
        Code.push_back(0x0f);
        Code.push_back(Segment);
        Code.push_back(((Destination & 7) << 3) | (Base & 7));
        if ((Base & 7) == 4)
          Code.push_back(0x24);
        Forms.push_back({Width, Segment, Base, Destination,
                         unsigned(Code.size() - Start), 0x1000 + Start});
        Code.resize(Start + 12, 0x90);
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
          for (unsigned Case = 0; Case != 181; ++Case) {
            if (Case == 123 && !CPU.SMAP)
              continue;
            SCOPED_TRACE(llvm::formatv("{0} llvm={1} {2} width={3} "
                                       "segment={4:x} base={5} case={6}",
                                       Compiler, LLVM, O, F.Width, F.Segment,
                                       F.Base, Case)
                             .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = F.PC;
            State.NMIBlocked = 1;
            unsigned CPL = Case < 64 ? (Case >> 4) & 3 : 0;
            unsigned DPL = Case < 64 ? (Case >> 2) & 3 : 0;
            unsigned RPL = Case < 64 ? Case & 3 : 0;
            if (Case >= 96 && Case < 112) {
              CPL = (Case - 96) >> 2;
              RPL = Case & 3;
            }
            if (Case == 122 || Case == 126 || Case == 127 || Case == 130 ||
                Case == 131 || Case == 132 || Case == 133 || Case == 141 ||
                Case == 148)
              CPL = DPL = RPL = 3;
            if (Case >= 149) {
              CPL = RPL = (Case - 149) & 3;
              DPL = ((Case - 149) >> 2) & 3;
            }
            State.Core.CS = {0, UINT32_MAX, uint16_t(24 | CPL),
                             uint16_t(0xa09b | (CPL << 5))};
            State.Core.SS = {0, UINT32_MAX, uint16_t(32 | CPL),
                             uint16_t(0xc093 | (CPL << 5))};
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, State.Core.SS.Selector,
                                   State.Core.SS.Attributes};
            Core.FSBase = 0x120;
            Core.GSBase = 0x340;
            Core.GPR[13] = 0xabcdef0123456789ULL;
            Core.GPR[F.Base] = 0x20000;
            put(0x8024, 0x1c000);
            for (unsigned V : {8, 10, 11, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            uint64_t Descriptor = 0x00cf92000000ffffULL | (uint64_t(DPL) << 45);
            if (Case >= 64 && Case < 96)
              Descriptor = (Descriptor & ~(uint64_t(15) << 40)) |
                           (uint64_t(Case & 15) << 40);
            if (Case >= 80 && Case < 96)
              Descriptor &= ~(uint64_t(1) << 47);
            if (Case >= 149)
              Descriptor |= uint64_t(Case < 165 ? 8 : 12) << 40;
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
              if (Case != 113)
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
            if (Case == 118 || Case == 123 || Case == 132 || Case == 141) {
              std::memmove(RAM.Memory.front().Bytes.data() + 0x7000,
                           RAM.Memory.front().Bytes.data() + 0x6000, 4096);
              State.IDTR.Base = 0x7000;
              State.GDTR.Limit = 0x1007;
              Selector = 0x1000 | (Case == 141 ? RPL : 0);
              put(0x6000, Descriptor);
              put(0x13030, Case == 123 ? 0x6007 : 0);
            }
            if (Case >= 119 && Case <= 121) {
              Core.GPR[F.Base] = 0x21000 - F.Width;
              if (Case != 120)
                put(0x13100, 0);
              if (Case != 119)
                put(0x13108, 0);
            }
            if (Case == 122)
              put(0x13100, 0x30003);
            if (Case == 123) {
              Controls.CR4 |= 0x200000;
              Core.RFlags |= 0x40000; // AC cannot override implicit SMAP.
              for (unsigned I = 0; I != 64; ++I)
                if (I != 6)
                  put(0x13000 + I * 8, I * 4096 | 3);
              put(0x13100, 0x30003);
            }
            if (Case == 124)
              Core.GPR[F.Base] = 0x800000000000ULL;
            if (Case == 126 || Case == 127) {
              Core.GPR[F.Base] += 1;
              Controls.CR0 |= 0x40000;
              Core.RFlags |= 0x40000;
              if (Case == 127)
                put(0x13100, 0);
            }
            if (Case == 128)
              Core.RFlags |= 0x4000;
            if (Case == 129)
              Core.RFlags |= 0x10000;
            if (Case == 130 || Case == 131) {
              Core.GPR[F.Base] = 0x21001 - F.Width;
              Controls.CR0 |= 0x40000;
              Core.RFlags |= 0x40000;
              put(0x13028, 0x5005);
              if (Case == 131)
                put(0x13108, 0);
            }
            if (Case == 133) {
              Selector = 40;
              put(0x5028, Descriptor & ~(uint64_t(1) << 47));
            }
            if (Case == 134)
              put(0x5028, 0x00cf10000000ffffULL);
            if (Case == 135)
              put(0x5028, 0x000002000000ffffULL);
            if (Case >= 136 && Case <= 138) {
              Selector = 12;
              State.LDTR = {0x8ff4, 0x1f, 56, 0x82};
              State.Unusable &= ~64;
              put(0x8ffc, Descriptor);
              put(Case == 136 ? 0x13040 : 0x13048, Case == 136   ? 0x8005
                                                   : Case == 137 ? 0x9005
                                                                 : 0);
            }
            if (Case == 148) {
              // An implicit descriptor read/write does not acquire user #AC.
              Selector = 15;
              State.LDTR = {0x9001, 0x1f, 56, 0x82};
              State.Unusable &= ~64;
              put(0x9009, Descriptor);
              Controls.CR0 |= 0x40000;
              Core.RFlags |= 0x40000;
            }
            if (Case == 139 || Case == 140) {
              Selector = 12;
              State.LDTR = {0x1fff8, 0x1f, 56, 0x82};
              State.Unusable &= ~64;
              put(0x30000, Descriptor);
              if (Case == 140) {
                Core.GPR[F.Base] -= F.Width;
                put(0x13100, 0x30005);
                put(0x130f8, 0x2f007);
              }
            }
            if (Case == 142 || Case == 145) {
              Selector = Case == 142 ? 12 : 0x40;
              Core.GPR[F.Base] = 0x21000 - F.Width;
              put(0x13108, 0);
            }
            if (Case == 143 || Case == 144) {
              Selector = 0;
              State.GDTR = {0x7ffffffffff8ULL, 0};
              State.LDTR.Base = 0x7ffffffffff8ULL;
              if (Case == 144)
                State.GDTR.Limit = 0xffff;
            }
            std::optional<uint64_t> PointerPhysical;
            if (Core.GPR[F.Base] < 0x40000)
              PointerPhysical = 0x30000 + (Core.GPR[F.Base] - 0x20000);
            if (Case == 146 || Case == 147) {
              Core.GPR[F.Base] = 0x800000000000ULL - F.Width;
              put(0x107f8, 0x14007);
              put(0x14ff8, 0x15007);
              put(0x15ff8, 0x16007);
              put(0x16ff8, Case == 146 ? 0x30007 : 0);
              PointerPhysical = 0x31000 - F.Width;
            }
            if (PointerPhysical) {
              const auto Physical = *PointerPhysical;
              const uint64_t Value = 0xdeadbeef12345678ULL;
              if (Case != 139)
                for (unsigned I = 0; I != F.Width; ++I)
                  RAM.Memory.front().Bytes[Physical + I] = Value >> (8 * I);
              RAM.Memory.front().Bytes[Physical + F.Width] = Selector;
              RAM.Memory.front().Bytes[Physical + F.Width + 1] = Selector >> 8;
            }
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0, F.Length, Native,
                Handler, false, nullptr, Controls.CR4, true));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(
                *Memory,
                {.AlignmentPriority = X64PagedAlignmentPriority::BeforePaging,
                 .FarPointerProfile = X64PagedFarPointerProfile::OffsetFirst});
            uint64_t Budget = 1;
            const auto Exit =
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Exit, Handler ? 7u : 9u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_EQ(Context.lastResult().InstructionCompleted, !Handler);
            expectFarPointerCPU(State, Native, *Memory, CPU,
                                Case == 146 || Case == 147 ? 0x17000 : 0x14000);
            ++Comparisons;
            Faults += Handler;
            if (HasFailure())
              return;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("far_pointer_fault_cpu_comparisons", Faults);
}

TEST_F(X64PagedExecutionTest, PagedFarPointerWrapsFieldsAndCapturesOldBases) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  struct Form {
    unsigned Width, Segment, Prefix, Kind, Length;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (unsigned Width : {2, 4, 8})
    for (unsigned Segment : {0xb2, 0xb4, 0xb5})
      for (unsigned Prefix : {0x64, 0x65})
        for (unsigned Kind = 0; Kind != 3; ++Kind) {
          const unsigned Start = Code.size();
          Addresses.push_back(0x1000 + Start);
          Code.push_back(Prefix);
          if (Width == 2)
            Code.push_back(0x66);
          Code.push_back(0x67);
          if (Width == 8)
            Code.push_back(0x48);
          Code.push_back(0x0f);
          Code.push_back(Segment);
          if (Kind == 0) { // Address and destination alias: old EDI.
            Code.push_back(0x3f);
          } else if (Kind == 1) { // Negative EIP-relative displacement wraps.
            Code.push_back(0x3d);
            const uint32_t Disp = -Width - (0x1000 + Code.size() + 4);
            for (unsigned I = 0; I != 4; ++I)
              Code.push_back(Disp >> (I * 8));
          } else { // FS/GS override of SP-based SIB, destination is SP.
            Code.push_back(0x24);
            Code.push_back(0x24);
          }
          Forms.push_back({Width, Segment, Prefix, Kind,
                           unsigned(Code.size() - Start), 0x1000 + Start});
          Code.resize(Start + 16, 0x90);
        }
  unsigned Comparisons = 0;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
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
            Core.FSBase = F.Prefix == 0x64 ? 0x20000 + F.Width : 0x12345678;
            Core.GSBase = F.Prefix == 0x65 ? 0x20000 + F.Width : 0x12345678;
            Core.GPR[F.Kind == 2 ? 4 : 7] = 0xabcdef0100000000ULL - F.Width;
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
            put(0x8024, 0x1c000);
            for (unsigned V : {8, 10, 11, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            put(0x11020, 0x14007); // 4 GiB+0x20000 maps the offset field.
            put(0x14000, 0x15007);
            put(0x15100, Case == 2 ? 0 : 0x31007);
            put(0x13100, Case == 3 ? 0 : 0x30007); // Wrapped selector field.
            put(0x5028, 0x00cf92567800ffffULL);
            put(0x31000, 0x1d000); // Also a usable new SP for the SP form.
            put(0x30000 + F.Width, Case == 1 ? 0 : 40);
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0, F.Length, Native,
                Handler, false, nullptr, Controls.CR4, true));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(
                *Memory,
                {.FarPointerProfile = X64PagedFarPointerProfile::OffsetFirst});
            uint64_t Budget = 1;
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                Handler ? 7u : 9u);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Budget, 0u);
            expectFarPointerCPU(State, Native, *Memory, CPU, 0x16000);
            ++Comparisons;
            if (HasFailure())
              return;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionTest, PagedFarPointerAdmissionBackingAndNullPolicies) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool SS : {false, true})
    for (bool LLVM : {false, true})
      for (const auto Policy :
           {std::optional<X64NullSegmentBasePolicy>{},
            std::optional(X64NullSegmentBasePolicy::Preserve)}) {
        const std::vector<uint8_t> Code{0x48, 0x0f, uint8_t(SS ? 0xb2 : 0xb4),
                                        0x07};
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                        NEVERD_TEST_CLANG, 0x1000, &Scheduled,
                                        Policy));
        for (unsigned Case = 0; Case != 10; ++Case) {
          SCOPED_TRACE(llvm::formatv("SS={0} llvm={1} preserve={2} case={3}",
                                     SS, LLVM, bool(Policy), Case)
                           .str());
          reset(Code);
          auto &Core = State.Core.Core.Core.Core;
          Core.FSBase = 0x12345678;
          State.NMIBlocked = 1;
          put(0x5028, 0x00cf92000000ffffULL);
          put(0x30000, 0x87654321);
          put(0x30008, Case >= 7 ? 0 : 40);
          X64PagedExecutionOptions Options;
          if (Case)
            Options.FarPointerProfile = X64PagedFarPointerProfile::OffsetFirst;
          if (Case == 1)
            RAM.Memory.front().Bytes[0x1000] = 0x90;
          if (Case == 2)
            put(0x13008, 0);
          if (Case == 3)
            put(0x13100, 0x50007);
          if (Case == 4)
            put(0x13028, 0x50007);
          if (Case == 5 || Case == 6) {
            put(0x30008, 0x40);
            if (Case == 5)
              put(0x13030, 0x50007);
            else
              State.IDTR.Limit = 0;
          }
          if (Case == 8)
            put(0x13028, 0x50007); // A null load never consults the GDT.
          if (Case == 9)
            put(0x13100, 0);
          const auto Old = State;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(*Memory, Options);
          Shadows Shadow;
          Shadow.SSBlocked = 1;
          Producer P;
          uint64_t Budget = 1, Polls = 1;
          const bool NullRefused = Case >= 7 && Case <= 8 && !SS && !Policy;
          const bool Attempt = Case == 2 || Case >= 5;
          const unsigned Expected = Case == 0 || NullRefused ? 5
                                    : Case == 1              ? 20
                                    : Case == 2 || Case == 9 ? 7
                                    : Case == 6              ? 19
                                    : Case >= 7              ? 9
                                                             : 18;
          EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                              &Budget, &Shadow, &Polls, Producer::peek,
                              Producer::accepted, &P),
                    Expected);
          EXPECT_EQ(Budget, Attempt && !NullRefused ? 0u : 1u);
          EXPECT_EQ(Shadow.SSBlocked, Attempt && !NullRefused ? 0u : 1u);
          EXPECT_EQ(State.NMIBlocked, 1);
          EXPECT_EQ(P.Acks, 0u);
          if (Case == 0)
            EXPECT_NE(Context.lastError().find("bound access order"),
                      std::string::npos);
          else
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          if (!Attempt || NullRefused)
            EXPECT_EQ(std::memcmp(&State, &Old, sizeof(State)), 0);
          if (Expected == 9) {
            EXPECT_EQ(Core.PC, 0x1004u);
            EXPECT_EQ(Core.GPR[0], 0x87654321u);
            EXPECT_EQ(Core.FSBase, Old.Core.Core.Core.Core.FSBase);
            EXPECT_EQ(SS ? State.Core.SS.Selector : State.FS.Selector, 0);
            EXPECT_TRUE(State.Unusable & (SS ? 2 : 16));
          }
        }
      }
}

TEST_F(X64PagedExecutionTest, PagedLssComposesWithScheduledIRQAndNMI) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  unsigned Comparisons = 0;
  for (unsigned Width : {2, 4, 8}) {
    std::vector<uint8_t> Code;
    if (Width != 4)
      Code.push_back(Width == 2 ? 0x66 : 0x48);
    Code.insert(Code.end(), {0x0f, 0xb2, 0x07});
    const unsigned Length = Code.size();
    Code.push_back(0x90);
    for (bool LLVM : {false, true}) {
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver,
                                      {0x1000, 0x1000 + Length},
                                      NEVERD_TEST_CLANG, 0x1000, &Scheduled));
      for (bool NMI : {false, true})
        for (bool Null : {false, true}) {
          reset(Code);
          State.Core.Core.Core.Core.RFlags = 0x202;
          State.DS = State.ES = State.Core.SS;
          State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
          put(0x30000, 0x1234);
          put(0x30000 + Width, Null ? 0 : 40);
          put(0x5028, 0x00cf92000000ffffULL);
          X64ContinuationEventV1 Event{};
          Event.Kind = NMI ? X64ContinuationEventKind::NMI
                           : X64ContinuationEventKind::Interrupt;
          Event.Vector = NMI ? 2 : 65;
          Event.SSBlocked = 1;
          X64ContinuationStateV6 Native;
          bool Handler = false;
          ASSERT_NO_FATAL_FAILURE(
              CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0, Length,
                          Native, Handler, true, &Event, Controls.CR4, true));
          ASSERT_TRUE(Handler);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(
              *Memory,
              {.FarPointerProfile = X64PagedFarPointerProfile::OffsetFirst});
          Shadows Shadow;
          Shadow.SSBlocked = 1;
          Producer P;
          P.Candidate = {Event.Vector, uint8_t(Event.Kind)};
          uint64_t Budget = 2, Polls = 2;
          EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                              &Budget, &Shadow, &Polls, Producer::peek,
                              Producer::accepted, &P),
                    7u);
          EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          EXPECT_EQ(Budget, 1u);
          EXPECT_EQ(Shadow.SSBlocked, 0);
          EXPECT_EQ(P.Polls, 2u);
          EXPECT_EQ(P.Acks, 1u);
          expectFarPointerCPU(State, Native, *Memory, CPU);
          ++Comparisons;
        }
    }
  }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionTest, PagedFarPointerHandlerRepairsMappingAndRetries) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  std::vector<uint8_t> Code(0x1012, 0x90);
  Code[0] = Code[0x1010] = 0x48;
  Code[1] = 0x0f;
  Code[3] = 0x07;
  Code[0x1011] = 0xcf;
  const uint8_t HandlerCode[] = {0x48, 0xc7, 0x04, 0x25, 0x00, 0x31,
                                 0x01, 0x00, 0x07, 0x00, 0x03, 0x00,
                                 0x48, 0x83, 0xc4, 0x08};
  std::copy(std::begin(HandlerCode), std::end(HandlerCode),
            Code.begin() + 0x1000);
  unsigned Comparisons = 0;
  for (unsigned Segment : {0xb2, 0xb4, 0xb5})
    for (bool LLVM : {false, true}) {
      Code[2] = Segment;
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver,
                                      {0x1000, 0x2000, 0x200c, 0x2010},
                                      NEVERD_TEST_CLANG, 0x1000, &Scheduled));
      reset(Code);
      State.NMIBlocked = 1;
      State.DS = State.ES = State.Core.SS;
      State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
      put(0x30000, 0x12345678);
      put(0x30008, 40);
      put(0x5028, 0x00cf92000000ffffULL);
      put(0x13100, 0);
      put(0x8024, 0x1c000);
      put(0x60e0, 0x2000 | (uint64_t(8) << 16) | (uint64_t(1) << 32) |
                      (uint64_t(0x8e) << 40));
      X64ContinuationStateV6 Native;
      bool Handler = false;
      ASSERT_NO_FATAL_FAILURE(CPU.execute(RAM.Memory.front().Bytes, State,
                                          Controls.CR0, 4, Native, Handler,
                                          false, nullptr, Controls.CR4, true));
      ASSERT_FALSE(Handler);
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(
          *Memory,
          {.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst,
           .FarPointerProfile = X64PagedFarPointerProfile::OffsetFirst});
      for (unsigned Phase = 0; Phase != 3; ++Phase) {
        uint64_t Budget = Phase == 1 ? 3 : 1;
        EXPECT_EQ(
            Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
            Phase == 0   ? 7u
            : Phase == 1 ? 6u
                         : 9u);
        EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
        EXPECT_EQ(Budget, 0u);
        EXPECT_EQ(State.NMIBlocked, Phase == 0 ? 1 : 0);
        EXPECT_EQ(State.Core.Core.Core.Core.PC, Phase == 0   ? 0x2000u
                                                : Phase == 1 ? 0x1000u
                                                             : 0x1004u);
      }
      expectFarPointerCPU(State, Native, *Memory, CPU);
      ++Comparisons;
    }
  RecordProperty("cpu_comparisons", Comparisons);
}
#endif
