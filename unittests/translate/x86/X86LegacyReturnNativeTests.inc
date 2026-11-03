//===- X86LegacyReturnNativeTests.inc - Original protected returns --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent KVM IRET/RETF oracle, including failed return restart frames.
//===----------------------------------------------------------------------===//

class X86LegacyReturnNativeTest : public X86PagingNativeTest {
protected:
  unsigned Comparisons = 0;
  unsigned VMXNMIRecoveries = 0;

  void compare(unsigned CPL, unsigned TargetCPL, bool Code32, bool Operand32,
               bool Stack32, bool TargetStack32, unsigned Kind, unsigned Case,
               uint32_t OldFlags = 0x202, uint32_t SavedFlags = 0x202) {
    using namespace neverd;
    SCOPED_TRACE(testing::Message()
                 << "cpl=" << CPL << " target=" << TargetCPL
                 << " code32=" << Code32 << " operand32=" << Operand32
                 << " stack32=" << Stack32
                 << " target_stack32=" << TargetStack32 << " kind=" << Kind
                 << " case=" << Case << " old_flags=" << std::hex << OldFlags
                 << " saved_flags=" << SavedFlags);
    const bool Legacy = GetParam() == 2;
    const unsigned PTWidth = Legacy ? 4 : 8;
    const uint64_t PT = Legacy ? 0x11000 : 0x12000;
    constexpr uint32_t GDT = 0x7000, IDT = 0x6000, Target = 0x4000;
    constexpr uint32_t Stop = 0x5000, VectorRecord = 0x4800;
    const unsigned Bytes = Operand32 ? 4 : 2;
    const unsigned Discard = Kind == 2 ? 7 : 0;
    const bool RoundTrip = Case >= 31 && Case <= 33;
    std::memset(RAM, 0, MemorySize);
    put(0x10000, Legacy ? 0x11007 : 0x11001, PTWidth);
    if (!Legacy)
      put(0x11000, 0x12007);
    for (unsigned I = 0; I != MemorySize / 4096; ++I)
      put(PT + I * PTWidth, I * 4096 | 7, PTWidth);
    const auto Clear = [&](uint32_t Address, uint64_t Bits) {
      const uint32_t PTE = PT + (Address >> 12) * PTWidth;
      put(PTE, get(PTE, PTWidth) & ~Bits, PTWidth);
    };
    put(GDT + 8, 0x00cf9b000000ffffULL);
    put(GDT + 16, 0x00cf93000000ffffULL);
    put(GDT + 24, (Code32 ? 0x00cf9b000000ffffULL : 0x008f9b000000ffffULL) |
                      (uint64_t(CPL) << 45));
    put(GDT + 32, (Stack32 ? 0x00cf93000000ffffULL : 0x000093000000ffffULL) |
                      (uint64_t(CPL) << 45));
    uint16_t CS = 64 | TargetCPL, SS = 80 | TargetCPL;
    uint64_t CodeDescriptor =
        0x00cf9a000000ffffULL | (uint64_t(TargetCPL) << 45);
    uint64_t StackDescriptor =
        (TargetStack32 ? 0x00cf92000000ffffULL : 0x000092000000ffffULL) |
        (uint64_t(TargetCPL) << 45);
    uint32_t IP = Target, SP = 0xd100, NewSP = 0xabcdf000;
    uint32_t CodeAddress = GDT + 64;
    if (Case == 1)
      CodeDescriptor |= uint64_t{4} << 40;
    if (Case == 2)
      CS = 0;
    if (Case == 3)
      CodeDescriptor &= ~(uint64_t{1} << 47);
    if (Case == 4)
      CodeDescriptor &= ~(uint64_t{8} << 40);
    if (Case == 5)
      CodeDescriptor ^= uint64_t{1} << 45;
    if (Case == 6)
      CS = 0x100 | TargetCPL;
    if (Case == 7)
      SS = 0;
    if (Case == 8)
      SS ^= 3;
    if (Case == 9)
      StackDescriptor &= ~(uint64_t{1} << 47);
    if (Case == 10)
      StackDescriptor &= ~(uint64_t{2} << 40);
    if (Case == 11)
      SS = 0x100 | TargetCPL;
    if (Case == 12)
      CodeDescriptor &= ~uint64_t{0x008f00000000ffffULL};
    if (Case == 13) {
      CS |= 4;
      CodeAddress = 0xc040;
    }
    if (Case == 14 || Case == 26)
      CodeDescriptor &= ~(uint64_t{1} << 54); // 16-bit destination CS.
    if (Case == 17)
      CodeDescriptor |= uint64_t{1} << 40;
    if (Case >= 18 && Case <= 22)
      SP = 0xd000 - (Case - 18) * Bytes;
    if (Case == 23)
      SP = 0xabcdd100;
    if (Case == 24 || Case == 25)
      SP = 0xd101;
    if (Case == 26)
      IP = 0x14000;
    if (Case == 27)
      SP = 0xfffc;
    if (Case == 28)
      NewSP = UINT32_MAX - 3;
    if (Case == 29)
      StackDescriptor =
          (StackDescriptor & ~uint64_t{0x008f00000000ffffULL}) | 0x1234;
    if (Case == 30)
      StackDescriptor |= uint64_t{4} << 40; // Expand-down target.
    if (Case == 34)
      CodeDescriptor |= uint64_t{0x20000} << 16;
    if (Case == 35)
      SP = CodeAddress - (Kind == 0 ? 3 : 2) * Bytes - Discard;
    if (Case == 36)
      SP = 0xd000 - (Kind == 0 ? 3 : 2) * Bytes - Discard;
    put(CodeAddress, CodeDescriptor);
    put(GDT + 80, StackDescriptor);
    put(0x5804, 0xb000, 4);
    put(0x5808, 16, 2);
    for (unsigned V = 0; V != 256; ++V) {
      const uint32_t Entry = 0x2000 + V * 16;
      put(IDT + V * 8, Entry | (uint64_t{8} << 16) | (uint64_t{0xee} << 40));
      RAM[Entry] = 0xc7;
      RAM[Entry + 1] = 0x05;
      put(Entry + 2, VectorRecord, 4);
      put(Entry + 6, V, 4);
      RAM[Entry + 10] = 0xe9;
      put(Entry + 11, Stop - (Entry + 15), 4);
    }
    const uint32_t StackMask = Stack32 ? UINT32_MAX : UINT16_MAX;
    const auto Frame = [&](unsigned Offset, uint32_t V) {
      put((SP + Offset) & StackMask, V, Bytes);
    };
    Frame(0, IP);
    Frame(Bytes, CS | (Operand32 ? 0xa5a50000u : 0));
    if (Kind == 0)
      Frame(2 * Bytes, SavedFlags);
    const unsigned OuterOffset = (Kind == 0 ? 3 : 2) * Bytes + Discard;
    Frame(OuterOffset, NewSP);
    Frame(OuterOffset + Bytes, SS | (Operand32 ? 0x5a5a0000u : 0));
    if (Case == 35)
      put(CodeAddress, CodeDescriptor);
    if (Case == 15)
      Clear(CodeAddress, 1);
    if (Case == 16 || Case == 17)
      Clear(CodeAddress, 2);
    if ((Case >= 18 && Case <= 22) || Case == 36)
      Clear(0xd000, 1);
    if (Case == 25)
      Clear(SP, 1);
    std::vector<uint8_t> Instruction;
    if ((RoundTrip || Code32) != Operand32)
      Instruction.push_back(0x66);
    Instruction.push_back(Kind == 0 ? 0xcf : Kind == 1 ? 0xcb : 0xca);
    if (Kind == 2) {
      Instruction.push_back(Discard);
      Instruction.push_back(0);
    }
    std::copy(Instruction.begin(), Instruction.end(), RAM + Code);
    const uint32_t TargetLinear =
        (Operand32 ? IP : uint16_t(IP)) + (Case == 34 ? 0x20000 : 0);
    RAM[TargetLinear] = RAM[TargetLinear + 1] = 0x90;
    if (RoundTrip) {
      const unsigned Vector = Case == 32 ? 2 : 64;
      put(IDT + Vector * 8, Code | (uint64_t{8} << 16) |
                                (uint64_t(Operand32 ? 0xee : 0xe6) << 40));
      RAM[Target - 2] = 0xcd;
      RAM[Target - 1] = 64;
    }
    RAM[Stop] = 0xf4;
    kvm_sregs Special = Initial;
    Special.cr0 = 0x80050011;
    Special.cr3 = 0x10000;
    Special.cr4 = Legacy ? 0 : 0x20;
    Special.efer = Legacy ? 0 : 0x800;
    Special.cr2 = 0x76543210;
    Special.cs = segment(24 | CPL, true, false, CPL);
    Special.cs.db = Code32;
    Special.ds = Special.es = Special.ss = Special.fs = Special.gs =
        segment(32 | CPL, false, false, CPL);
    Special.ss.db = Stack32;
    if (!Stack32) {
      Special.ss.g = 0;
      Special.ss.limit = 0xffff;
    }
    Special.gdt.base = GDT;
    Special.gdt.limit = 0xff;
    Special.idt.base = IDT;
    Special.idt.limit = 0x7ff;
    Special.ldt = {};
    Special.ldt.base = 0xc000;
    Special.ldt.limit = 0xff;
    Special.ldt.selector = 48;
    Special.ldt.type = 2;
    Special.ldt.present = Case == 13;
    Special.ldt.unusable = Case != 13;
    Special.tr = {};
    Special.tr.base = 0x5800;
    Special.tr.limit = 0x67;
    Special.tr.selector = 40;
    Special.tr.type = 11;
    Special.tr.present = 1;
    kvm_regs Registers{};
    Registers.rip = RoundTrip ? Target - (Case == 31 ? 2 : 0) : Code;
    Registers.rsp = SP;
    Registers.rax = 0x11223344;
    Registers.rcx = 0x22334455;
    Registers.rdx = 0x33445566;
    Registers.rbx = 0x44556677;
    Registers.rbp = 0x55667788;
    Registers.rsi = 0x66778899;
    Registers.rdi = 0x778899aa;
    Registers.rflags = OldFlags;
    const auto Cache = [](const kvm_segment &C) {
      return X86LegacySegmentCache{uint32_t(C.base), C.limit, C.selector,
                                   uint16_t(C.type | C.s << 4 | C.dpl << 5 |
                                            C.present << 7 | C.avl << 12 |
                                            C.l << 13 | C.db << 14 | C.g << 15),
                                   bool(C.unusable)};
    };
    std::array<kvm_segment *, 6> Segments{&Special.es, &Special.cs,
                                          &Special.ss, &Special.ds,
                                          &Special.fs, &Special.gs};
    X86LegacyExecutionState State;
    State.Core.CPL = CPL;
    State.Core.GPR = {uint32_t(Registers.rax),
                      uint32_t(Registers.rcx),
                      uint32_t(Registers.rdx),
                      uint32_t(Registers.rbx),
                      SP,
                      uint32_t(Registers.rbp),
                      uint32_t(Registers.rsi),
                      uint32_t(Registers.rdi)};
    for (unsigned I = 0; I != Segments.size(); ++I)
      State.Core.Segments[I] = Cache(*Segments[I]);
    State.Core.GDTRBase = GDT;
    State.Core.GDTRLimit = Special.gdt.limit;
    State.Core.LDTR = Cache(Special.ldt);
    State.TR = Cache(Special.tr);
    State.IDTRBase = IDT;
    State.IDTRLimit = Special.idt.limit;
    State.EIP = Registers.rip;
    State.EFlags = Registers.rflags;
    State.CR2 = Special.cr2;
    State.NMIBlocked = !RoundTrip;
    X86PagingState Controls{.CR0 = Special.cr0,
                            .CR3 = Special.cr3,
                            .CR4 = Special.cr4,
                            .EFER = Special.efer,
                            .RFlags = Registers.rflags,
                            .PDPTE = {0x11001, 0, 0, 0}};
    auto Logical =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Logical.Memory.push_back({0,
                              MemoryPermission::Read | MemoryPermission::Write,
                              0, std::vector<uint8_t>(RAM, RAM + MemorySize)});
    auto Memory =
        llvm::cantFail(X86PagedMemory::create(Logical, Features, Controls));
    X86LegacyExecutionOptions Options{
        {X86LegacySegmentProfile::WordStackOffsetFirst,
         X86AlignmentPriority::BeforePaging, X86FullSegmentLimitChoice::Fault},
        X86LegacyEventProfile::OrderedFrame,
        false,
        X86LegacyReturnProfile::OrderedFrame};
    if (RoundTrip) {
      auto Entered =
          Case == 31
              ? executeX86LegacyInstruction(State, *Memory, {0xcd, 64}, Options)
              : deliverX86LegacyEvent(
                    State, *Memory,
                    {Target, 0, 0, uint8_t(Case == 32 ? 2 : 64),
                     Case == 32 ? X86LegacyEventKind::NMI
                                : X86LegacyEventKind::Interrupt},
                    Options);
      ASSERT_TRUE(bool(Entered)) << llvm::toString(Entered.takeError());
      ASSERT_EQ(Entered->Status, X86LegacyExecutionStatus::EventDelivered);
      ASSERT_EQ(State.EIP, Code);
    }
    auto Expected =
        executeX86LegacyInstruction(State, *Memory, Instruction, Options);
    ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
    auto Flush = Special;
    Flush.cr3 = 0x90000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0) << errno;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0) << errno;
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    Events.nmi.masked = !RoundTrip;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
    Debug.arch.debugreg[0] = TargetLinear;
    Debug.arch.debugreg[1] = TargetLinear + 1;
    Debug.arch.debugreg[2] = Stop;
    Debug.arch.debugreg[7] = 0x415;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    if (Case == 32)
      ASSERT_EQ(ioctl(CPU, KVM_NMI, 0), 0);
    if (Case == 33) {
      kvm_interrupt IRQ{64};
      ASSERT_EQ(ioctl(CPU, KVM_INTERRUPT, &IRQ), 0);
    }
    for (unsigned Attempt = 0; Attempt != (Case == 36 ? 2u : 1u); ++Attempt) {
      SCOPED_TRACE(testing::Message() << "attempt=" << Attempt);
      if (Attempt) {
        ASSERT_EQ(Expected->DeliveredVector, 14);
        const uint32_t PTE = PT + 13 * PTWidth;
        put(PTE, get(PTE, PTWidth) | 1, PTWidth);
        const auto PW =
            PTWidth == 4 ? GuestScalarWidth::I32 : GuestScalarWidth::I64;
        auto Old = Memory->physicalMemory().loadScalar(PTE, PW);
        ASSERT_EQ(Old.Status, GuestMemoryAccessStatus::Completed);
        ASSERT_EQ(
            Memory->physicalMemory().storeScalar(PTE, PW, Old.Value | 1).Status,
            GuestMemoryAccessStatus::Completed);
        // The handler boundary supplies the repaired PTE and consumes the
        // error code. Original IRETD then returns to the faulting return and
        // retries its original bytes, with no second frame-pop publication.
        State.Core.GPR[4] += 4;
        Registers.rsp += 4;
        State.EIP = Stop;
        Registers.rflags = State.EFlags;
        put(Stop, 0xcf, 1);
        ASSERT_EQ(Memory->physicalMemory()
                      .storeScalar(Stop, GuestScalarWidth::I8, 0xcf)
                      .Status,
                  GuestMemoryAccessStatus::Completed);
        Expected = executeX86LegacyInstruction(State, *Memory, {0xcf}, Options);
        ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
        ASSERT_TRUE(Expected->InstructionCompleted);
        ASSERT_EQ(State.EIP, Code);
        Expected =
            executeX86LegacyInstruction(State, *Memory, Instruction, Options);
        ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
        ASSERT_TRUE(Expected->InstructionCompleted);
        Flush = Special;
        Flush.cr3 = 0x90000;
        ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0);
        ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0);
        ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
        Debug.arch.debugreg[7] = 0x405;
        ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
      }
      ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
      const bool Shutdown =
          Expected->Status == X86LegacyExecutionStatus::Shutdown;
      ASSERT_EQ(Run->exit_reason,
                unsigned(Shutdown ? KVM_EXIT_SHUTDOWN : KVM_EXIT_DEBUG));
      ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
      ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
      kvm_vcpu_events FinalEvents{};
      ASSERT_EQ(ioctl(CPU, KVM_GET_VCPU_EVENTS, &FinalEvents), 0);
      EXPECT_EQ(Special.cr2, State.CR2);
      if (!Shutdown) {
        if (Expected->InstructionCompleted) {
          ASSERT_EQ(Expected->Status, X86LegacyExecutionStatus::Completed);
          // RF suppresses only the first execution breakpoint. The two stops
          // witness restored RF without trusting the debugger-modified flags.
          EXPECT_EQ(Registers.rip,
                    State.EIP + ((State.EFlags & 0x10000) ? 1 : 0));
        } else {
          ASSERT_EQ(Expected->Status, X86LegacyExecutionStatus::EventDelivered);
          EXPECT_EQ(Registers.rip, Stop);
          EXPECT_EQ(get(VectorRecord, 4), Expected->DeliveredVector);
          EXPECT_EQ(State.EIP, 0x2000u + Expected->DeliveredVector * 16);
        }
        const std::array<uint32_t, 8> GPR{
            uint32_t(Registers.rax), uint32_t(Registers.rcx),
            uint32_t(Registers.rdx), uint32_t(Registers.rbx),
            uint32_t(Registers.rsp), uint32_t(Registers.rbp),
            uint32_t(Registers.rsi), uint32_t(Registers.rdi)};
        EXPECT_EQ(GPR, State.Core.GPR);
        if (Kind == 0 && Expected->DeliveredVector == 17 &&
            FinalEvents.nmi.masked && !State.NMIBlocked) {
          // Linux VMX intercepts #AC and restores NMI blocking on reinjection;
          // retain the architectural IRET rule and report that VMM discrepancy.
          ++VMXNMIRecoveries;
        } else {
          EXPECT_EQ(bool(FinalEvents.nmi.masked), State.NMIBlocked);
        }
        EXPECT_EQ(Registers.rflags & ~uint64_t{0x10000},
                  State.EFlags & ~uint32_t{0x10000});
        for (unsigned I = 0; I != Segments.size(); ++I) {
          const auto Native = Cache(*Segments[I]);
          const auto &Model = State.Core.Segments[I];
          EXPECT_EQ(Native.Selector, Model.Selector) << "segment " << I;
          EXPECT_EQ(Native.Unusable, Model.Unusable) << "segment " << I;
          if (!Model.Unusable) {
            EXPECT_EQ(Native.Base, Model.Base) << "segment " << I;
            EXPECT_EQ(Native.Limit, Model.Limit) << "segment " << I;
            EXPECT_EQ(Native.Attributes, Model.Attributes) << "segment " << I;
          }
        }
      }
      for (unsigned Begin : {0xb000u - 32, 0xcff0u, 0xd000u, 0xd0e0u, 0xdf00u,
                             0xfff0u, GDT, 0xc000u})
        for (unsigned I = 0; I != 128; ++I) {
          const auto Got = Memory->physicalMemory().loadScalar(
              Begin + I, GuestScalarWidth::I8);
          ASSERT_EQ(Got.Status, GuestMemoryAccessStatus::Completed);
          EXPECT_EQ(RAM[Begin + I], Got.Value)
              << "address " << std::hex << Begin + I;
        }
      ++Comparisons;
    }
  }
};

TEST_P(X86LegacyReturnNativeTest, WidthsPrivilegesAndDescriptors) {
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (unsigned TargetCPL = CPL; TargetCPL != 4; ++TargetCPL)
      for (bool Code32 : {false, true})
        for (bool Operand32 : {false, true})
          for (bool Stack32 : {false, true})
            for (bool TargetStack32 : {false, true})
              for (unsigned Kind : {0u, 1u, 2u})
                for (unsigned Case :
                     {0u, 1u, 13u, 14u, 26u, 28u, 29u, 30u, 34u})
                  ASSERT_NO_FATAL_FAILURE(compare(CPL, TargetCPL, Code32,
                                                  Operand32, Stack32,
                                                  TargetStack32, Kind, Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyReturnNativeTest, AccessedDescriptorAliasesAnOuterStackField) {
  for (unsigned Kind : {0u, 1u, 2u})
    ASSERT_NO_FATAL_FAILURE(compare(0, 3, true, true, true, true, Kind, 35));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyReturnNativeTest, PageRepairIRETAndRetryOriginalReturn) {
  for (bool Operand32 : {false, true})
    for (unsigned Kind : {0u, 1u, 2u})
      ASSERT_NO_FATAL_FAILURE(
          compare(0, 3, true, Operand32, true, true, Kind, 36));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyReturnNativeTest, ReturnFaultsAndCompetingFrameAccesses) {
  for (unsigned CPL : {0u, 3u})
    for (unsigned TargetCPL : {0u, 3u})
      for (bool Operand32 : {false, true})
        for (unsigned Kind : {0u, 1u, 2u})
          for (unsigned Case = 2; Case != 23; ++Case)
            ASSERT_NO_FATAL_FAILURE(compare(CPL, TargetCPL, true, Operand32,
                                            true, true, Kind, Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyReturnNativeTest, FlagsStackWidthsAndWrapping) {
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (unsigned TargetCPL : {CPL, 3u})
      for (bool Operand32 : {false, true})
        for (unsigned IOPL = 0; IOPL != 4; ++IOPL)
          for (unsigned Kind : {0u, 1u, 2u})
            for (unsigned Case : {0u, 23u, 27u})
              ASSERT_NO_FATAL_FAILURE(
                  compare(CPL, TargetCPL, true, Operand32, false, false, Kind,
                          Case, 0x390ed7 | (IOPL << 12), 0x3d7ed7));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyReturnNativeTest, OriginalINTIRQAndNMIReturnToInterruptedCode) {
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (bool Code32 : {false, true})
      for (bool Operand32 : {false, true})
        for (bool Stack32 : {false, true})
          for (unsigned Case : {31u, 32u, 33u})
            ASSERT_NO_FATAL_FAILURE(compare(CPL, CPL, Code32, Operand32,
                                            Stack32, Stack32, 0, Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyReturnNativeTest, AlignmentAndPageFaultPriority) {
  const char *Binding = std::getenv("NEVERD_TEST_LEGACY_ALIGNMENT");
  if (!Binding || std::strcmp(Binding, "before") != 0)
    GTEST_SKIP() << "Bind the qualified legacy return alignment priority";
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true})
      for (unsigned Kind : {0u, 1u, 2u})
        for (unsigned Case : {24u, 25u})
          ASSERT_NO_FATAL_FAILURE(compare(3, 3, Code32, Operand32, true, true,
                                          Kind, Case, 0x40202, 0x202));
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("vmx_ac_nmi_recoveries", VMXNMIRecoveries);
}

INSTANTIATE_TEST_SUITE_P(LegacyReturns, X86LegacyReturnNativeTest,
                         testing::Values(2u, 3u));
