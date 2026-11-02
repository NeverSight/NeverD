//===- X64PagedReturnTests.inc - Faultable interrupt returns -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included after the paged source fixture. Execute original IRET bytes on an
/// independent CPU and compare generated returns, fault frames and RAM writes.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, PagedInterruptReturnsMatchOriginalCPU) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  RecordProperty("return_processor", CPU.ReturnProcessor);
  RecordProperty("return_core_type", CPU.ReturnCoreType);
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  std::vector<uint8_t> Code(0x22, 0x90);
  Code[0] = 0xcf;
  Code[0x10] = 0x66;
  Code[0x11] = 0xcf;
  Code[0x20] = 0x48;
  Code[0x21] = 0xcf;
  unsigned Comparisons = 0, Faults = 0, VirtualizedNMI = 0;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver,
                                        {0x1000, 0x1010, 0x1020}, Compiler,
                                        0x1000, &Scheduled));
        for (unsigned Width : {2, 4, 8})
          for (unsigned Case = 0; Case != 256; ++Case) {
            if (Case >= 244 && Case < 250 && Width != 8)
              continue;
            SCOPED_TRACE(llvm::formatv("llvm={0} {1} width={2} case={3}", LLVM,
                                       O, Width, Case)
                             .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            for (unsigned I = 0; I != std::size(Core.Undefined); ++I)
              Core.Undefined[I] = 0xa500000000000000ULL | (uint64_t(I) << 32);
            Core.PC = Width == 2 ? 0x1010 : Width == 4 ? 0x1000 : 0x1020;
            Core.GPR[4] = 0x1d000;
            State.NMIBlocked = 1;
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
            put(0x8024, 0x1c000);
            RAM.Memory.front().Bytes[0x1800] = 0x90;
            RAM.Memory.front().Bytes[0x1801] = 0x90;
            for (unsigned V : {8, 10, 11, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            put(0x5028, 0x00af9a000000ffffULL);
            put(0x5030, 0x00cf92000000ffffULL);
            uint64_t PC = 0x1800, CS = 40, Flags = 0x246, SP = 0xe800, SS = 48;
            switch (Case) {
            case 1:
              SS = 0;
              break;
            case 2:
              CS = 0;
              break;
            case 3:
              CS = 0x38;
              break; // Zero descriptor.
            case 4:
              CS = 0x40;
              break; // Table limit.
            case 5:
              CS |= 4;
              break; // Unusable LDT.
            case 6:
              put(0x5028, 0x00af1a000000ffffULL);
              break;
            case 7:
              put(0x5028, 0x00cf92000000ffffULL);
              break;
            case 8:
              put(0x5028, 0x00ef9a000000ffffULL);
              break;
            case 9:
              CS |= 3;
              break;
            case 10:
              put(0x5028, 0x00affa000000ffffULL);
              break;
            case 11:
              SS = 0x40;
              break;
            case 12:
              SS |= 4;
              break;
            case 13:
              SS |= 3;
              break;
            case 14:
              put(0x5030, 0x00cf12000000ffffULL);
              break;
            case 15:
              put(0x5030, 0x00cf90000000ffffULL);
              break;
            case 16:
              put(0x5030, 0x00cff2000000ffffULL);
              break;
            case 17:
              put(0x13028, 0x5005);
              break; // Descriptor accessed write #PF.
            case 18:
              put(0x130e8, 0);
              break; // First frame word #PF.
            case 19:
              Core.GPR[4] = 0x1e000 - 3 * Width;
              break;
            case 20:
              Core.GPR[4] = 0x1e000 - 4 * Width;
              break;
            case 21:
              Core.GPR[4] = 0x800000000000ULL;
              break;
            case 22:
              PC = 0x800000000000ULL;
              break;
            case 23:
              SP = 0x800000000000ULL;
              break;
            case 24:
              Core.RFlags |= 0x4000;
              break;
            case 25:
              Core.RFlags |= 0x10000;
              break;
            case 26:
              Core.GPR[4] |= 3;
              break;
            case 27:
              CS = 27;
              SS = 35;
              break;
            case 28:
              CS = 27;
              SS = 3;
              break;
            case 29:
              State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
              State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
              break; // User IRET may not enter ring zero.
            case 30:
            case 31:
              State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
              State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
              CS = 27;
              SS = 35;
              Flags = 0x183202;
              Core.RFlags = Case == 30 ? 2 : 0x3002;
              break;
            default:
              if (Case >= 32 && Case < 96)
                Flags = 2 | (uint64_t(1) << (Case - 32));
              break;
            }
            if (Case >= 96 && Case < 112) {
              const unsigned N = Case - 96, CPL = N & 3;
              CS |= CPL;
              SS = N & 8 ? CPL : SS | CPL;
              put(0x5028,
                  (N & 4 ? 0x00cf9a000000ffffULL : 0x00009a000000ffffULL) |
                      (uint64_t(CPL) << 45));
              put(0x5030,
                  (N & 4 ? 0x00cf92000000ffffULL : 0x000092000000ffffULL) |
                      (uint64_t(CPL) << 45));
            }
            if (Case >= 112 && Case < 144) {
              const unsigned N = Case - 112, CPL = N & 3, IOPL = (N >> 2) & 3;
              State.Core.CS = {0, UINT32_MAX, uint16_t(24 | CPL),
                               uint16_t(0xa09b | (CPL << 5))};
              State.Core.SS = {0, UINT32_MAX, uint16_t(32 | CPL),
                               uint16_t(0xc093 | (CPL << 5))};
              Core.RFlags = 2 | (IOPL << 12) | (N & 16 ? 512 : 0);
              CS |= CPL;
              SS |= CPL;
              Flags = 0x183002 | (N & 16 ? 0 : 512);
              put(0x5028, 0x00af9a000000ffffULL | (uint64_t(CPL) << 45));
              put(0x5030, 0x00cf92000000ffffULL | (uint64_t(CPL) << 45));
            }
            if (Case >= 144 && Case < 152) {
              State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
              State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
              CS = 27;
              SS = 35;
              Core.RFlags = 0x40002;
              Core.GPR[4] += Case - 144;
              Controls.CR0 |= 0x40000;
              if (Case & 4)
                put(0x130e8, 0);
            }
            if (Case >= 152 && Case < 160) {
              const unsigned N = Case - 152;
              PC = 0x800000000000ULL;
              SS = N & 1 ? 0x40 : 0;
              if (N & 2) {
                Core.GPR[4] = 0x1e000 - 3 * Width;
                put(0x130f0, 0);
              }
              if (N & 4)
                CS = 0;
            }
            if (Case >= 160 && Case < 176) {
              const unsigned N = Case - 160;
              Core.GPR[4] = 0x1e000 - (N % 8 + 1) * Width / 2;
              put(N & 8 ? 0x130e8 : 0x130f0, 0);
            }
            if (Case >= 176 && Case < 192) {
              const unsigned N = Case - 176, CPL = N & 3;
              CS |= CPL;
              SS = N >> 2;
              put(0x5028, 0x00af9a000000ffffULL | (uint64_t(CPL) << 45));
            }
            if (Case >= 192 && Case < 224) {
              const unsigned N = Case - 192, CPL = N & 3, DPL = (N >> 2) & 3;
              State.LDTR = {0x9000, 0x1f, 56, 0x82};
              State.Unusable &= ~64;
              CS = 12 | CPL;
              SS = 20 | CPL;
              put(0x9008,
                  (N & 16 ? 0x00af9e000000ffffULL : 0x00af9a000000ffffULL) |
                      (uint64_t(DPL) << 45));
              put(0x9010, 0x00cf92000000ffffULL | (uint64_t(CPL) << 45));
            }
            if (Case >= 224 && Case < 240) {
              const unsigned N = Case - 224;
              State.GDTR.Base += N;
              std::memmove(RAM.Memory.front().Bytes.data() + State.GDTR.Base,
                           RAM.Memory.front().Bytes.data() + 0x5000, 64);
            }
            if (Case >= 240 && Case < 244) {
              const unsigned N = Case - 240;
              Core.GPR[4] = 0x800000000000ULL - (N + 1) * Width;
              put(0x107f8, 0x14007);
              put(0x14ff8, 0x15007);
              put(0x15ff8, 0x16007);
              put(0x16ff8, 0x30007);
            }
            if (Case >= 244 && Case < 247) {
              const unsigned N = Case - 244;
              CS = 8;
              SS = N == 1 ? 16 : 0;
              SP = N ? 0x00cf92000000ffffULL : 0x00af9a000000ffffULL;
              State.GDTR.Base = Core.GPR[4] + (N ? 8 : 16);
              State.GDTR.Limit = 31;
              if (N == 1)
                Flags = 0x00af9b000000ffffULL;
              if (N == 2) {
                Flags = 0x209a;
                SP = 0x19123;
                State.GDTR.Base = Core.GPR[4] + 3;
                State.GDTR.Limit = 15;
              }
            }
            if (Case >= 247 && Case < 250) {
              PC |= Case == 247   ? 0x100000000ULL
                    : Case == 248 ? 0x1000000000000ULL
                                  : 0xffffffff00000000ULL;
              put(0x5028, 0x00cf9a000000ffffULL);
            }
            if (Case >= 250 && Case < 253) {
              Core.GPR[4] = 0x1e000 - Width;
              put(0x130e8, 0);
              if (Case == 250)
                CS = 0;
              if (Case == 251) {
                State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
                State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
              }
              if (Case == 252)
                put(0x5028, 0);
            }
            if (Case == 253 || Case == 254) {
              if (Case == 253) {
                State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
                State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
                CS = 0x1000;
              } else {
                CS |= 3;
                SS = 0x1000;
                put(0x5028, 0x00affa000000ffffULL);
              }
              State.GDTR.Limit = 0x1007;
              std::memmove(RAM.Memory.front().Bytes.data() + 0x7000,
                           RAM.Memory.front().Bytes.data() + 0x6000, 4096);
              State.IDTR.Base = 0x7000;
              put(0x6000, 0x00af9a000000ffffULL);
              put(0x13030, 0);
            }
            if (Case == 255) {
              State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
              State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
              put(0x5028, 0x00af1a000000ffffULL);
            }
            if (Core.GPR[4] < 0x40000 || Case >= 240)
              for (unsigned I = 0; I != 5; ++I) {
                const uint64_t Value[] = {PC, CS, Flags, SP, SS};
                for (unsigned B = 0; B != Width; ++B)
                  if (Case < 240 || Case >= 244)
                    RAM.Memory.front().Bytes[Core.GPR[4] + I * Width + B] =
                        Value[I] >> (B * 8);
                  else if (Core.GPR[4] + I * Width + B < 0x800000000000ULL)
                    RAM.Memory.front()
                        .Bytes[0x30000 + ((Core.GPR[4] + I * Width + B) &
                                          0xfff)] = Value[I] >> (B * 8);
              }
            if (Case == 19 || Case == 20)
              put(0x130f0, 0);
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(
                CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                            Width == 4 ? 1 : 2, Native, Handler, false, nullptr,
                            Controls.CR4, true));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(
                *Memory,
                {.AlignmentPriority = X64PagedAlignmentPriority::BeforePaging,
                 .IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst});
            uint64_t Budget = 1;
            const auto Exit =
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Exit, Handler ? 7u : 6u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_EQ(Context.lastResult().InstructionCompleted, !Handler);
            if (Handler && Native.Core.Core.Core.Core.PC == 0x4110) {
              // VMX intercepts #AC. Linux vmx_recover_nmi_blocking restores the
              // pre-IRET NMI mask before reinjecting this exception. This bit
              // is a VMM side effect, not an original CPU witness for SDM
              // IRET's unconditional NMI unblocking. Keep and count the
              // discrepancy.
              EXPECT_EQ(State.NMIBlocked, 0);
              VirtualizedNMI += Native.NMIBlocked != State.NMIBlocked;
            } else {
              EXPECT_EQ(State.NMIBlocked, Native.NMIBlocked);
            }
            const auto &Want = Native.Core.Core.Core.Core;
            EXPECT_EQ(Core.PC, Want.PC);
            EXPECT_EQ(Core.RFlags, Want.RFlags);
            const uint64_t RegisterMask =
                !Handler && !(Native.Core.CS.Attributes & 0x2000) ? UINT32_MAX
                                                                  : UINT64_MAX;
            for (unsigned I = 0; I != 16; ++I)
              EXPECT_EQ(Core.GPR[I] & RegisterMask, Want.GPR[I] & RegisterMask)
                  << I;
            if (RegisterMask == UINT32_MAX)
              for (unsigned I = 0; I != 16; ++I)
                EXPECT_EQ(Core.GPR[I] >> 32, Core.Undefined[I] >> 32) << I;
            EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
            EXPECT_EQ(State.Core.CS.Selector, Native.Core.CS.Selector);
            EXPECT_EQ(State.Core.SS.Selector, Native.Core.SS.Selector);
            EXPECT_EQ(State.Core.CS.Attributes, Native.Core.CS.Attributes);
            // VMX canonicalizes unusable segment attributes on VM exit. Such a
            // cache cannot be used for access and is not an independent witness
            // for the software model's retained null-SS metadata.
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
            const auto Actual =
                Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
            auto WantRAM = std::vector<uint8_t>(CPU.RAM, CPU.RAM + CPU.Size);
            auto Comparable = Actual;
            std::fill(Comparable.begin() + 0x10000,
                      Comparable.begin() + (Case >= 240 ? 0x17000 : 0x14000),
                      0);
            std::fill(WantRAM.begin() + 0x10000,
                      WantRAM.begin() + (Case >= 240 ? 0x17000 : 0x14000), 0);
            if (Comparable != WantRAM) {
              const auto Difference = std::mismatch(
                  Comparable.begin(), Comparable.end(), WantRAM.begin());
              ADD_FAILURE() << "RAM differs at "
                            << (Difference.first - Comparable.begin())
                            << " actual=" << unsigned(*Difference.first)
                            << " native=" << unsigned(*Difference.second);
            }
            ++Comparisons;
            Faults += Handler;
          }
      }
  RecordProperty("iret_cpu_comparisons", Comparisons);
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("iret_fault_cpu_comparisons", Faults);
  RecordProperty("iret_ac_vmm_nmi_discrepancies", VirtualizedNMI);
}

TEST_F(X64PagedExecutionTest, PagedIretComposesWithScheduledIRQAndNMI) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  std::vector<uint8_t> Code(18, 0x90);
  Code[16] = 0x48;
  Code[17] = 0xcf;
  unsigned Comparisons = 0;
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000, 0x1010},
                                    NEVERD_TEST_CLANG, 0x1000, &Scheduled));
    for (bool NMI : {false, true})
      for (bool User : {false, true}) {
        reset(Code);
        State.Core.Core.Core.Core.RFlags = 0x202;
        if (User) {
          State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
          State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
          State.Core.Core.Core.Core.GPR[4] = 0x1d000;
        }
        const unsigned Vector = NMI ? 2 : 65;
        put(0x6000 + Vector * 16,
            0x1010 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
        X64ContinuationEventV1 Event{};
        Event.Kind = NMI ? X64ContinuationEventKind::NMI
                         : X64ContinuationEventKind::Interrupt;
        Event.Vector = Vector;
        X64ContinuationStateV6 Native;
        bool Handler = false;
        ASSERT_NO_FATAL_FAILURE(CPU.execute(RAM.Memory.front().Bytes, State,
                                            Controls.CR0, 1, Native, Handler,
                                            false, &Event));
        ASSERT_FALSE(Handler);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Context(
            *Memory, {.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst});
        Shadows Shadow;
        Producer P;
        P.Candidate = {uint8_t(Vector), uint8_t(Event.Kind)};
        for (unsigned Phase = 0; Phase != 3; ++Phase) {
          uint64_t Budget = 1, Polls = 1;
          EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Context),
                              &Budget, &Shadow, &Polls, Producer::peek,
                              Producer::accepted, &P),
                    Phase == 0   ? 7u
                    : Phase == 1 ? 6u
                                 : 9u);
          EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          EXPECT_EQ(Budget, Phase == 0 ? 1u : 0u);
          EXPECT_EQ(P.Acks, 1u);
          EXPECT_EQ(State.NMIBlocked, Phase == 0 && NMI ? 1 : 0);
          P.Ready = 0;
        }
        const auto &Actual = State.Core.Core.Core.Core,
                   &Want = Native.Core.Core.Core.Core;
        EXPECT_EQ(Actual.PC, Want.PC);
        EXPECT_EQ(Actual.RFlags, Want.RFlags);
        for (unsigned I = 0; I != 16; ++I)
          EXPECT_EQ(Actual.GPR[I], Want.GPR[I]);
        EXPECT_EQ(State.Core.CS.Selector, Native.Core.CS.Selector);
        EXPECT_EQ(State.Core.SS.Selector, Native.Core.SS.Selector);
        auto ActualRAM =
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
        auto NativeRAM = std::vector<uint8_t>(CPU.RAM, CPU.RAM + CPU.Size);
        std::fill(ActualRAM.begin() + 0x10000, ActualRAM.begin() + 0x14000, 0);
        std::fill(NativeRAM.begin() + 0x10000, NativeRAM.begin() + 0x14000, 0);
        EXPECT_EQ(ActualRAM, NativeRAM);
        ++Comparisons;
      }
  }
  RecordProperty("iret_roundtrip_cpu_comparisons", Comparisons);
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionTest, PagedReturnFaultHandlerRepairsMappingAndRetries) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  std::vector<uint8_t> Code(0x1012, 0x90);
  Code[0] = Code[0x1010] = 0x48;
  Code[1] = Code[0x1011] = 0xcf;
  const uint8_t HandlerCode[] = {0x48, 0xc7, 0x04, 0x25, 0xe8, 0x30,
                                 0x01, 0x00, 0x07, 0xd0, 0x01, 0x00,
                                 0x48, 0x83, 0xc4, 0x08};
  std::copy(std::begin(HandlerCode), std::end(HandlerCode),
            Code.begin() + 0x1000);
  unsigned Comparisons = 0;
  for (bool Far : {false, true})
    for (bool LLVM : {false, true}) {
      Code[1] = Far ? 0xcb : 0xcf;
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver,
                                      {0x1000, 0x2000, 0x200c, 0x2010},
                                      NEVERD_TEST_CLANG, 0x1000, &Scheduled));
      reset(Code);
      auto &Core = State.Core.Core.Core.Core;
      Core.GPR[4] = 0x1d000;
      State.NMIBlocked = 1;
      for (unsigned I = 0; I != 5; ++I) {
        const uint64_t Frame[] = {0x1800, 8, 0x246, 0xe800, 16};
        put(0x1d000 + I * 8, Frame[I]);
      }
      put(0x130e8, 0);
      put(0x8024, 0x1c000);
      put(0x60e0, 0x2000 | (uint64_t(8) << 16) | (uint64_t(1) << 32) |
                      (uint64_t(0x8e) << 40));
      X64ContinuationStateV6 Native;
      bool Handler = false;
      ASSERT_NO_FATAL_FAILURE(CPU.execute(RAM.Memory.front().Bytes, State,
                                          Controls.CR0, 2, Native, Handler));
      ASSERT_FALSE(Handler);
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(
          *Memory,
          {.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst,
           .FarReturnProfile = X64PagedFarReturnProfile::SelectorFirst});
      for (unsigned Phase = 0; Phase != 3; ++Phase) {
        uint64_t Budget = Phase == 1 ? 3 : 1;
        EXPECT_EQ(
            Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
            Phase == 0 ? 7u : 6u);
        EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
        EXPECT_EQ(Budget, 0u);
        EXPECT_EQ(Core.PC, Phase == 0   ? 0x2000u
                           : Phase == 1 ? 0x1000u
                                        : 0x1800u);
        EXPECT_EQ(State.NMIBlocked, Far && Phase == 0 ? 1 : 0);
      }
      const auto &Want = Native.Core.Core.Core.Core;
      EXPECT_EQ(Core.PC, Want.PC);
      EXPECT_EQ(Core.RFlags, Want.RFlags);
      for (unsigned I = 0; I != 16; ++I)
        EXPECT_EQ(Core.GPR[I], Want.GPR[I]);
      EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
      auto ActualRAM =
          Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
      auto NativeRAM = std::vector<uint8_t>(CPU.RAM, CPU.RAM + CPU.Size);
      std::fill(ActualRAM.begin() + 0x10000, ActualRAM.begin() + 0x14000, 0);
      std::fill(NativeRAM.begin() + 0x10000, NativeRAM.begin() + 0x14000, 0);
      EXPECT_EQ(ActualRAM, NativeRAM);
      ++Comparisons;
    }
  RecordProperty("return_retry_cpu_comparisons", Comparisons);
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionTest,
       PagedIretDistinguishesAdmissionBackingAndShutdown) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0xcf};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                    NEVERD_TEST_CLANG, 0x1000, &Scheduled));
    for (unsigned Case = 0; Case != 7; ++Case) {
      SCOPED_TRACE(Case);
      reset(Code);
      auto &Core = State.Core.Core.Core.Core;
      Core.GPR[4] = 0x1d000;
      State.NMIBlocked = 1;
      for (unsigned I = 0; I != 5; ++I) {
        const uint64_t Frame[] = {0x1800, 8, 0x246, 0xe800, 16};
        put(0x1d000 + I * 8, Frame[I]);
      }
      X64PagedExecutionOptions Options;
      if (Case)
        Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
      if (Case == 1)
        RAM.Memory.front().Bytes[0x1000] = 0x90;
      if (Case == 2)
        put(0x13008, 0);
      if (Case == 3)
        put(0x130e8, 0x50007);
      if (Case == 4)
        put(0x13028, 0x50007);
      if (Case >= 5) {
        put(0x1d008, 0);
        if (Case == 5)
          put(0x13030, 0x50007);
        else
          State.IDTR.Limit = 0;
      }
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, Options);
      uint64_t Budget = 1;
      const auto Exit =
          Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
      EXPECT_EQ(Exit, Case == 0   ? 5u
                      : Case == 1 ? 20u
                      : Case == 2 ? 7u
                      : Case == 6 ? 19u
                                  : 18u);
      EXPECT_EQ(Budget, Case < 2 ? 1u : 0u);
      EXPECT_EQ(State.NMIBlocked, Case < 3 ? 1 : 0);
      EXPECT_EQ(Context.lastResult().InstructionAttempted, Case >= 2);
      if (Case == 0)
        EXPECT_NE(Context.lastError().find("bound frame-access order"),
                  std::string::npos);
      else
        EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
      if (Case == 3 || Case == 4)
        EXPECT_EQ(Core.PC, 0x1000u);
      if (Case < 2) {
        EXPECT_EQ(Core.GPR[4], 0x1d000u);
        EXPECT_EQ(Core.PC, 0x1000u);
      }
    }
  }
}
#endif
