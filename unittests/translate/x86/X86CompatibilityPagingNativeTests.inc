//===- X86CompatibilityPagingNativeTests.inc - Native narrow IA-32e -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original compatibility-mode MOVs over IA-32e paging. Included after the
/// X86PagingNativeTest fixture inside its Linux/x86-64 namespace.
//===----------------------------------------------------------------------===//

class X86CompatibilityPagingNativeTest : public X86PagingNativeTest {};

TEST_P(X86CompatibilityPagingNativeTest, WrapPermissionsAndLimitsMatchCPU) {
  using namespace neverd;
  unsigned Comparisons = 0;
  for (unsigned Case = 0; Case != 7; ++Case) {
    SCOPED_TRACE(Case);
    const bool Write = Case == 1 || Case == 4;
    const bool SegmentFault = Case == 5;
    const unsigned Prefix = Case == 6 ? 0x65 : 0x64;
    const uint64_t FSBase = 0xffff8000fffffffe;
    const uint32_t Offset = SegmentFault ? 2 : 0;
    std::memset(RAM, 0, MemorySize);
    const uint64_t Root = 0x10000;
    const uint64_t PML4 = GetParam() == 5 ? 0x11000 : Root;
    const uint64_t PDPT = PML4 + 0x1000, PD = PDPT + 0x1000;
    const uint64_t PT = PD + 0x1000, HighPD = PT + 0x1000;
    const uint64_t HighPT = HighPD + 0x1000;
    if (GetParam() == 5)
      put(Root, PML4 | 3);
    put(PML4, PDPT | 3);
    put(PDPT, PD | 3);
    put(PDPT + 3 * 8, HighPD | 3);
    put(PD, PT | 3);
    put(HighPD + 511 * 8, HighPT | 3);
    for (unsigned I = 0; I != MemorySize / 4096; ++I)
      put(PT + I * 8, uint64_t(I * 4096) | 3);
    put(PT, SecondDataPage | (Case == 3 ? 0 : Case == 4 ? 1 : 3));
    put(HighPT + 511 * 8, DataPage | (Case == 2 ? 0 : 3));
    put(DataPage + 4094, 0x6655, 2);
    put(SecondDataPage, 0x8877, 2);
    put(0x8008, 0x00cf9b000000ffff);
    put(0x8010, 0x00cf93000000ffff);
    put(0x8018, 0x00af9b000000ffff);
    for (unsigned Vector : {12u, 13u, 14u}) {
      const uint64_t Target = Handler + Vector * 16;
      put(0x9000 + Vector * 16,
          Target | (uint64_t(0x18) << 16) | (uint64_t(0x8e) << 40));
      RAM[Target] = 0xcc;
    }
    RAM[Code] = Prefix;
    RAM[Code + 1] = Write ? 0xa3 : 0xa1;
    put(Code + 2, Offset, 4);
    RAM[Code + 6] = 0xcc;
    auto Native = Initial;
    Native.cs = segment(8, true, false, 0);
    Native.ds = Native.es = Native.ss = segment(16, false, false, 0);
    Native.fs = Native.gs = Native.ds;
    Native.fs.base = Native.gs.base = FSBase;
    if (SegmentFault) {
      Native.fs.limit = Native.gs.limit = 3;
      Native.fs.g = Native.gs.g = 0;
    }
    Native.gdt = {0x8000, 0x3f, {}};
    Native.idt = {0x9000, 0xfff, {}};
    Native.tr.base = 0xa000;
    Native.tr.limit = 0x67;
    Native.tr.type = 11;
    Native.tr.present = 1;
    Native.tr.s = Native.tr.unusable = 0;
    Native.cr0 = 0x80010031;
    Native.cr2 = 0x12345678;
    Native.cr3 = Root;
    Native.cr4 = 0x20 | (GetParam() == 5 ? 0x1000 : 0);
    Native.efer = 0x500;
    kvm_regs Registers{};
    Registers.rip = Code;
    Registers.rsp = Stack;
    Registers.rflags = 2;
    Registers.rax = 0x44332211;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Native), 0) << errno;
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0) << errno;
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0) << errno;

    X86PagingState Controls{.CR0 = Native.cr0,
                            .CR3 = Native.cr3,
                            .CR4 = Native.cr4,
                            .EFER = Native.efer,
                            .RFlags = 2};
    auto Input =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Input.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Write,
                            0, std::vector<uint8_t>(RAM, RAM + MemorySize)});
    auto Memory =
        llvm::cantFail(X86PagedMemory::create(Input, Features, Controls));
    std::array<uint8_t, 4> Bytes{0x11, 0x22, 0x33, 0x44};
    // The mode owner supplies the low base for an explicit compatibility
    // operand; the full FS/GS base remains in its architectural bank.
    X86SegmentedAccess Access{
        Prefix == 0x64 ? X86SegmentRegister::FS : X86SegmentRegister::GS,
        {uint32_t(FSBase), SegmentFault ? 3u : UINT32_MAX, 16,
         uint16_t(SegmentFault ? 0x4093 : 0xc093), false},
        Offset,
        Write ? MemoryAccessKind::Write : MemoryAccessKind::Read,
        0,
        X86FullSegmentLimitChoice::Fault};
    auto Model = Write ? Memory->storeSegment(Access, Bytes)
                       : Memory->loadSegment(Access, Bytes);
    ASSERT_TRUE(bool(Model)) << llvm::toString(Model.takeError());
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Run->exit_reason, KVM_EXIT_DEBUG);
    ASSERT_EQ(Run->debug.arch.exception, 3);
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Native), 0);
    EXPECT_EQ(Native.fs.base, FSBase);
    EXPECT_EQ(Native.gs.base, FSBase);
    if (Case >= 2 && Case <= 5) {
      ASSERT_EQ(Model->Status, X86PageWalkStatus::Exception);
      ASSERT_TRUE(Model->Exception);
      const uint8_t Vector = SegmentFault ? 13 : 14;
      EXPECT_EQ(Model->Exception->Vector, Vector);
      EXPECT_EQ(Registers.rip, Handler + Vector * 16);
      EXPECT_EQ(get(Registers.rsp), Model->Exception->ErrorCode);
      EXPECT_EQ(get(Registers.rsp + 8), Code);
      EXPECT_EQ(Native.cr2, Model->Exception->CR2.value_or(0x12345678));
      EXPECT_EQ(uint32_t(Registers.rax), 0x44332211u);
      EXPECT_EQ(Native.cs.selector, 0x18);
      EXPECT_EQ(Native.cs.l, 1);
    } else {
      ASSERT_EQ(Model->Status, X86PageWalkStatus::Completed);
      EXPECT_EQ(Registers.rip, Code + 6);
      EXPECT_EQ(Native.cs.selector, 8);
      EXPECT_EQ(Native.cs.l, 0);
      if (!Write) {
        uint32_t Value = 0;
        for (unsigned I = 0; I != Bytes.size(); ++I)
          Value |= uint32_t(Bytes[I]) << (I * 8);
        EXPECT_EQ(uint32_t(Registers.rax), Value);
      }
    }
    for (uint64_t Address : {DataPage, SecondDataPage}) {
      std::array<uint8_t, 4096> Actual{};
      ASSERT_EQ(Memory->physicalMemory().loadBytes(Address, Actual).Status,
                GuestMemoryAccessStatus::Completed);
      EXPECT_TRUE(std::equal(Actual.begin(), Actual.end(), RAM + Address));
    }
    ++Comparisons;
  }
  RecordProperty("original_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(IA32e, X86CompatibilityPagingNativeTest,
                         testing::Values(4, 5));
