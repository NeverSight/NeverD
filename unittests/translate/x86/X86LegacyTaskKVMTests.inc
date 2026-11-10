//===- X86LegacyTaskKVMTests.inc - Independent software task reference ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// VMX exits task switches to KVM's emulator: this is software-reference
/// evidence, not native CPU task-switch or TSS.T/debug qualification.
//===----------------------------------------------------------------------===//

class X86LegacyTaskKVMTest : public X86PagingNativeTest,
                             public X86LegacyTaskTestState {
protected:
  using X86LegacyTestState::put;
  unsigned Comparisons = 0;

  void compare(unsigned Route, bool RoundTrip, bool Code32) {
    SCOPED_TRACE(testing::Message()
                 << "paging=" << GetParam() << " route=" << Route
                 << " return=" << RoundTrip << " code32=" << Code32);
    taskState();
    S.EFlags = 0x3202; // Permit the OUT stop marker at CPL3.
    put(0x8024, 0x3202, GuestScalarWidth::I32);
    S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
    put(0x7018, Code32 ? 0x00cffb000000ffffULL : 0x008ffb000000ffffULL);
    // Keep the incoming task in code32; returning reloads the original CS.D.
    put(0x7048, 0x00cffb000000ffffULL);
    put(0x804c, 0x4b, GuestScalarWidth::I16);
    auto Controls = M->controls();
    Controls.CR0 |= 0x10;
    Controls.RFlags = S.EFlags;
    const bool PAE = GetParam() == 3;
    if (PAE) {
      auto State =
          llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
      State.Memory = M->physicalMemory().snapshotMemoryRegions();
      Controls.CR4 = 0x20;
      Controls.PDPTE = {0x11001, 0, 0, 0};
      M = llvm::cantFail(X86PagedMemory::create(State, Features, Controls));
      put(0x10000, 0x11001);
      put(0x11000, 0x12007);
      for (unsigned I = 0; I != 32; ++I)
        put(0x12000 + I * 8, I * 4096 | 7);
    } else {
      llvm::cantFail(M->bindControls(Controls));
    }
    std::vector<uint8_t> Bytes{uint8_t(Route == 1 ? 0xea : 0x9a), 0, 0};
    if (Code32)
      Bytes.insert(Bytes.end(), {0, 0});
    Bytes.insert(Bytes.end(), {uint8_t(Route == 2 ? 0x38 : 0x30), 0});
    put(0x7038, (uint64_t{0xe5} << 40) | (uint64_t{48} << 16));
    if (Route == 3) {
      Bytes = {0xcd, 0x40};
      put(0x6200, (uint64_t{0xe5} << 40) | (uint64_t{48} << 16));
    }
    for (unsigned I = 0; I != Bytes.size(); ++I)
      put(0x1000 + I, Bytes[I], GuestScalarWidth::I8);
    put(0x1000 + Bytes.size(), 0x80e6, GuestScalarWidth::I16);
    put(0x3000, RoundTrip ? 0xcf : 0x80e6, GuestScalarWidth::I16);
    const auto Image = M->physicalMemory().snapshotMemoryRegions();
    std::memset(RAM, 0, MemorySize);
    std::copy(Image.front().Bytes.begin(), Image.front().Bytes.end(), RAM);
    const auto Segment = [](const neverd::X86LegacySegmentCache &C) {
      kvm_segment K{};
      K.base = C.Base;
      K.limit = C.Limit;
      K.selector = C.Selector;
      K.type = C.Attributes & 15;
      K.s = (C.Attributes >> 4) & 1;
      K.dpl = (C.Attributes >> 5) & 3;
      K.present = (C.Attributes >> 7) & 1;
      K.avl = (C.Attributes >> 12) & 1;
      K.l = (C.Attributes >> 13) & 1;
      K.db = (C.Attributes >> 14) & 1;
      K.g = (C.Attributes >> 15) & 1;
      K.unusable = C.Unusable;
      return K;
    };
    kvm_sregs Special = Initial;
    std::array<kvm_segment *, 6> Segments{&Special.es, &Special.cs,
                                          &Special.ss, &Special.ds,
                                          &Special.fs, &Special.gs};
    for (unsigned I = 0; I != 6; ++I)
      *Segments[I] = Segment(S.Core.Segments[I]);
    Special.tr = Segment(S.TR);
    Special.ldt = Segment(S.Core.LDTR);
    Special.gdt = {S.Core.GDTRBase, S.Core.GDTRLimit, {}};
    Special.idt = {S.IDTRBase, S.IDTRLimit, {}};
    Special.cr0 = Controls.CR0;
    Special.cr2 = S.CR2;
    Special.cr3 = Controls.CR3;
    Special.cr4 = Controls.CR4;
    Special.efer = 0;
    auto Flush = Special;
    Flush.cr3 = 0x19000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0) << errno;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0) << errno;
    kvm_regs Regs{};
    std::array<__u64 *, 8> GPR{&Regs.rax, &Regs.rcx, &Regs.rdx, &Regs.rbx,
                               &Regs.rsp, &Regs.rbp, &Regs.rsi, &Regs.rdi};
    for (unsigned I = 0; I != 8; ++I)
      *GPR[I] = S.Core.GPR[I];
    Regs.rip = S.EIP;
    Regs.rflags = S.EFlags;
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Regs), 0);
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    kvm_guest_debug GuestDebug{};
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &GuestDebug), 0);
    kvm_debugregs Debug{};
    Debug.dr6 = S.Debug->DR6;
    Debug.dr7 = S.Debug->DR7;
    ASSERT_EQ(ioctl(CPU, KVM_SET_DEBUGREGS, &Debug), 0);
    auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
    ASSERT_TRUE(R.InstructionCompleted);
    if (RoundTrip) {
      R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0xcf}, O));
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
    }
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_IO))
        << Run->fail_entry.hardware_entry_failure_reason;
    ASSERT_EQ(Run->io.port, 0x80);
    Run->immediate_exit = 1;
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), -1);
    ASSERT_EQ(errno, EINTR);
    Run->immediate_exit = 0;
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Regs), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    EXPECT_EQ(Regs.rip - 2, S.EIP);
    EXPECT_EQ(Regs.rflags, S.EFlags);
    for (unsigned I = 0; I != 8; ++I)
      EXPECT_EQ(*GPR[I], S.Core.GPR[I]) << I;
    EXPECT_EQ(Special.tr.selector, S.TR.Selector);
    EXPECT_EQ(Special.tr.base, S.TR.Base);
    EXPECT_EQ(Special.tr.limit, S.TR.Limit);
    EXPECT_EQ(Special.tr.type, S.TR.Attributes & 15);
    EXPECT_EQ(Special.cr0, M->controls().CR0);
    EXPECT_EQ(Special.cr2, S.CR2);
    EXPECT_EQ(Special.cr3, M->controls().CR3);
    EXPECT_EQ(Special.cr4, M->controls().CR4);
    EXPECT_EQ(Special.efer, M->controls().EFER);
    for (unsigned I = 0; I != 6; ++I) {
      EXPECT_EQ(Segments[I]->selector, S.Core.Segments[I].Selector) << I;
      EXPECT_EQ(bool(Segments[I]->unusable), S.Core.Segments[I].Unusable) << I;
      if (!S.Core.Segments[I].Unusable) {
        EXPECT_EQ(Segments[I]->base, S.Core.Segments[I].Base) << I;
        EXPECT_EQ(Segments[I]->limit, S.Core.Segments[I].Limit) << I;
        EXPECT_EQ(Segments[I]->type, S.Core.Segments[I].Attributes & 15) << I;
        EXPECT_EQ(Segments[I]->s, bool(S.Core.Segments[I].Attributes & 0x10))
            << I;
        EXPECT_EQ(Segments[I]->dpl, (S.Core.Segments[I].Attributes >> 5) & 3)
            << I;
        EXPECT_EQ(Segments[I]->present,
                  bool(S.Core.Segments[I].Attributes & 0x80))
            << I;
        EXPECT_EQ(Segments[I]->db, bool(S.Core.Segments[I].Attributes & 0x4000))
            << I;
      }
    }
    const auto After = M->physicalMemory().snapshotMemoryRegions();
    // Compare every ordinary RAM byte, including untouched stack/code and
    // both TSS images. Page-walk A/D timing differs between the declared
    // software profile and KVM; only those bits are excluded in table RAM.
    for (auto Range : {std::pair{0u, 0x10000u}, std::pair{0x13000u, 0x20000u}})
      EXPECT_TRUE(std::equal(RAM + Range.first, RAM + Range.second,
                             After.front().Bytes.begin() + Range.first));
    const unsigned EntryBytes = PAE ? 8 : 4;
    for (unsigned I = 0x10000; I != 0x13000; ++I) {
      const uint8_t Mask = I % EntryBytes ? 0xff : 0x9f;
      ASSERT_EQ(RAM[I] & Mask, After.front().Bytes[I] & Mask) << I;
    }
    ++Comparisons;
  }
};

TEST_P(X86LegacyTaskKVMTest, DwordTSSBranchesGatesAndNestedReturn) {
  for (unsigned Route : {0u, 1u, 2u, 3u})
    for (bool Code32 : {false, true})
      for (bool RoundTrip : {false, true}) {
        if (Route == 1 && RoundTrip)
          continue;
        ASSERT_NO_FATAL_FAILURE(compare(Route, RoundTrip, Code32));
      }
  RecordProperty("software_reference_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(KVMTaskSoftwareReference, X86LegacyTaskKVMTest,
                         testing::Values(2u, 3u));
