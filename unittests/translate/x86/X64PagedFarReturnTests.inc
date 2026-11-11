//===- X64PagedFarReturnTests.inc - Faultable far returns -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original CPU comparison for RETF widths, parameter cleanup, privilege
/// transitions and ordered descriptor/frame faults through generated C.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, PagedFarReturnsMatchOriginalCPU) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  RecordProperty("return_processor", CPU.ReturnProcessor);
  RecordProperty("return_core_type", CPU.ReturnCoreType);
  const auto ReturnProfile =
      CPU.ReturnCoreType == 0x20
          ? X64PagedFarReturnProfile::SelectorFirstTruncatedIP
          : X64PagedFarReturnProfile::SelectorFirst;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  struct Form {
    unsigned Width, Discard, Length;
    uint64_t Address;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code(9 * 8, 0x90);
  std::vector<uint64_t> Addresses;
  for (unsigned Width : {2, 4, 8})
    for (unsigned Discard : {0, 16, 65535}) {
      const unsigned Offset = Forms.size() * 8;
      unsigned At = Offset;
      if (Width != 4)
        Code[At++] = Width == 2 ? 0x66 : 0x48;
      Code[At++] = Discard ? 0xca : 0xcb;
      if (Discard) {
        Code[At++] = Discard;
        Code[At++] = Discard >> 8;
      }
      Forms.push_back({Width, Discard, At - Offset, 0x1000 + Offset});
      Addresses.push_back(0x1000 + Offset);
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
                                        Compiler, 0x1000, &Scheduled));
        for (const auto &F : Forms)
          for (unsigned Case = 0; Case != 96; ++Case) {
            SCOPED_TRACE(
                llvm::formatv("{0} llvm={1} {2} width={3} discard={4} case={5}",
                              Compiler, LLVM, O, F.Width, F.Discard, Case)
                    .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            for (unsigned I = 0; I != std::size(Core.Undefined); ++I)
              Core.Undefined[I] = 0xa500000000000000ULL | (uint64_t(I) << 32);
            Core.PC = F.Address;
            Core.GPR[4] = 0x1d000;
            State.NMIBlocked = 1;
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
            put(0x8024, 0x1c000);
            RAM.Memory.front().Bytes[0x1800] =
                RAM.Memory.front().Bytes[0x1801] = 0x90;
            for (unsigned V : {8, 10, 11, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            unsigned OldCPL = Case < 32 ? (Case >> 2) & 3 : 0;
            unsigned CPL = Case < 32 ? Case & 3 : 3;
            if (Case == 51 || Case == 55 || (Case >= 58 && Case < 63))
              OldCPL = 3;
            State.Core.CS = {0, UINT32_MAX, uint16_t(24 | OldCPL),
                             uint16_t(0xa09b | (OldCPL << 5))};
            State.Core.SS = {0, UINT32_MAX, uint16_t(32 | OldCPL),
                             uint16_t(0xc093 | (OldCPL << 5))};
            uint64_t PC = 0x1800, CS = 40 | CPL, SP = 0xe800, SS = 48 | CPL;
            put(0x5028, ((Case < 32 && (Case & 16)) ? 0x00cf9a000000ffffULL
                                                    : 0x00af9a000000ffffULL) |
                            (uint64_t(CPL) << 45));
            put(0x5030, 0x00cf92000000ffffULL | (uint64_t(CPL) << 45));
            switch (Case) {
            case 32:
              CS = 0;
              break;
            case 33:
              CS = 0x40;
              break;
            case 34:
              CS |= 4;
              break;
            case 35:
              RAM.Memory.front().Bytes[0x502d] &= ~0x80;
              break;
            case 36:
              RAM.Memory.front().Bytes[0x502d] &= ~0x60;
              break;
            case 37:
              RAM.Memory.front().Bytes[0x502e] |= 0x40;
              break;
            case 38:
              SS = 0;
              break;
            case 39:
              SS = 0x40;
              break;
            case 40:
              RAM.Memory.front().Bytes[0x5035] &= ~0x80;
              break;
            case 41:
              RAM.Memory.front().Bytes[0x5035] &= ~2;
              break;
            case 42:
              put(0x130e8, 0);
              break;
            case 43:
              Core.GPR[4] = 0x1e000 - F.Width;
              put(0x130f0, 0);
              break;
            case 44:
              Core.GPR[4] = 0x1e000 - 2 * F.Width;
              put(0x130f0, 0);
              break;
            case 45:
              put(0x13028, 0x5005);
              break;
            case 46:
              Core.RFlags |= 0x4000;
              break;
            case 47:
              Core.RFlags |= 0x10000;
              break;
            case 48:
              PC = 0x800000000000ULL;
              break;
            case 49:
              SP = 0x800000000000ULL;
              break;
            case 50:
              Core.GPR[4] = 0x800000000000ULL;
              break;
            case 52:
              put(0x13100, 0);
              break;
            case 53:
              PC |= 0x1000000000000ULL;
              put(0x5028, 0x00cffa000000ffffULL);
              break;
            case 54:
              put(0x5028, 0x0000fa0000001000ULL);
              break;
            case 56:
            case 57:
              PC |= Case == 56 ? 0x100000000ULL : 0xffffffff00000000ULL;
              put(0x5028, 0x00cffa000000ffffULL);
              break;
            case 55:
              Core.GPR[4] += 1;
              Core.RFlags |= 0x40000;
              Controls.CR0 |= 0x40000;
              break;
            }
            if (Case == 58) {
              CS = 0x1000;
              State.GDTR.Limit = 0x1007;
              std::memmove(RAM.Memory.front().Bytes.data() + 0x7000,
                           RAM.Memory.front().Bytes.data() + 0x6000, 4096);
              State.IDTR.Base = 0x7000;
              put(0x6000, 0x00af9a000000ffffULL);
              put(0x13030, 0);
            }
            if (Case >= 59 && Case < 62) {
              Core.GPR[4] = 0x1e000 - F.Width;
              put(0x130e8, 0);
              if (Case == 59)
                CS = 40;
              if (Case == 60)
                CS = 0;
              if (Case == 61)
                put(0x5028, 0);
            }
            if (Case == 62) {
              CS = 40;
              put(0x5028, 0x00af1a000000ffffULL);
            }
            if (Case == 63) {
              SS = 0x1000;
              State.GDTR.Limit = 0x1007;
              std::memmove(RAM.Memory.front().Bytes.data() + 0x7000,
                           RAM.Memory.front().Bytes.data() + 0x6000, 4096);
              State.IDTR.Base = 0x7000;
              put(0x6000, 0x00cff2000000ffffULL);
              put(0x13030, 0);
            }
            if (Case >= 64) {
              const unsigned N = Case - 64, CPL = N & 3, DPL = (N >> 2) & 3;
              State.LDTR = {0x9000, 0x1f, 56, 0x82};
              State.Unusable &= ~64;
              CS = 12 | CPL;
              SS = 20 | CPL;
              put(0x9008,
                  (N & 16 ? 0x00af9e000000ffffULL : 0x00af9a000000ffffULL) |
                      (uint64_t(DPL) << 45));
              put(0x9010, 0x00cf92000000ffffULL | (uint64_t(CPL) << 45));
            }
            if (Core.GPR[4] < 0x40000) {
              const uint64_t Values[] = {PC, CS, SP, SS};
              for (unsigned I = 0; I != 4; ++I) {
                const uint64_t Address =
                    Core.GPR[4] + I * F.Width + (I >= 2 ? F.Discard : 0);
                for (unsigned B = 0; B != F.Width; ++B)
                  RAM.Memory.front().Bytes[Address + B] = Values[I] >> (8 * B);
              }
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
                 .FarReturnProfile = ReturnProfile});
            uint64_t Budget = 1;
            const auto Exit =
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Exit, Handler ? 7u : 6u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_EQ(Context.lastResult().InstructionCompleted, !Handler);
            EXPECT_EQ(State.NMIBlocked, Native.NMIBlocked);
            const auto &Want = Native.Core.Core.Core.Core;
            EXPECT_EQ(Core.PC, Want.PC);
            EXPECT_EQ(Core.RFlags, Want.RFlags);
            const auto Mask = !Handler && !(Native.Core.CS.Attributes & 0x2000)
                                  ? uint64_t(UINT32_MAX)
                                  : UINT64_MAX;
            for (unsigned I = 0; I != 16; ++I)
              EXPECT_EQ(Core.GPR[I] & Mask, Want.GPR[I] & Mask) << I;
            if (Mask == UINT32_MAX)
              for (unsigned I = 0; I != 16; ++I)
                EXPECT_EQ(Core.GPR[I] >> 32, Core.Undefined[I] >> 32) << I;
            EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
            EXPECT_EQ(State.Core.CS.Selector, Native.Core.CS.Selector);
            EXPECT_EQ(State.Core.SS.Selector, Native.Core.SS.Selector);
            EXPECT_EQ(State.Core.CS.Attributes, Native.Core.CS.Attributes);
            if (!(Native.Unusable & 2))
              EXPECT_EQ(State.Core.SS.Attributes, Native.Core.SS.Attributes);
            EXPECT_EQ(State.Core.CS.Base, Native.Core.CS.Base);
            EXPECT_EQ(State.Core.SS.Base, Native.Core.SS.Base);
            EXPECT_EQ(State.Core.CS.Limit, Native.Core.CS.Limit);
            EXPECT_EQ(State.Core.SS.Limit, Native.Core.SS.Limit);
            EXPECT_EQ(State.DS.Selector, Native.DS.Selector);
            EXPECT_EQ(State.ES.Selector, Native.ES.Selector);
            EXPECT_EQ(State.FS.Selector, Native.FS.Selector);
            EXPECT_EQ(State.GS.Selector, Native.GS.Selector);
            EXPECT_EQ(State.Unusable, Native.Unusable);
            EXPECT_EQ(Core.FSBase, Want.FSBase);
            EXPECT_EQ(Core.GSBase, Want.GSBase);
            auto Actual =
                Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
            auto WantRAM = std::vector<uint8_t>(CPU.RAM, CPU.RAM + CPU.Size);
            std::fill(Actual.begin() + 0x10000, Actual.begin() + 0x14000, 0);
            std::fill(WantRAM.begin() + 0x10000, WantRAM.begin() + 0x14000, 0);
            if (Actual != WantRAM) {
              const auto D =
                  std::mismatch(Actual.begin(), Actual.end(), WantRAM.begin());
              ADD_FAILURE() << "RAM differs at " << (D.first - Actual.begin())
                            << " actual=" << unsigned(*D.first)
                            << " native=" << unsigned(*D.second);
            }
            ++Comparisons;
            Faults += Handler;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("far_return_fault_cpu_comparisons", Faults);
}

TEST_F(X64PagedExecutionTest, PagedFarReturnRetainedEffectsControlProgress) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0xcb};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                    NEVERD_TEST_CLANG, 0x1000, &Scheduled));
    for (unsigned Case = 0; Case != 9; ++Case) {
      SCOPED_TRACE(Case);
      reset(Code);
      auto &Core = State.Core.Core.Core.Core;
      Core.GPR[4] = Case == 5 || Case == 6 ? 0x1dff0 : 0x1d000;
      State.NMIBlocked = 1;
      put(0x5028, Case == 6 ? 0x00affb000000ffffULL : 0x00affa000000ffffULL);
      put(0x5030, 0x00cff2000000ffffULL);
      const uint64_t Frame[] = {0x1800, 43, 0xe800, 51};
      for (unsigned I = 0; I != 4; ++I)
        put(Core.GPR[4] + I * 8, Frame[I]);
      X64PagedExecutionOptions Options;
      if (Case)
        Options.FarReturnProfile = X64PagedFarReturnProfile::SelectorFirst;
      if (Case == 1)
        RAM.Memory.front().Bytes[0x1000] = 0x90;
      if (Case == 2)
        put(0x13008, 0);
      if (Case == 3)
        put(0x130e8, 0x50007);
      if (Case == 4)
        put(0x13028, 0x50007);
      if (Case == 5 || Case == 6)
        put(0x130f0, 0x50007);
      if (Case >= 7) {
        put(Core.GPR[4] + 8, 0);
        if (Case == 7)
          put(0x13030, 0x50007);
        else
          State.IDTR.Limit = 0;
      }
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, Options);
      Shadows Shadow;
      Shadow.SSBlocked = 1;
      Producer P;
      uint64_t Budget = 1, Polls = 1;
      const auto Exit =
          Scheduled(&State, reinterpret_cast<uint8_t *>(&Context), &Budget,
                    &Shadow, &Polls, Producer::peek, Producer::accepted, &P);
      const bool Attempt = Case == 2 || Case == 5 || Case >= 7;
      EXPECT_EQ(Exit, Case == 0   ? 5u
                      : Case == 1 ? 20u
                      : Case == 2 ? 7u
                      : Case == 8 ? 19u
                                  : 18u);
      EXPECT_EQ(Budget, Attempt ? 0u : 1u);
      EXPECT_EQ(State.NMIBlocked, 1);
      EXPECT_EQ(Context.lastResult().InstructionAttempted, Attempt);
      EXPECT_EQ(Shadow.SSBlocked, Attempt && Case != 5 ? 0 : 1);
      EXPECT_EQ(P.Acks, 0u);
      if (Case == 0)
        EXPECT_NE(Context.lastError().find("bound frame-access order"),
                  std::string::npos);
      else
        EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
      if (Case == 5 || Case == 6)
        EXPECT_EQ(Memory->physicalMemory()
                      .loadScalar(0x5028, GuestScalarWidth::I64)
                      .Value,
                  0x00affb000000ffffULL);
    }
  }
}
#endif
