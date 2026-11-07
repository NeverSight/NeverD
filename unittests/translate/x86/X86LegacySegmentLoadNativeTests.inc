//===- X86LegacySegmentLoadNativeTests.inc - Original segment loads -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original i386 selector instructions, independent descriptors and KVM state.
//===----------------------------------------------------------------------===//

class X86LegacySegmentLoadNativeTest : public X86PagingNativeTest {
protected:
  unsigned Comparisons = 0;

  void
  compare(unsigned SegmentIndex, unsigned Form, unsigned Bytes, bool Addr16,
          bool Code32, unsigned CPL, unsigned Case,
          unsigned Encoding = UINT32_MAX, int SegmentOverride = -1,
          X86AlignmentPriority Alignment = X86AlignmentPriority::AfterPaging) {
    using namespace neverd;
    const bool Read = Form >= 4, Push = Form == 6, Pop = Form == 2;
    const bool StackOperand = Push || Pop, Far = Form == 3;
    const bool Register = Form == 0 || Form == 4;
    if ((SegmentIndex == 1 && !Read) || (Read && Case && Case < 17) ||
        (Read && Case >= 23 && Case <= 27) ||
        (Register && Case >= 17 && Case <= 22) ||
        (Register && (Case == 26 || Case == 31)) ||
        (Case == 20 && !StackOperand) || (Case == 26 && !Far) ||
        (Case == 31 && (!Far || !Addr16)) ||
        ((Case >= 17 && Case <= 19 || Case == 35 || Case == 36 || Case == 38) &&
         StackOperand && CPL == 0) ||
        (Read && Case >= 34 && Case < 40) ||
        (Register && Case >= 35 && Case <= 38) || (Case == 37 && !Far))
      return;
    SCOPED_TRACE(testing::Message()
                 << "segment=" << SegmentIndex << " form=" << Form
                 << " bytes=" << Bytes << " addr16=" << Addr16
                 << " code32=" << Code32 << " cpl=" << CPL << " case=" << Case
                 << " encoding=" << Encoding
                 << " override=" << SegmentOverride);
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
      put(0x21ffc, WrappedPage | 7, 4);
    } else {
      put(0x10018, 0x20001);
      put(0x20ff8, 0x21007);
      put(0x21ff8, WrappedPage | 7);
    }
    const auto Physical = [&](uint32_t Address) -> uint64_t {
      return Address >= 0xfffff000 ? WrappedPage + (Address & 4095) : Address;
    };
    const auto Put = [&](uint32_t Address, uint64_t V, unsigned Size = 8) {
      for (unsigned I = 0; I != Size; ++I)
        put(Physical(Address + I), V >> (I * 8), 1);
    };
    const auto PTE = [&](uint32_t Address) {
      return Address >= 0xfffff000 ? 0x21000 + (Legacy ? 1023 : 511) * Width
                                   : PT + (Address >> 12) * Width;
    };
    uint32_t GDT = (Case == 29 || Case == 30 || (Case >= 108 && Case <= 110))
                       ? 0x7fc4
                   : Case == 32 ? 0xffffffe0
                   : Case == 33 ? 0xffffffc4
                                : 0x7000;
    Put(GDT + 8, 0x00cf9b000000ffffULL);
    Put(GDT + 16, 0x00cf93000000ffffULL);
    Put(GDT + 24, 0x00cffb000000ffffULL);
    Put(GDT + 32, 0x00cff3000000ffffULL);
    put(0x5804, Stack, 4);
    put(0x5808, 16, 2);
    constexpr uint64_t FaultStop = 0x2800;
    for (unsigned Vector : {11u, 12u, 13u, 14u, 17u}) {
      const uint64_t Entry = Handler + Vector * 16;
      put(0x6000 + Vector * 8,
          Entry | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
      RAM[Entry] = 0x6a;
      RAM[Entry + 1] = Vector;
      RAM[Entry + 2] = 0xe9;
      put(Entry + 3, FaultStop - (Entry + 7), 4);
    }
    const uint16_t Index = Case == 32 || Case == 33 ? 56 : 0x1038;
    uint16_t Selector = Index | CPL;
    uint64_t Descriptor = 0x00cff2000000ffffULL;
    uint16_t GDTLimit = 0x103f;
    bool Local = false, LDTUsable = false;
    switch (Case) {
    case 1:
      Selector = 0;
      break;
    case 2:
      Selector = 3;
      break;
    case 3:
      Descriptor = 0x00cff1000000ffffULL;
      break;
    case 4:
      Descriptor = 0x00cff9000000ffffULL;
      break;
    case 5:
      Descriptor = 0x00cffb000000ffffULL;
      break;
    case 6:
      Descriptor = 0x00cf9f000000ffffULL;
      break;
    case 7:
      Descriptor = 0x00cfe2000000ffffULL;
      break;
    case 8:
      Descriptor = 0x00cf92000000ffffULL;
      break;
    case 9:
      Descriptor &= ~(uint64_t{1} << 47);
      break;
    case 10:
      Selector = Index | (3 - CPL);
      break;
    case 11:
      GDTLimit = Index + 6;
      break;
    case 12:
      Local = true;
      break;
    case 13:
      Local = LDTUsable = true;
      break;
    case 16:
      Descriptor |= uint64_t{1} << 40;
      break;
    case 23:
      Selector = 32 | CPL;
      break;
    case 24:
      Descriptor = 0x120af2345678bcdeULL;
      break;
    case 25:
      Descriptor = 0xffcff2fc0000ffffULL;
      break;
    case 27:
      Local = LDTUsable = true;
      Selector = CPL;
      break;
    case 28:
      Descriptor = 0x00cf71000000ffffULL; // Read-only and not present.
      break;
    case 34:
      Selector = Index | (3 - CPL);
      break;
    case 38:
      Selector = 0;
      break;
    case 39:
      Local = LDTUsable = true;
      break;
    }
    if (Case != 6 && Case != 8)
      Descriptor = (Descriptor & ~(uint64_t{3} << 45)) | (uint64_t(CPL) << 45);
    if (Case >= 40 && Case < 104) {
      Selector = (Selector & ~3) | ((Case - 40) & 3);
      Descriptor = (Descriptor & ~(uint64_t{3} << 45)) |
                   (uint64_t(((Case - 40) / 4) & 3) << 45);
      if (Case >= 56)
        Descriptor |= uint64_t{15} << 40; // Conforming readable code.
    }
    if (Case >= 72 && Case < 104) {
      Selector = Index | CPL;
      Descriptor =
          (Descriptor & ~(uint64_t{0x7f} << 40)) |
          (uint64_t(CPL * 32 + (Case < 88 ? 16 : 0) + (Case - 72) % 16) << 40);
    }
    if (Case == 110)
      Descriptor |= uint64_t{1} << 40;
    if (Local)
      Selector |= 4;
    const uint32_t DescriptorAddress =
        (Local ? 0xc000 : GDT) + (Selector & 0xfff8);
    Put(DescriptorAddress, Descriptor);

    kvm_sregs Special = Initial;
    Special.cr0 = 0x80010011 |
                  (Case == 22 || (Case >= 104 && Case <= 106) ? 1u << 18 : 0);
    Special.cr3 = 0x10000;
    Special.cr4 = Legacy ? 0 : 0x20;
    Special.efer = Legacy ? 0 : 0x800;
    Special.cr2 = 0x76543210;
    Special.cs = segment(CPL ? 24 | CPL : 8, true, false, CPL);
    Special.cs.db = Code32;
    Special.ds = Special.es = Special.ss = Special.fs = Special.gs =
        segment(32 | CPL, false, false, CPL);
    Special.gdt.base = GDT;
    Special.gdt.limit = GDTLimit;
    Special.idt.base = 0x6000;
    Special.idt.limit = 4095;
    Special.ldt = {};
    Special.ldt.base = 0xc000;
    Special.ldt.limit = Case == 39 ? Index + 6 : 0x103f;
    Special.ldt.selector = 48;
    Special.ldt.type = 2;
    Special.ldt.present = LDTUsable;
    Special.ldt.unusable = !LDTUsable;
    Special.tr = {};
    Special.tr.base = 0x5800;
    Special.tr.limit = 0x67;
    Special.tr.selector = 40;
    Special.tr.type = 11;
    Special.tr.present = 1;
    uint32_t Offset = Addr16 ? (StackOperand ? 0xe000 : 0x9000) : 0x40000;
    if (Case >= 108 && Case <= 110)
      Offset = 0xe000;
    if (Case == 20) {
      Special.ss.db = 0;
      Special.ss.g = 0;
      Special.ss.limit = 0xffff;
      Offset = 0xfffc;
    }
    if (Case == 21)
      Offset = Far ? 0xb000 - Bytes : 0xafff;
    if (Case == 22 || (Case >= 104 && Case <= 106))
      Offset += Case >= 105 ? 2 : 1;
    if (Case == 31)
      Offset = 0x10000 - Bytes;
    if (Case == 19 || Case == 26 || Case == 36 || Case == 37) {
      auto &Source = StackOperand ? Special.ss : Special.ds;
      Source.g = 0;
      Source.limit = Offset + (Case == 26 || Case == 37 ? Bytes - 1 : 0);
    }
    Put(Offset, Far ? 0x89abcdef : Selector, Far ? Bytes : 2);
    if (Far)
      Put(Addr16 ? uint16_t(Offset + Bytes) : Offset + Bytes, Selector, 2);
    if (Case == 14 || Case == 15 || Case == 16 || Case == 30 || Case == 34 ||
        Case == 35) {
      const auto Entry = PTE(DescriptorAddress + (Case == 30 ? 7 : 0));
      put(Entry, get(Entry, Width) & ~(Case == 15 || Case == 16 ? 2 : 1),
          Width);
    }
    if (Case == 17 || Case == 18 || (Case >= 35 && Case <= 38) || Case == 104 ||
        Case == 106) {
      const auto Entry = PTE(Offset);
      put(Entry, get(Entry, Width) & ~(Case != 18 ? 1 : Read ? 2 : 4), Width);
    }

    if (Case >= 108 && Case <= 110) {
      const auto Entry = PTE(DescriptorAddress + (Case == 109 ? 7 : 0));
      put(Entry, get(Entry, Width) & ~uint64_t{2}, Width);
    }

    kvm_regs Registers{};
    Registers.rip = Code;
    Registers.rsp = StackOperand ? Offset + (Push ? Bytes : 0) : UserStack;
    if (Case == 20)
      Registers.rsp |= 0xabcd0000;
    Registers.rax = Form == 4 ? 0xcafebabe : 0xface0000 | Selector;
    Registers.rbx = Offset - 0x20;
    Registers.rsi = 0x20;
    Registers.rdi = Offset;
    Registers.rcx = 0x12345678;
    Registers.rdx = 0x87654321;
    Registers.rbp = 0xabcddcba;
    Registers.rflags = Case == 22 || (Case >= 104 && Case <= 106) ? 0x40002 : 2;
    std::vector<uint8_t> AddressEncoding;
    if (Encoding != UINT32_MAX) {
      // Author independent machine operands from the architectural ModRM/SIB
      // tables. The implementation sees only their bytes and input registers.
      std::array<decltype(Registers.rax) *, 8> GPR{
          &Registers.rax, &Registers.rcx, &Registers.rdx, &Registers.rbx,
          &Registers.rsp, &Registers.rbp, &Registers.rsi, &Registers.rdi};
      for (unsigned I = 0; I != GPR.size(); ++I)
        *GPR[I] = Addr16 ? 0xbeef4400u + I * 16 : 0x30000u + I * 256;
      unsigned Mod = Encoding / 8, RM = Encoding % 8;
      std::optional<unsigned> Base, Index;
      unsigned Scale = 1;
      std::optional<uint8_t> SIB;
      if (Addr16) {
        if (Mod || RM != 6) {
          constexpr unsigned Bases[]{3, 3, 5, 5, 6, 7, 5, 3};
          Base = Bases[RM];
          if (RM < 4)
            Index = RM & 1 ? 7 : 6;
        }
      } else if (Encoding >= 24) {
        unsigned Shape = Encoding - 24;
        const unsigned ScaleBits = Shape % 4;
        Shape /= 4;
        const unsigned IndexBits = Shape % 8;
        Shape /= 8;
        const unsigned BaseBits = Shape % 8;
        Mod = Shape / 8;
        RM = 4;
        SIB = (ScaleBits << 6) | (IndexBits << 3) | BaseBits;
        Scale = 1u << ScaleBits;
        if (IndexBits != 4)
          Index = IndexBits;
        if (Mod || BaseBits != 5)
          Base = BaseBits;
      } else if (Mod || RM != 5) {
        Base = RM;
        if (RM == 4)
          SIB = 0x24;
      }
      const int32_t Displacement = !Base      ? 0xd000
                                   : Mod == 1 ? -7
                                   : Mod == 2 ? -0x1234
                                              : 0;
      const unsigned DispBytes = !Base      ? (Addr16 ? 2 : 4)
                                 : Mod == 1 ? 1
                                 : Mod == 2 ? (Addr16 ? 2 : 4)
                                            : 0;
      AddressEncoding.push_back((Mod << 6) | RM);
      if (SIB)
        AddressEncoding.push_back(*SIB);
      for (unsigned I = 0; I != DispBytes; ++I)
        AddressEncoding.push_back(uint32_t(Displacement) >> (I * 8));
      uint32_t EA = Displacement;
      if (Base)
        EA += *GPR[*Base];
      if (Index)
        EA += *GPR[*Index] * Scale;
      if (Addr16)
        EA = uint16_t(EA);
      const unsigned Source = SegmentOverride >= 0 ? unsigned(SegmentOverride)
                              : Base == 4 || Base == 5 ? 2
                                                       : 3;
      std::array<kvm_segment *, 6> SourceCaches{&Special.es, &Special.cs,
                                                &Special.ss, &Special.ds,
                                                &Special.fs, &Special.gs};
      SourceCaches[Source]->base = Source == 1 ? 0 : 0x1000;
      Offset = EA + SourceCaches[Source]->base;
      Put(Offset, Far ? 0x89abcdef : Selector, Far ? Bytes : 2);
      if (Far)
        Put(Offset + Bytes, Selector, 2);
    }
    const auto OriginalRegisters = Registers;
    std::vector<uint8_t> Instruction;
    if ((Bytes == 4) != Code32)
      Instruction.push_back(0x66);
    if (Addr16 == Code32)
      Instruction.push_back(0x67);
    if (SegmentOverride >= 0) {
      constexpr uint8_t Prefixes[]{0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65};
      Instruction.push_back(Prefixes[SegmentOverride]);
    }
    const auto AppendMemory = [&](unsigned Reg) {
      if (AddressEncoding.empty()) {
        Instruction.push_back(Reg * 8 | (Addr16 ? 0 : 7));
      } else {
        Instruction.push_back(Reg * 8 | AddressEncoding[0]);
        Instruction.insert(Instruction.end(), AddressEncoding.begin() + 1,
                           AddressEncoding.end());
      }
    };
    const auto Extended = [&](uint8_t Opcode) {
      Instruction.push_back(0x0f);
      Instruction.push_back(Opcode);
    };
    if (StackOperand) {
      if (SegmentIndex >= 4)
        Extended((SegmentIndex == 4 ? 0xa0 : 0xa8) | (Pop ? 1 : 0));
      else
        Instruction.push_back(6 + SegmentIndex * 8 + (Pop ? 1 : 0));
    } else if (Far) {
      if (SegmentIndex == 0 || SegmentIndex == 3)
        Instruction.push_back(SegmentIndex == 0 ? 0xc4 : 0xc5);
      else
        Extended(SegmentIndex == 2 ? 0xb2 : SegmentIndex == 4 ? 0xb4 : 0xb5);
      AppendMemory(7);
    } else {
      Instruction.push_back(Read ? 0x8c : 0x8e);
      if (Register)
        Instruction.push_back(0xc0 | SegmentIndex * 8);
      else
        AppendMemory(SegmentIndex);
    }
    std::copy(Instruction.begin(), Instruction.end(), RAM + Code);
    RAM[Code + Instruction.size()] = 0x90;
    // A harmless following instruction crosses MOV/POP SS debug inhibition.
    const uint64_t SuccessStop = Code + Instruction.size() + 1;

    X86LegacySegmentState State;
    State.CPL = CPL;
    State.GPR = {uint32_t(Registers.rax), uint32_t(Registers.rcx),
                 uint32_t(Registers.rdx), uint32_t(Registers.rbx),
                 uint32_t(Registers.rsp), uint32_t(Registers.rbp),
                 uint32_t(Registers.rsi), uint32_t(Registers.rdi)};
    std::array<kvm_segment *, 6> Segments{&Special.es, &Special.cs,
                                          &Special.ss, &Special.ds,
                                          &Special.fs, &Special.gs};
    const auto Cache = [](const kvm_segment &C) {
      return X86LegacySegmentCache{uint32_t(C.base), C.limit, C.selector,
                                   uint16_t(C.type | C.s << 4 | C.dpl << 5 |
                                            C.present << 7 | C.avl << 12 |
                                            C.l << 13 | C.db << 14 | C.g << 15),
                                   bool(C.unusable)};
    };
    for (unsigned I = 0; I != Segments.size(); ++I)
      State.Segments[I] = Cache(*Segments[I]);
    State.GDTRBase = GDT;
    State.GDTRLimit = GDTLimit;
    State.LDTR = Cache(Special.ldt);
    const auto OriginalState = State;
    X86PagingState Controls{.CR0 = Special.cr0,
                            .CR3 = Special.cr3,
                            .CR4 = Special.cr4,
                            .EFER = Special.efer,
                            .RFlags = Registers.rflags,
                            .PDPTE = {0x11001, 0, 0, 0x20001}};
    auto Logical =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Logical.Memory.push_back({0,
                              MemoryPermission::Read | MemoryPermission::Write,
                              0, std::vector<uint8_t>(RAM, RAM + MemorySize)});
    auto Memory =
        llvm::cantFail(X86PagedMemory::create(Logical, Features, Controls));
    auto Expected = executeX86LegacySegmentInstruction(
        State, *Memory, Instruction,
        {X86LegacySegmentProfile::WordStackOffsetFirst, Alignment, {}});
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
    Debug.arch.debugreg[0] = SuccessStop;
    Debug.arch.debugreg[1] = FaultStop;
    Debug.arch.debugreg[7] = 0x405;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    if (Expected->Exception) {
      ASSERT_EQ(Registers.rip, FaultStop);
      EXPECT_EQ(get(Registers.rsp, 4), Expected->Exception->Vector);
      EXPECT_EQ(get(Registers.rsp + 4, 4), Expected->Exception->ErrorCode);
      EXPECT_EQ(Special.cr2, Expected->Exception->CR2.value_or(0x76543210));
      EXPECT_EQ(State.GPR, OriginalState.GPR);
      EXPECT_FALSE(Expected->LoadsSSWithInhibition);
      if (CPL) {
        EXPECT_EQ(get(Registers.rsp + 20, 4), OriginalRegisters.rsp);
        EXPECT_EQ(get(Registers.rsp + 24, 4) & 0xffff,
                  OriginalState.Segments[2].Selector);
      } else {
        EXPECT_EQ(Registers.rsp + 20, OriginalRegisters.rsp);
      }
      for (unsigned I = 0; I != State.Segments.size(); ++I) {
        EXPECT_EQ(State.Segments[I].Selector,
                  OriginalState.Segments[I].Selector);
        EXPECT_EQ(State.Segments[I].Base, OriginalState.Segments[I].Base);
        EXPECT_EQ(State.Segments[I].Limit, OriginalState.Segments[I].Limit);
        EXPECT_EQ(State.Segments[I].Attributes,
                  OriginalState.Segments[I].Attributes);
        EXPECT_EQ(State.Segments[I].Unusable,
                  OriginalState.Segments[I].Unusable);
      }
    } else {
      ASSERT_EQ(Registers.rip, SuccessStop);
      EXPECT_EQ(State.GPR[4], uint32_t(Registers.rsp));
      EXPECT_EQ(Expected->LoadsSSWithInhibition,
                !Read && !Far && SegmentIndex == 2);
    }
    for (unsigned I = 0; I != Segments.size(); ++I) {
      // The fault handler changes CS, and changes SS only on an outer-ring
      // entry. All other cache fields remain directly observable in hardware.
      if (Expected->Exception && (I == 1 || (I == 2 && CPL)))
        continue;
      const auto Native = Cache(*Segments[I]);
      const auto &Bound = State.Segments[I];
      EXPECT_EQ(Bound.Selector, Native.Selector);
      EXPECT_EQ(Bound.Unusable, Native.Unusable);
      if (!Bound.Unusable) {
        EXPECT_EQ(Bound.Base, Native.Base);
        EXPECT_EQ(Bound.Limit, Native.Limit);
        EXPECT_EQ(Bound.Attributes, Native.Attributes);
      }
    }
    if (Encoding != UINT32_MAX && !Expected->Exception) {
      if (!Read)
        EXPECT_EQ(Segments[SegmentIndex]->selector, Selector);
      else {
        const uint16_t Stored =
            RAM[Physical(Offset)] | uint16_t(RAM[Physical(Offset + 1)]) << 8;
        EXPECT_EQ(Stored, OriginalState.Segments[SegmentIndex].Selector);
      }
    }
    const std::array<uint32_t, 8> Actual{uint32_t(Registers.rax),
                                         uint32_t(Registers.rcx),
                                         uint32_t(Registers.rdx),
                                         uint32_t(Registers.rbx),
                                         State.GPR[4],
                                         uint32_t(Registers.rbp),
                                         uint32_t(Registers.rsi),
                                         uint32_t(Registers.rdi)};
    EXPECT_EQ(State.GPR, Actual);
    // Compare the complete descriptor and explicit operand, including bytes
    // adjacent to a word-only PUSH, without exception-frame writes.
    for (uint32_t Address : {DescriptorAddress, Offset}) {
      if (Address == Offset && StackOperand && CPL == 0 && Expected->Exception)
        continue; // Fault delivery writes the same supervisor stack.
      for (unsigned I = 0; I != 8; ++I) {
        const auto P = Physical(Address + I);
        EXPECT_EQ(
            Memory->physicalMemory().loadScalar(P, GuestScalarWidth::I8).Value,
            RAM[P]);
      }
    }
    ++Comparisons;
  }
};

TEST_P(X86LegacySegmentLoadNativeTest,
       OriginalSelectorTransfersMatchProtectedState) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  for (unsigned Segment = 0; Segment != 6; ++Segment)
    for (unsigned Form = 0; Form != 7; ++Form)
      for (unsigned Bytes : {2u, 4u})
        for (bool Addr16 : {false, true})
          for (unsigned CPL : {0u, 1u, 2u, 3u})
            for (unsigned Case = 0; Case != 40; ++Case)
              ASSERT_NO_FATAL_FAILURE(
                  compare(Segment, Form, Bytes, Addr16, true, CPL, Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacySegmentLoadNativeTest,
       OriginalCode16UsesIndependentOperandAndAddressSizes) {
  for (unsigned Segment = 0; Segment != 6; ++Segment)
    for (unsigned Form = 0; Form != 7; ++Form)
      for (unsigned Bytes : {2u, 4u})
        for (bool Addr16 : {false, true})
          ASSERT_NO_FATAL_FAILURE(
              compare(Segment, Form, Bytes, Addr16, false, 3, 0));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacySegmentLoadNativeTest,
       OriginalModRMAndSIBKeepNonflatSourceCaches) {
  for (bool Addr16 : {false, true})
    for (unsigned Encoding = 0; Encoding != (Addr16 ? 24u : 792u); ++Encoding)
      for (unsigned Form : {1u, 3u, 5u})
        for (unsigned Bytes : {2u, 4u})
          ASSERT_NO_FATAL_FAILURE(
              compare(3, Form, Bytes, Addr16, true, 3, 0, Encoding));
  for (int Segment = 0; Segment != 6; ++Segment)
    for (bool Addr16 : {false, true})
      for (unsigned Form : {1u, 3u, 5u})
        for (unsigned Bytes : {2u, 4u})
          for (unsigned Encoding : {0u, 4u, 5u, 6u, 12u, 21u})
            ASSERT_NO_FATAL_FAILURE(compare(Segment == 1 ? 4 : Segment, Form,
                                            Bytes, Addr16, true, 3, 0, Encoding,
                                            Segment));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacySegmentLoadNativeTest,
       OriginalPrivilegeChecksCoverEveryCPLRPLDPL) {
  for (unsigned Segment : {0u, 2u, 3u, 4u, 5u})
    for (unsigned CPL = 0; CPL != 4; ++CPL)
      for (unsigned Case = 40; Case != 104; ++Case)
        ASSERT_NO_FATAL_FAILURE(compare(Segment, 0, 4, false, true, CPL, Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacySegmentLoadNativeTest,
       OriginalAlignmentFaultPriorityUsesTheBoundCore) {
  const char *Binding = std::getenv("NEVERD_TEST_LEGACY_ALIGNMENT");
  if (!Binding)
    GTEST_SKIP() << "Bind NEVERD_TEST_LEGACY_ALIGNMENT=before or after and pin "
                    "the qualified core";
  ASSERT_TRUE(std::strcmp(Binding, "before") == 0 ||
              std::strcmp(Binding, "after") == 0);
  const auto Priority = std::strcmp(Binding, "before") == 0
                            ? X86AlignmentPriority::BeforePaging
                            : X86AlignmentPriority::AfterPaging;
  for (unsigned Segment = 0; Segment != 6; ++Segment)
    for (unsigned Form : {1u, 2u, 3u, 5u, 6u})
      for (unsigned Bytes : {2u, 4u})
        for (bool Addr16 : {false, true})
          for (unsigned Case : {104u, 105u, 106u})
            ASSERT_NO_FATAL_FAILURE(compare(Segment, Form, Bytes, Addr16, true,
                                            3, Case, UINT32_MAX, -1, Priority));
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("alignment_priority", Binding);
}

TEST_P(X86LegacySegmentLoadNativeTest,
       OriginalAccessedWriteSpansBothDescriptorPages) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  for (unsigned Segment : {0u, 2u, 3u, 4u, 5u})
    for (unsigned Form : {0u, 1u, 2u, 3u})
      for (unsigned Bytes : {2u, 4u})
        for (bool Addr16 : {false, true})
          for (unsigned CPL = 0; CPL != 4; ++CPL)
            for (unsigned Case : {108u, 109u, 110u})
              ASSERT_NO_FATAL_FAILURE(
                  compare(Segment, Form, Bytes, Addr16, true, CPL, Case));
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacySegmentLoads, X86LegacySegmentLoadNativeTest,
                         testing::Values(2u, 3u));
