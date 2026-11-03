//===- X86LegacyEventNativeTests.inc - Independent legacy IDT oracle ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare original INT/UD2 and injected IRQ/NMI entry with KVM snapshots.
//===----------------------------------------------------------------------===//

class X86LegacyEventNativeTest : public X86PagingNativeTest {
protected:
  unsigned Comparisons = 0;

  void compare(unsigned CPL, unsigned TargetCPL, bool Gate32, bool Code32,
               bool Stack32, bool TSS32, unsigned EventKind, unsigned Case,
               uint32_t ExtraFlags = 0, bool SameRingFaults = false) {
    using namespace neverd;
    SCOPED_TRACE(testing::Message()
                 << "cpl=" << CPL << " target=" << TargetCPL << " gate32="
                 << Gate32 << " code32=" << Code32 << " stack32=" << Stack32
                 << " tss32=" << TSS32 << " event=" << EventKind
                 << " case=" << Case << " extra_flags=" << ExtraFlags
                 << " same_ring_faults=" << SameRingFaults);
    const bool Legacy = GetParam() == 2;
    const unsigned PTWidth = Legacy ? 4 : 8;
    const uint64_t PT = Legacy ? 0x11000 : 0x12000;
    constexpr uint32_t GDT = 0x7000, IDT = 0x6000;
    const uint32_t TSS = Case == 32   ? 0xa000 - (TSS32 ? 8 : 4)
                         : Case == 33 ? 0xa000 - (TSS32 ? 10 : 6)
                                      : 0x5800;
    constexpr uint32_t Target = 0x3000, Stop = 0x4000, VectorRecord = 0x4800;
    std::memset(RAM, 0, MemorySize);
    put(0x10000, Legacy ? 0x11007 : 0x11001, PTWidth);
    if (!Legacy)
      put(0x11000, 0x12007);
    for (unsigned I = 0; I != MemorySize / 4096; ++I)
      put(PT + I * PTWidth, I * 4096 | 7, PTWidth);
    const auto PTE = [&](uint32_t Address) {
      return PT + (Address >> 12) * PTWidth;
    };
    const auto Clear = [&](uint32_t Address, uint64_t Bits) {
      put(PTE(Address), get(PTE(Address), PTWidth) & ~Bits, PTWidth);
    };
    put(GDT + 8, 0x00cf9b000000ffffULL);
    put(GDT + 16, 0x00cf93000000ffffULL);
    put(GDT + 24, 0x00cffb000000ffffULL);
    put(GDT + 32, 0x00cff3000000ffffULL);
    uint16_t Selector = 64 | (Case == 28 ? 3 : TargetCPL);
    uint64_t CodeDescriptor =
        (Code32 ? 0x00cf9a000000ffffULL : 0x008f9a000000ffffULL) |
        (uint64_t(TargetCPL) << 45);
    uint64_t StackDescriptor =
        (Stack32 ? 0x00cf92000000ffffULL : 0x000092000000ffffULL) |
        (uint64_t(TargetCPL) << 45);
    uint32_t NewSP = Case == 23   ? 0xf004
                     : Case == 35 ? 0xabcde000
                     : Case == 36 ? 0xe003
                                  : 0xe000;
    uint16_t NewSS = 80 | TargetCPL;
    uint32_t CodeAddress = GDT + 64;
    uint32_t IDTBase = IDT;
    uint8_t Vector = EventKind == 1 ? 6 : EventKind == 2 ? 2 : 64;
    if (Case == 29 && (EventKind == 0 || EventKind == 3))
      Vector = 14;
    uint32_t TargetIP = Target;
    uint8_t GateAccess = (Gate32 ? 0xee : 0xe6) | (Case == 27 ? 1 : 0);
    if (Case == 1)
      CodeDescriptor |= uint64_t{4} << 40;
    if (Case == 2)
      Selector = 0;
    if (Case == 3)
      CodeDescriptor &= ~(uint64_t{1} << 47);
    if (Case == 4)
      CodeDescriptor &= ~(uint64_t{8} << 40);
    if (Case == 5)
      CodeDescriptor =
          (CodeDescriptor & ~(uint64_t{3} << 45)) | (uint64_t{3} << 45);
    if (Case == 8)
      GateAccess &= ~0x80;
    if (Case == 9)
      GateAccess = 0xf0;
    if (Case == 10)
      GateAccess &= ~0x60;
    if (Case == 12)
      NewSS = 0;
    if (Case == 13)
      NewSS ^= 3;
    if (Case == 14)
      StackDescriptor &= ~(uint64_t{1} << 47);
    if (Case == 15)
      StackDescriptor &= ~(uint64_t{2} << 40);
    if (Case == 16)
      StackDescriptor =
          (StackDescriptor & ~uint64_t{0x008f00000000ffffULL}) | (NewSP - 24);
    if (Case == 17)
      CodeDescriptor &= ~uint64_t{0x008f00000000ffffULL};
    if (Case == 26)
      CodeDescriptor |= uint64_t{1} << 40;
    if (Case == 30) {
      Selector |= 4;
      CodeAddress = 0xc040;
    }
    if (Case == 31)
      TargetIP = 0xabcd3000;
    put(CodeAddress, CodeDescriptor);
    put(GDT + 80, StackDescriptor);
    for (unsigned Ring = 0; Ring != 3; ++Ring) {
      const unsigned Offset = TSS32 ? 4 + Ring * 8 : 2 + Ring * 4;
      put(TSS + Offset, Ring == TargetCPL ? NewSP : 0xb000, TSS32 ? 4 : 2);
      put(TSS + Offset + (TSS32 ? 4 : 2), Ring == TargetCPL ? NewSS : 16, 2);
    }
    // Each entry records its own vector with a register-preserving MOV, then
    // jumps to a common hardware breakpoint. This distinguishes faults that
    // have identical error codes and frames, without taking another exception.
    const auto Handler = [&](uint32_t Address, uint8_t V, bool Wide) {
      uint32_t Cursor = Address;
      if (!Wide) {
        RAM[Cursor++] = 0x66;
        RAM[Cursor++] = 0x67;
      }
      RAM[Cursor++] = 0xc7;
      RAM[Cursor++] = 0x05;
      put(Cursor, VectorRecord, 4);
      Cursor += 4;
      put(Cursor, V, 4);
      Cursor += 4;
      if (!Wide)
        RAM[Cursor++] = 0x66;
      RAM[Cursor++] = 0xe9;
      put(Cursor, Stop - (Cursor + 4), 4);
    };
    for (unsigned V = 0; V != 256; ++V) {
      const uint32_t Entry = 0x2000 + V * 16;
      put(IDT + V * 8, Entry | (uint64_t(SameRingFaults ? 24 | CPL : 8) << 16) |
                           (uint64_t{0xee} << 40));
      Handler(Entry, V, true);
    }
    Handler(Target, Vector, Code32);
    if (Case == 24)
      IDTBase = 0xaffc - Vector * 8;
    const uint64_t Gate = (TargetIP & 0xffff) | (uint64_t(Selector) << 16) |
                          (uint64_t(GateAccess) << 40) |
                          (uint64_t(TargetIP & 0xffff0000) << 32);
    put(IDTBase + Vector * 8, Gate);
    if (Case == 18)
      Clear(CodeAddress, 1);
    if (Case == 19 || Case == 20 || Case == 26)
      Clear(CodeAddress, 2);
    if (Case == 20) {
      put(CodeAddress, CodeDescriptor | (uint64_t{1} << 40));
    }
    if (Case == 21)
      Clear(TSS, 1);
    if (Case == 22)
      Clear(NewSP - 1, 1);
    if (Case == 23)
      Clear(0xefff, 1);
    if (Case == 24)
      Clear(0xb000, 1);
    if (Case == 32 || Case == 33)
      Clear(0xa000, 1);
    std::fill(RAM + 0xd000, RAM + 0xd100, 0xa5);
    std::fill(RAM + 0xdf00, RAM + 0xe000, 0xa5);
    std::fill(RAM + 0xef00, RAM + 0xf100, 0xa5);
    kvm_sregs Special = Initial;
    Special.cr0 = 0x80010011;
    Special.cr3 = 0x10000;
    Special.cr4 = Legacy ? 0 : 0x20;
    Special.efer = Legacy ? 0 : 0x800;
    Special.cr2 = 0x76543210;
    Special.cs = segment(CPL ? 24 | CPL : 8, true, false, CPL);
    Special.cs.db = Code32;
    Special.ds = Special.es = Special.ss = Special.fs = Special.gs =
        segment(32 | CPL, false, false, CPL);
    Special.ss.db = Stack32;
    if (!Stack32) {
      Special.ss.g = 0;
      Special.ss.limit = 0xffff;
    }
    Special.gdt.base = GDT;
    Special.gdt.limit = Case == 6 ? 63 : 0xff;
    Special.idt.base = IDTBase;
    Special.idt.limit = Case == 7 ? Vector * 8 + 6 : 0x7ff;
    Special.ldt = {};
    Special.ldt.base = 0xc000;
    Special.ldt.limit = 0xff;
    Special.ldt.selector = 48;
    Special.ldt.type = 2;
    Special.ldt.present = Case == 30;
    Special.ldt.unusable = Case != 30;
    Special.tr = {};
    Special.tr.base = TSS;
    Special.tr.limit = Case == 11 ? 0 : 0x67;
    Special.tr.selector = 40;
    Special.tr.type = TSS32 ? 11 : 3;
    Special.tr.present = 1;
    kvm_regs Registers{};
    Registers.rip = Code;
    Registers.rsp = Case == 34 ? 0xabcdd100 : Case == 36 ? 0xd103 : 0xd100;
    Registers.rax = 0x11223344;
    Registers.rflags = 0x2d7 | ExtraFlags;
    std::vector<uint8_t> Instruction = EventKind == 1
                                           ? std::vector<uint8_t>{0x0f, 0x0b}
                                           : std::vector<uint8_t>{0xcd, Vector};
    std::copy(Instruction.begin(), Instruction.end(), RAM + Code);
    RAM[Stop] = 0xf4;
    X86LegacyExecutionState State;
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
    State.Core.CPL = CPL;
    State.Core.GPR[0] = Registers.rax;
    State.Core.GPR[4] = Registers.rsp;
    for (unsigned I = 0; I != Segments.size(); ++I)
      State.Core.Segments[I] = Cache(*Segments[I]);
    State.Core.GDTRBase = GDT;
    State.Core.GDTRLimit = Special.gdt.limit;
    State.Core.LDTR = Cache(Special.ldt);
    State.TR = Cache(Special.tr);
    State.IDTRBase = IDTBase;
    State.IDTRLimit = Special.idt.limit;
    State.EIP = Registers.rip;
    State.EFlags = Registers.rflags;
    State.CR2 = Special.cr2;
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
        false};
    auto Expected =
        EventKind <= 1
            ? executeX86LegacyInstruction(State, *Memory, Instruction, Options)
            : deliverX86LegacyEvent(State, *Memory,
                                    {uint32_t(Code), 0, 0, Vector,
                                     EventKind == 2
                                         ? X86LegacyEventKind::NMI
                                         : X86LegacyEventKind::Interrupt},
                                    Options);
    ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
    auto Flush = Special;
    Flush.cr3 = 0x90000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0) << errno;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0) << errno;
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
    Debug.arch.debugreg[0] = Stop;
    Debug.arch.debugreg[7] = 0x401;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    if (EventKind == 2)
      ASSERT_EQ(ioctl(CPU, KVM_NMI, 0), 0);
    if (EventKind == 3) {
      kvm_interrupt IRQ{Vector};
      ASSERT_EQ(ioctl(CPU, KVM_INTERRUPT, &IRQ), 0);
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
      ASSERT_EQ(Expected->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_EQ(Registers.rip, Stop);
      const auto Delivered = get(VectorRecord, 4);
      EXPECT_EQ(Delivered, Expected->DeliveredVector);
      const uint32_t Entry = Delivered == Vector
                                 ? (Gate32 ? TargetIP : uint16_t(TargetIP))
                                 : 0x2000 + Delivered * 16;
      EXPECT_EQ(Entry, State.EIP);
      const std::array<uint32_t, 8> GPR{
          uint32_t(Registers.rax), uint32_t(Registers.rcx),
          uint32_t(Registers.rdx), uint32_t(Registers.rbx),
          uint32_t(Registers.rsp), uint32_t(Registers.rbp),
          uint32_t(Registers.rsi), uint32_t(Registers.rdi)};
      EXPECT_EQ(GPR, State.Core.GPR);
      EXPECT_EQ(bool(FinalEvents.nmi.masked), State.NMIBlocked);
      EXPECT_EQ(Registers.rflags & ~uint64_t{0x10000}, State.EFlags);
      for (unsigned I = 0; I != Segments.size(); ++I) {
        const auto Native = Cache(*Segments[I]);
        const auto &Model = State.Core.Segments[I];
        EXPECT_EQ(Native.Selector, Model.Selector) << "segment " << I;
        EXPECT_EQ(Native.Base, Model.Base) << "segment " << I;
        EXPECT_EQ(Native.Limit, Model.Limit) << "segment " << I;
        EXPECT_EQ(Native.Attributes, Model.Attributes) << "segment " << I;
        EXPECT_EQ(Native.Unusable, Model.Unusable) << "segment " << I;
      }
    }
    for (unsigned Begin : {0xb000u - 32, 0xd000u, 0xd100u, 0xdf00u, 0xe000u,
                           0xef00u, 0xf000u, GDT, 0xc000u}) {
      for (unsigned I = 0; I != 256; ++I) {
        const auto Got = Memory->physicalMemory().loadScalar(
            Begin + I, GuestScalarWidth::I8);
        ASSERT_EQ(Got.Status, GuestMemoryAccessStatus::Completed);
        EXPECT_EQ(RAM[Begin + I], Got.Value)
            << "address " << std::hex << Begin + I;
      }
    }
    ++Comparisons;
  }
};

TEST_P(X86LegacyEventNativeTest, OriginalGateWidthsPrivilegesAndFrames) {
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (unsigned TargetCPL = 0; TargetCPL <= CPL; ++TargetCPL)
      for (bool Gate32 : {false, true})
        for (bool Code32 : {false, true})
          for (bool Stack32 : {false, true})
            for (bool TSS32 : {false, true})
              for (unsigned Event = 0; Event != 4; ++Event)
                for (unsigned Case : {0u, 1u, 27u, 28u, 29u, 30u})
                  ASSERT_NO_FATAL_FAILURE(compare(CPL, TargetCPL, Gate32,
                                                  Code32, Stack32, TSS32, Event,
                                                  Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyEventNativeTest, OriginalDescriptorAndDeliveryFaults) {
  for (unsigned CPL : {0u, 3u})
    for (bool Gate32 : {false, true})
      for (unsigned Event = 0; Event != 4; ++Event)
        for (unsigned Case = 2; Case != 27; ++Case) {
          if (Case == 25)
            continue;
          ASSERT_NO_FATAL_FAILURE(
              compare(CPL, 0, Gate32, true, true, true, Event, Case));
          if (CPL == 3)
            ASSERT_NO_FATAL_FAILURE(compare(CPL, 0, Gate32, true, true, true,
                                            Event, Case, 0, true));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyEventNativeTest, OriginalTSSAccessExtent) {
  for (bool Gate32 : {false, true})
    for (bool TSS32 : {false, true})
      for (unsigned Event = 0; Event != 4; ++Event)
        for (unsigned Case : {21u, 32u, 33u})
          ASSERT_NO_FATAL_FAILURE(
              compare(3, 0, Gate32, true, true, TSS32, Event, Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyEventNativeTest, OriginalEntryFlagsAndStackState) {
  for (unsigned CPL : {0u, 3u})
    for (bool Gate32 : {false, true})
      for (bool Stack32 : {false, true})
        for (bool TSS32 : {false, true})
          for (unsigned Event = 0; Event != 4; ++Event)
            for (uint32_t Flags : {0u, 0x10000u, 0x244c00u})
              for (unsigned Case : {0u, 19u, 22u, 23u, 31u, 34u, 35u, 36u}) {
                if (Case == 31 && Gate32)
                  continue;
                ASSERT_NO_FATAL_FAILURE(compare(CPL, 0, Gate32, true, Stack32,
                                                TSS32, Event, Case, Flags));
              }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyEvents, X86LegacyEventNativeTest,
                         testing::Values(2u, 3u));
