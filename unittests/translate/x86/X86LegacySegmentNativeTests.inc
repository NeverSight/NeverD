//===- X86LegacySegmentNativeTests.inc - Original i386 operand oracle
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original MOV instructions with independently configured KVM segment caches.
//===----------------------------------------------------------------------===//

class X86LegacySegmentNativeTest : public X86PagingNativeTest {};

TEST_P(X86LegacySegmentNativeTest, OriginalMOVMatchesSegmentsBeforePaging) {
  using namespace neverd;
  unsigned Comparisons = 0;
  const auto Compare = [&](unsigned SegmentIndex, unsigned Form, unsigned Bytes,
                           bool Write, unsigned PageFailure) {
    SCOPED_TRACE(testing::Message()
                 << "segment=" << SegmentIndex << " form=" << Form << " bytes="
                 << Bytes << " write=" << Write << " page=" << PageFailure);
    const bool Legacy = GetParam() == 2;
    const unsigned Width = Legacy ? 4 : 8;
    const uint64_t PT = Legacy ? 0x11000 : 0x12000;
    std::memset(RAM, 0, MemorySize);
    put(0x10000, Legacy ? 0x11007 : 0x11001, Width);
    if (!Legacy)
      put(0x11000, 0x12007);
    for (unsigned I = 0; I != MemorySize / 4096; ++I)
      put(PT + I * Width, I * 4096 | 7, Width);
    if (Legacy) {
      put(0x10ffc, 0x21007, 4);
      put(0x21ffc, DataPage | 7, 4);
    } else {
      put(0x10018, 0x20001);
      put(0x20ff8, 0x21007);
      put(0x21ff8, DataPage | 7);
    }
    put(PT, SecondDataPage | 7, Width);
    put(PT + 15 * Width, DataPage | 7, Width);
    put(PT + 16 * Width, SecondDataPage | 7, Width);
    put(0x5008, 0x00cf9b000000ffffULL);
    put(0x5010, 0x00cf93000000ffffULL);
    put(0x5018, 0x00cffb000000ffffULL);
    put(0x5020, 0x00cff3000000ffffULL);
    put(0x5804, Stack, 4);
    put(0x5808, 16, 2);
    for (unsigned Vector : {12u, 13u, 14u})
      put(0x6000 + Vector * 8,
          Handler + Vector * 16 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));

    // CPL3 lets #SS delivery use a separate valid supervisor stack cache.
    auto Cache =
        segment(SegmentIndex == 1 ? 27 : 35, SegmentIndex == 1, false, 3);
    uint32_t Offset = 0x40000;
    switch (Form) {
    case 0:
      break; // Flat valid span.
    case 1:
      Cache.base = 0x10000;
      break;
    case 2:
      Cache.base = 0xffff0000;
      break;
    case 3:
      Cache.limit = Offset + Bytes - 1;
      Cache.g = 0;
      break;
    case 4:
      Cache.limit = Offset + Bytes - 2;
      Cache.g = 0;
      break;
    case 5:
      Cache.type = SegmentIndex == 1 ? 9 : 1;
      break;
    case 6:
      Cache.type = 7;
      Cache.g = 0;
      Cache.limit = Offset - 1;
      break;
    case 7:
      Cache.type = 7;
      Cache.g = 0;
      Cache.limit = Offset;
      break;
    case 8:
      Cache.type = 7;
      Cache.g = 0;
      Cache.db = 0;
      Cache.limit = 0x7fff;
      Offset = 0xffff - Bytes + 1;
      break;
    case 9:
      Cache.type = 7;
      Cache.g = 0;
      Cache.db = 0;
      Cache.limit = 0x7fff;
      Offset = 0xffff;
      break;
    case 10:
      Cache.unusable = 1;
      Cache.present = 0;
      Cache.selector = 0;
      break;
    case 11:
      Offset = UINT32_MAX - Bytes + 1;
      break;
    case 12:
      Cache.base = 0xfffc0000;
      Offset = 0x3ffff;
      break;
    }
    if (SegmentIndex == 1 && (Form == 1 || Form == 2 || Form >= 6))
      return; // CS base/limit also owns instruction fetch; qualified
              // separately.
    if (SegmentIndex == 2 && (Form == 5 || Form == 10))
      return; // These snapshots could not have been loaded into protected SS.
    const uint32_t Linear = Cache.base + Offset;
    const auto PTE = [&](uint32_t Address) {
      return Address >= 0xfffff000 ? 0x21000 + (Legacy ? 1023 : 511) * Width
                                   : PT + (Address >> 12) * Width;
    };
    const auto Physical = [&](uint32_t Address) -> uint64_t {
      return Address >= 0xfffff000 ? DataPage + (Address & 4095)
             : Address < 4096      ? SecondDataPage + Address
             : Address >= 0xf000 && Address < 0x10000
                 ? DataPage + (Address & 4095)
             : Address >= 0x10000 && Address < 0x11000
                 ? SecondDataPage + (Address & 4095)
                 : Address;
    };
    for (unsigned I = 0; I != Bytes; ++I)
      put(Physical(Linear + I), 0x80 + I, 1);
    const uint64_t BadPTE = PTE(Linear + Bytes - 1);
    if (PageFailure)
      put(BadPTE,
          get(BadPTE, Width) & ~(PageFailure == 1   ? 1
                                 : PageFailure == 2 ? 2
                                                    : 4),
          Width);

    const std::array<uint8_t, 6> Prefix{0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65};
    unsigned CodeBytes = 0;
    RAM[Code + CodeBytes++] = Prefix[SegmentIndex];
    if (Bytes == 2)
      RAM[Code + CodeBytes++] = 0x66;
    RAM[Code + CodeBytes++] =
        Write ? (Bytes == 1 ? 0x88 : 0x89) : (Bytes == 1 ? 0x8a : 0x8b);
    RAM[Code + CodeBytes++] = 0x07;
    X86PagingState State{.CR0 = 0x80010011,
                         .CR3 = 0x10000,
                         .CR4 = Legacy ? 0u : 0x20u,
                         .EFER = Legacy ? 0u : 0x800u,
                         .RFlags = 2,
                         .PDPTE = {0x11001, 0, 0, 0x20001}};
    auto Logical =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Logical.Memory.push_back({0,
                              MemoryPermission::Read | MemoryPermission::Write,
                              0, std::vector<uint8_t>(RAM, RAM + MemorySize)});
    auto Memory =
        llvm::cantFail(X86PagedMemory::create(Logical, Features, State));
    const X86LegacySegmentCache Bound{
        uint32_t(Cache.base), Cache.limit, Cache.selector,
        uint16_t(Cache.type | Cache.s << 4 | Cache.dpl << 5 |
                 Cache.present << 7 | Cache.db << 14 | Cache.g << 15),
        bool(Cache.unusable)};
    const X86SegmentedAccess Access{
        static_cast<X86SegmentRegister>(SegmentIndex),
        Bound,
        Offset,
        Write ? MemoryAccessKind::Write : MemoryAccessKind::Read,
        3,
        {}};
    std::array<uint8_t, 4> Output{0xa5, 0xa5, 0xa5, 0xa5};
    const std::array<uint8_t, 4> Input{0x44, 0x33, 0x22, 0x11};
    auto Expected =
        Write ? Memory->storeSegment(Access,
                                     llvm::ArrayRef(Input).take_front(Bytes))
              : Memory->loadSegment(
                    Access, llvm::MutableArrayRef(Output).take_front(Bytes));
    ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
    kvm_sregs Special = Initial;
    Special.cr0 = State.CR0;
    Special.cr3 = State.CR3;
    Special.cr4 = State.CR4;
    Special.efer = State.EFER;
    Special.cr2 = 0x76543210;
    Special.cs = segment(27, true, false, 3);
    Special.ds = Special.es = Special.ss = Special.fs = Special.gs =
        segment(35, false, false, 3);
    std::array<kvm_segment *, 6> Segments{&Special.es, &Special.cs,
                                          &Special.ss, &Special.ds,
                                          &Special.fs, &Special.gs};
    *Segments[SegmentIndex] = Cache;
    Special.gdt.base = 0x5000;
    Special.gdt.limit = 0x3f;
    Special.idt.base = 0x6000;
    Special.idt.limit = 4095;
    Special.ldt.unusable = 1;
    Special.tr = {};
    Special.tr.base = 0x5800;
    Special.tr.limit = 0x67;
    Special.tr.selector = 40;
    Special.tr.type = 11;
    Special.tr.present = 1;
    auto Flush = Special;
    Flush.cr3 = 0x90000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0) << errno;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0) << errno;
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    kvm_regs Registers{};
    Registers.rip = Code;
    Registers.rsp = UserStack;
    Registers.rdi = Offset;
    Registers.rflags = 2;
    Registers.rax = 0x11223344;
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
    Debug.arch.debugreg[0] = Code + CodeBytes;
    Debug.arch.debugreg[1] = Handler + 12 * 16;
    Debug.arch.debugreg[2] = Handler + 13 * 16;
    Debug.arch.debugreg[3] = Handler + 14 * 16;
    Debug.arch.debugreg[7] = 0x455;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    if (Expected->Exception) {
      EXPECT_EQ(Registers.rip, Handler + Expected->Exception->Vector * 16);
      EXPECT_EQ(get(Registers.rsp, 4), Expected->Exception->ErrorCode);
      EXPECT_EQ(Special.cr2, Expected->Exception->CR2.value_or(0x76543210));
      EXPECT_EQ(Registers.rax, 0x11223344u);
      EXPECT_EQ(Output, (std::array<uint8_t, 4>{0xa5, 0xa5, 0xa5, 0xa5}));
    } else {
      ASSERT_EQ(Expected->Status, X86PageWalkStatus::Completed);
      EXPECT_EQ(Registers.rip, Code + CodeBytes);
      uint32_t Value = 0x11223344;
      if (!Write)
        std::memcpy(&Value, Output.data(), Bytes);
      EXPECT_EQ(Registers.rax, Value);
    }
    EXPECT_EQ(Registers.rdi, Offset);
    for (unsigned I = 0; I != Bytes; ++I) {
      const auto P = Physical(Linear + I);
      EXPECT_EQ(
          Memory->physicalMemory().loadScalar(P, GuestScalarWidth::I8).Value,
          RAM[P]);
    }
    ++Comparisons;
  };
  for (unsigned SegmentIndex = 0; SegmentIndex != 6; ++SegmentIndex)
    for (unsigned Form = 0; Form != 13; ++Form)
      for (unsigned Bytes : {1u, 2u, 4u})
        for (bool Write : {false, true})
          for (unsigned PageFailure : {0u, 1u, 2u, 3u})
            ASSERT_NO_FATAL_FAILURE(
                Compare(SegmentIndex, Form, Bytes, Write, PageFailure));
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyPagingModes, X86LegacySegmentNativeTest,
                         testing::Values(2u, 3u));
