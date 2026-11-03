//===- X86LegacyFastSystemNativeTests.inc - Original SEP transfers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent protected SYSENTER/SYSEXIT cache, fault and flag comparisons.
//===----------------------------------------------------------------------===//

class X86LegacyFastSystemNativeTest : public X86PagingNativeTest {
protected:
  unsigned Comparisons = 0;

  void SetUp() override {
    X86PagingNativeTest::SetUp();
    if (HasFatalFailure() || IsSkipped())
      return;
    const char *Bound = std::getenv("NEVERD_TEST_LEGACY_FAST_SYSTEM");
    if (!Bound || std::strcmp(Bound, "intel"))
      GTEST_SKIP() << "Bind the qualified Intel SEP processor profile";
  }

  void compare(bool Enter, bool Code32, bool Stack32, unsigned CPL, uint32_t CS,
               uint32_t Flags, unsigned Case = 0,
               std::vector<uint8_t> Prefix = {}) {
    using namespace neverd;
    SCOPED_TRACE(testing::Message()
                 << "enter=" << Enter << " code32=" << Code32
                 << " stack32=" << Stack32 << " cpl=" << CPL << " case=" << Case
                 << " cs=" << std::hex << CS << " flags=" << Flags
                 << " prefix=" << testing::PrintToString(Prefix));
    const bool Legacy = GetParam() == 2;
    const unsigned PW = Legacy ? 4 : 8;
    const uint32_t PT = Legacy ? 0x11000 : 0x12000;
    constexpr uint32_t Base = 0x20000, Target = 0x4000, Stop = 0x5000;
    constexpr uint32_t Record = 0x4800, GDT = 0x7000, IDT = 0x6000;
    const uint32_t PC = Case == 4 ? 0x1fff : 0x1000;
    Prefix.push_back(0x0f);
    Prefix.push_back(Enter ? 0x34 : 0x35);
    std::memset(RAM, 0, MemorySize);
    put(0x10000, 0x11000 | (Legacy ? 7 : 1), PW);
    if (!Legacy)
      put(0x11000, 0x12007);
    for (unsigned I = 0; I != MemorySize / 4096; ++I)
      put(PT + I * PW, I * 4096 | 7, PW);
    put(GDT + 8, 0x00cf9b000000ffffULL);
    put(GDT + 16, 0x00cf93000000ffffULL);
    put(0x5804, 0xb000, 4);
    put(0x5808, 16, 2);
    for (unsigned V = 0; V != 256; ++V) {
      const unsigned HandlerPC = 0x2000 + V * 16;
      put(IDT + V * 8,
          HandlerPC | (uint64_t{8} << 16) | (uint64_t{0x8e} << 40));
      const std::vector<uint8_t> Save{0xc7,       5, 0, 0x48, 0,   0,
                                      uint8_t(V), 0, 0, 0,    0xe9};
      std::copy(Save.begin(), Save.end(), RAM + HandlerPC);
      put(HandlerPC + Save.size(), Stop - (HandlerPC + Save.size() + 4), 4);
    }
    std::copy(Prefix.begin(), Prefix.end(), RAM + Base + PC);
    if (Case == 1)
      put(PT + 15 * PW, 0xf006, PW);
    if (Case == 2)
      for (unsigned Page : {5u, 6u, 7u})
        put(PT + Page * PW, Page * 4096 | 6, PW);
    if (Case == 4)
      put(PT + 0x22 * PW, 0x22006, PW);
    kvm_sregs Special = Initial;
    Special.cr0 = 0x80050011;
    Special.cr3 = 0x10000;
    Special.cr4 = Legacy ? 0 : 0x20;
    Special.efer = Legacy ? 0 : 0x800;
    Special.cr2 = 0x76543210;
    Special.cs = segment(24 | CPL, true, false, CPL);
    Special.cs.base = Base;
    Special.cs.limit = Case == 3 ? PC : 0xfffff;
    Special.cs.g = 0;
    Special.cs.db = Code32;
    Special.ds = Special.es = Special.ss = Special.fs = Special.gs =
        segment(32 | CPL, false, false, CPL);
    Special.ss.base = 0x10000;
    Special.ss.db = Stack32;
    Special.gdt = {GDT, 0xff, {}};
    Special.idt = {IDT, 0x7ff, {}};
    Special.ldt = {};
    Special.ldt.unusable = 1;
    Special.tr = {};
    Special.tr.base = 0x5800;
    Special.tr.limit = 0x67;
    Special.tr.selector = 40;
    Special.tr.type = 11;
    Special.tr.present = 1;
    kvm_regs Registers{};
    Registers.rip = PC;
    Registers.rsp = Stack32 ? 0xd100 : 0xabcdd100;
    Registers.rax = 0x12345678;
    Registers.rcx = 0xf000;
    Registers.rdx = Target;
    Registers.rbx = 0x22334455;
    Registers.rbp = 0x33445566;
    Registers.rsi = 0x44556677;
    Registers.rdi = 0x55667788;
    Registers.rflags = Flags;
    const auto Cache = [](const kvm_segment &C) {
      return X86LegacySegmentCache{uint32_t(C.base), C.limit, C.selector,
                                   uint16_t(C.type | C.s << 4 | C.dpl << 5 |
                                            C.present << 7 | C.avl << 12 |
                                            C.l << 13 | C.db << 14 | C.g << 15),
                                   bool(C.unusable)};
    };
    X86LegacyExecutionState State;
    State.Core.GPR = {uint32_t(Registers.rax), uint32_t(Registers.rcx),
                      uint32_t(Registers.rdx), uint32_t(Registers.rbx),
                      uint32_t(Registers.rsp), uint32_t(Registers.rbp),
                      uint32_t(Registers.rsi), uint32_t(Registers.rdi)};
    std::array<kvm_segment *, 6> Segments{&Special.es, &Special.cs,
                                          &Special.ss, &Special.ds,
                                          &Special.fs, &Special.gs};
    for (unsigned I = 0; I != Segments.size(); ++I)
      State.Core.Segments[I] = Cache(*Segments[I]);
    State.Core.CPL = CPL;
    State.Core.GDTRBase = GDT;
    State.Core.GDTRLimit = 0xff;
    State.Core.LDTR = Cache(Special.ldt);
    State.TR = Cache(Special.tr);
    State.IDTRBase = IDT;
    State.IDTRLimit = 0x7ff;
    State.EIP = PC;
    State.EFlags = Flags;
    State.CR2 = Special.cr2;
    State.Sysenter = {CS, 0xf000, Target};
    X86PagingState Controls{.CR0 = Special.cr0,
                            .CR3 = Special.cr3,
                            .CR4 = Special.cr4,
                            .EFER = Special.efer,
                            .RFlags = Flags,
                            .PDPTE = {0x11001, 0, 0, 0}};
    auto Logical =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Logical.Memory.push_back({0,
                              MemoryPermission::Read | MemoryPermission::Write,
                              0, std::vector<uint8_t>(RAM, RAM + MemorySize)});
    auto Memory =
        llvm::cantFail(X86PagedMemory::create(Logical, Features, Controls));
    X86LegacyExecutionOptions Options;
    Options.Events = X86LegacyEventProfile::OrderedFrame;
    Options.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
    Options.Segments.FullLimitChoice = X86FullSegmentLimitChoice::Fault;
    auto Expected =
        executeX86LegacyInstruction(State, *Memory, Prefix, Options);
    ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
    struct {
      uint32_t Count = 3, Padding = 0;
      kvm_msr_entry Entries[3]{};
    } MSRs;
    MSRs.Entries[0] = {0x174, 0, CS};
    MSRs.Entries[1] = {0x175, 0, 0xf000};
    MSRs.Entries[2] = {0x176, 0, Target};
    ASSERT_EQ(ioctl(CPU, KVM_SET_MSRS, &MSRs), 3);
    auto Flush = Special;
    Flush.cr3 = 0x90000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0);
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
    Debug.arch.debugreg[0] = Target;
    Debug.arch.debugreg[1] = Stop;
    Debug.arch.debugreg[7] = 0x405;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    if (Expected->InstructionCompleted) {
      EXPECT_EQ(Registers.rip, Target);
      EXPECT_EQ(State.EIP, Target);
    } else {
      ASSERT_EQ(Expected->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_EQ(Registers.rip, Stop);
      EXPECT_EQ(get(Record, 4), Expected->DeliveredVector);
    }
    const std::array<uint32_t, 8> Actual{
        uint32_t(Registers.rax), uint32_t(Registers.rcx),
        uint32_t(Registers.rdx), uint32_t(Registers.rbx),
        uint32_t(Registers.rsp), uint32_t(Registers.rbp),
        uint32_t(Registers.rsi), uint32_t(Registers.rdi)};
    EXPECT_EQ(Actual, State.Core.GPR);
    EXPECT_EQ(Registers.rflags & ~uint64_t{0x10000},
              State.EFlags & ~uint32_t{0x10000});
    EXPECT_EQ(Special.cr2, State.CR2);
    for (unsigned I = 0; I != Segments.size(); ++I) {
      const auto Got = Cache(*Segments[I]);
      const auto &Want = State.Core.Segments[I];
      EXPECT_EQ(Got.Base, Want.Base) << I;
      EXPECT_EQ(Got.Limit, Want.Limit) << I;
      EXPECT_EQ(Got.Selector, Want.Selector) << I;
      EXPECT_EQ(Got.Attributes, Want.Attributes) << I;
      EXPECT_EQ(Got.Unusable, Want.Unusable) << I;
    }
    for (unsigned Begin : {0xafc0u, 0x1d0c0u, 0x7000u, 0xf000u})
      for (unsigned I = 0; I != 64; ++I) {
        auto Got = Memory->physicalMemory().loadScalar(Begin + I,
                                                       GuestScalarWidth::I8);
        ASSERT_EQ(Got.Status, GuestMemoryAccessStatus::Completed);
        EXPECT_EQ(get(Begin + I, 1), Got.Value) << std::hex << Begin + I;
      }
    ++Comparisons;
  }
};

TEST_P(X86LegacyFastSystemNativeTest, PrivilegesSelectorsFlagsAndCachedState) {
  for (bool Enter : {false, true})
    for (bool Code32 : {false, true})
      for (bool Stack32 : {false, true})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (uint32_t CS : {0u, 1u, 2u, 3u, 4u, 8u, 0xffecu, 0xfff0u, 0xfff4u,
                              0xfff8u, 0xfffcu, 0xffffu})
            for (uint32_t Flags :
                 {0x202u, 0x14a97u, 0x40202u, 0x183202u, 0x3d7ed7u})
              ASSERT_NO_FATAL_FAILURE(
                  compare(Enter, Code32, Stack32, CPL, CS, Flags));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyFastSystemNativeTest, TransfersDoNotReadTablesOrNewStack) {
  for (bool Enter : {false, true})
    for (bool Code32 : {false, true})
      for (unsigned CPL = 0; CPL != (Enter ? 4u : 1u); ++CPL)
        for (unsigned Case : {1u, 2u})
          for (uint32_t CS : {4u, 0xfff0u, 0xfff8u})
            ASSERT_NO_FATAL_FAILURE(
                compare(Enter, Code32, false, CPL, CS, 0x10202, Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyFastSystemNativeTest, PrefixesAndFetchFaultPrecedence) {
  for (bool Enter : {false, true})
    for (bool Code32 : {false, true})
      for (unsigned CPL : {0u, 3u})
        for (uint32_t CS : {0u, 8u})
          for (unsigned Case : {0u, 3u, 4u})
            for (const std::vector<uint8_t> Prefix : {std::vector<uint8_t>{},
                                                      {0x66},
                                                      {0x67},
                                                      {0xf2},
                                                      {0xf3},
                                                      {0x36},
                                                      {0x66, 0x67, 0x36},
                                                      {0xf0},
                                                      {0x67, 0xf0, 0x66}})
              ASSERT_NO_FATAL_FAILURE(
                  compare(Enter, Code32, false, CPL, CS, 0x202, Case, Prefix));
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyFastSystem, X86LegacyFastSystemNativeTest,
                         testing::Values(2u, 3u));
