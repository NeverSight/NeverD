//===- X86LegacyStepNativeTests.inc - Native legacy instruction sequences -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare fetch boundaries, original selector steps and IRQ/NMI shadows.
//===----------------------------------------------------------------------===//

class X86LegacyStepNativeTest : public X86PagingNativeTest {
protected:
  unsigned Comparisons = 0;

  void compare(bool Code32, unsigned Case) {
    using namespace neverd;
    SCOPED_TRACE(testing::Message() << "code32=" << Code32 << " case=" << Case);
    const bool Legacy = GetParam() == 2;
    const unsigned Width = Legacy ? 4 : 8;
    const uint32_t PT = Legacy ? 0x181000 : 0x182000;
    std::memset(RAM, 0, MemorySize);
    put(0x180000, Legacy ? 0x181007 : 0x181001, Width);
    if (!Legacy)
      put(0x181000, 0x182007);
    for (unsigned I = 0; I != MemorySize / 4096; ++I)
      put(PT + I * Width, I * 4096 | 7, Width);
    put(0x7008, 0x00cf9b000000ffffULL);
    put(0x7010, 0x00cf93000000ffffULL);
    for (unsigned V = 0; V != 256; ++V)
      put(0x6000 + V * 8,
          0x2800 | (uint64_t{8} << 16) | (uint64_t{0x8e} << 40));
    put(0xd000, 16, 2);
    put(0x9000, 0xb000, 4);
    put(0x9004, 16, 2);
    kvm_sregs Special = Initial;
    Special.cr0 = 0x80010011;
    Special.cr3 = 0x180000;
    Special.cr4 = Legacy ? 0 : 0x20;
    Special.efer = Legacy ? 0 : 0x800;
    Special.cr2 = 0x12345678;
    Special.cs = segment(8, true, false, 0);
    Special.cs.db = Code32;
    Special.cs.base = Case == 9 ? 0x80000 : 0;
    if (Case == 8) {
      Special.cs.g = 0;
      Special.cs.limit = 0xffff;
    }
    Special.ss = Special.ds = Special.es = Special.fs = Special.gs =
        segment(16, false, false, 0);
    Special.ldt.unusable = 1;
    Special.gdt.base = 0x7000;
    Special.gdt.limit = 0xff;
    Special.idt.base = 0x6000;
    Special.idt.limit = 0x7ff;
    kvm_regs Registers{};
    Registers.rax = 16;
    Registers.rdi = 0x9000;
    Registers.rsp = 0xd000;
    Registers.rip = Case == 6 ? 0xffff : Case == 7 || Case == 8 ? 0xfffe : Code;
    Registers.rflags = 2 | (Case == 3 || Case == 4 ? 0 : 0x200);
    std::vector<std::vector<uint8_t>> Instructions;
    const std::vector<uint8_t> LoadSS{0x8e, 0xd0};
    if (Case == 0) {
      Instructions = {LoadSS, {0xbc, 0x00, 0xc0, 0x00, 0x00}};
      if (!Code32)
        Instructions[1].insert(Instructions[1].begin(), 0x66);
    } else if (Case == 1) {
      Instructions = {{0x17}, {0x90}};
    } else if (Case == 2) {
      Instructions = {LoadSS, LoadSS};
    } else if (Case == 3 || Case == 4) {
      Instructions = {{0xfb}, {uint8_t(Case == 3 ? 0x90 : 0xfb)}};
    } else if (Case == 5) {
      Instructions = {{0xfa}};
    } else if (Case == 7 || Case == 8) {
      Instructions = {{0xb8, 0x34, 0x12}};
      if (Code32)
        Instructions[0].insert(Instructions[0].end(), {0x78, 0x56});
    } else if (Case == 9) {
      Instructions = {LoadSS, {0x90}};
    } else if (Case == 13) {
      Instructions = {{0x0f, 0xb2, 0x27}};
      if (!Code32)
        Instructions[0].insert(Instructions[0].begin(), {0x66, 0x67});
    } else {
      Instructions = {{0x90}};
    }
    uint32_t End = Registers.rip;
    for (const auto &Bytes : Instructions) {
      std::copy(Bytes.begin(), Bytes.end(), RAM + Special.cs.base + End);
      End += Bytes.size();
    }
    RAM[Special.cs.base + End] = 0x90;
    RAM[Special.cs.base + End + 1] = 0xf4;
    RAM[0x2800] = 0xf4;
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
    for (unsigned I = 0; I != Segments.size(); ++I)
      State.Core.Segments[I] = Cache(*Segments[I]);
    State.Core.GPR[0] = Registers.rax;
    State.Core.GPR[4] = Registers.rsp;
    State.Core.GPR[7] = Registers.rdi;
    State.Core.GDTRBase = Special.gdt.base;
    State.Core.GDTRLimit = Special.gdt.limit;
    State.Core.LDTR.Unusable = true;
    State.EIP = Registers.rip;
    State.EFlags = Registers.rflags;
    State.CR2 = Special.cr2;
    State.IDTRBase = Special.idt.base;
    State.IDTRLimit = Special.idt.limit;
    State.SSBlocked = Case == 10 || Case == 11;
    State.STIBlocked = Case == 12;
    const bool Pending = Case >= 10 && Case <= 12;
    const auto EventKind =
        Case == 11 ? X86LegacyEventKind::NMI : X86LegacyEventKind::Interrupt;
    const uint8_t Vector = Case == 11 ? 2 : 64;
    const auto Before = State;
    auto Logical =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Logical.Memory.push_back({0,
                              MemoryPermission::Read | MemoryPermission::Write,
                              0, std::vector<uint8_t>(RAM, RAM + MemorySize)});
    auto Memory =
        llvm::cantFail(X86PagedMemory::create(Logical, Features,
                                              {.CR0 = Special.cr0,
                                               .CR3 = Special.cr3,
                                               .CR4 = Special.cr4,
                                               .EFER = Special.efer,
                                               .RFlags = Registers.rflags,
                                               .PDPTE = {0x181001, 0, 0, 0}}));
    const X86LegacyExecutionOptions Options{
        {X86LegacySegmentProfile::WordStackOffsetFirst,
         X86AlignmentPriority::BeforePaging, X86FullSegmentLimitChoice::Fault},
        X86LegacyEventProfile::OrderedFrame,
        false};
    if (Pending) {
      auto R = llvm::cantFail(deliverX86LegacyEvent(
          State, *Memory, {State.EIP, 0, 0, Vector, EventKind}, Options));
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDeferred);
    }
    for (const auto &Bytes : Instructions) {
      auto R = llvm::cantFail(
          executeX86LegacyInstruction(State, *Memory, Bytes, Options));
      if (R.Status == X86LegacyExecutionStatus::EventDelivered)
        break;
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
      ASSERT_TRUE(R.InstructionCompleted);
    }
    if (Pending) {
      auto R = llvm::cantFail(deliverX86LegacyEvent(
          State, *Memory, {State.EIP, 0, 0, Vector, EventKind}, Options));
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    }
    auto Flush = Special;
    Flush.cr3 = 0x190000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0) << errno;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0) << errno;
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    Events.interrupt.shadow = Before.SSBlocked    ? KVM_X86_SHADOW_INT_MOV_SS
                              : Before.STIBlocked ? KVM_X86_SHADOW_INT_STI
                                                  : 0;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
    Debug.arch.debugreg[0] = Pending ? 0x2800 : Special.cs.base + End;
    Debug.arch.debugreg[1] = 0x2800;
    Debug.arch.debugreg[2] = Special.cs.base + uint16_t(End);
    Debug.arch.debugreg[3] = Special.cs.base + End + 1;
    Debug.arch.debugreg[7] = Pending ? 0x405 : 0x455;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    if (Pending && Case == 11)
      ASSERT_EQ(ioctl(CPU, KVM_NMI, 0), 0);
    Run->request_interrupt_window = Pending && Case != 11;
    if (Run->request_interrupt_window) {
      ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
      ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_IRQ_WINDOW_OPEN));
      Run->request_interrupt_window = 0;
      kvm_regs Window{};
      ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Window), 0);
      EXPECT_EQ(Window.rip, End);
      kvm_interrupt IRQ{Vector};
      ASSERT_EQ(ioctl(CPU, KVM_INTERRUPT, &IRQ), 0);
    }
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    EXPECT_EQ(Registers.rip, State.EIP);
    EXPECT_EQ(Registers.rax, State.Core.GPR[0]);
    EXPECT_EQ(Registers.rsp, State.Core.GPR[4]);
    EXPECT_EQ(Registers.rdi, State.Core.GPR[7]);
    EXPECT_EQ(Registers.rflags & ~uint64_t{0x10000}, State.EFlags);
    EXPECT_EQ(Special.cr2, State.CR2);
    for (unsigned I = 0; I != Segments.size(); ++I) {
      const auto Native = Cache(*Segments[I]);
      const auto &Model = State.Core.Segments[I];
      EXPECT_EQ(Native.Selector, Model.Selector);
      EXPECT_EQ(Native.Base, Model.Base);
      EXPECT_EQ(Native.Limit, Model.Limit);
      EXPECT_EQ(Native.Attributes, Model.Attributes);
    }
    for (unsigned Address = 0xcfd0; Address != 0xd020; ++Address) {
      auto Got =
          Memory->physicalMemory().loadScalar(Address, GuestScalarWidth::I8);
      EXPECT_EQ(Got.Value, RAM[Address]) << std::hex << Address;
    }
    ++Comparisons;
  }
};

TEST_P(X86LegacyStepNativeTest, OriginalFetchCompletionAndPendingEvents) {
  for (bool Code32 : {false, true})
    for (unsigned Case = 0; Case != 14; ++Case)
      ASSERT_NO_FATAL_FAILURE(compare(Code32, Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacySteps, X86LegacyStepNativeTest,
                         testing::Values(2u, 3u));
