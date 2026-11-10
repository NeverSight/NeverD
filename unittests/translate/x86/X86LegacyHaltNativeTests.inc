//===- X86LegacyHaltNativeTests.inc - Original HLT and event boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// KVM comparison of original HLT bytes, privilege faults and IRQ/NMI wake.
//===----------------------------------------------------------------------===//

class X86LegacyHaltNativeTest : public X86PagingNativeTest,
                                public X86LegacyTestState {
protected:
  unsigned Comparisons = 0;

  void compare(llvm::ArrayRef<uint8_t> Bytes, bool Code32, unsigned CPL,
               uint32_t Flags, unsigned Shadow = 0, unsigned Wake = 0,
               bool FetchFault = false, bool BadWakeGate = false,
               uint32_t PC = 0x1000) {
    using neverd::X86LegacySegmentCache;
    SCOPED_TRACE(testing::Message()
                 << "code32=" << Code32 << " cpl=" << CPL
                 << " flags=" << std::hex << Flags << " shadow=" << Shadow
                 << " wake=" << Wake << " fetch_fault=" << FetchFault
                 << " bad_wake_gate=" << BadWakeGate);
    resetLegacyState(Features);
    const bool PAE = GetParam() == 3;
    const unsigned PW = PAE ? 8 : 4;
    const uint32_t PT = PAE ? 0x12000 : 0x11000;
    S.Core.CPL = CPL;
    S.Core.Segments[1] = {0, UINT32_MAX, uint16_t(24 | CPL),
                          uint16_t((Code32 ? 0xc09b : 0x809b) | (CPL << 5)),
                          false};
    S.Core.Segments[2] = {0, UINT32_MAX, uint16_t(32 | CPL),
                          uint16_t(0xc093 | (CPL << 5)), false};
    S.EFlags = Flags;
    S.EIP = PC;
    for (unsigned I = 0; I != 8; ++I)
      if (I != 4)
        S.Core.GPR[I] = 0x87654321u ^ (0x11111111u * I);
    const bool SeedSS = Shadow == KVM_X86_SHADOW_INT_MOV_SS;
    const bool SeedSTI = Shadow == KVM_X86_SHADOW_INT_STI;
    std::vector<uint8_t> Prefix;
    if (SeedSS) {
      ASSERT_EQ(CPL, 0u);
      S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
      S.Core.GPR[0] = 16;
      Prefix = {0x8e, 0xd0};
    } else if (SeedSTI) {
      Prefix = {0xfb};
      S.EFlags &= ~0x200u;
    }
    // Original instructions establish the complete interrupt/debug boundary.
    // In particular, TF traps after STI before HLT can begin. An artificial
    // STI shadow with TF omits that pending cause and can fail VM entry.
    S.EIP -= Prefix.size();
    for (unsigned I = 0; I != Prefix.size(); ++I)
      X86LegacyTestState::put(S.EIP + I, Prefix[I], GuestScalarWidth::I8);
    S.Debug.emplace();
    O.Debug = X86LegacyDebugProfile::InstructionStep;
    auto Controls = M->controls();
    Controls.CR0 |= 0x10;
    Controls.CR4 = PAE ? 0x20 : 0;
    Controls.EFER = PAE ? 0x800 : 0;
    Controls.RFlags = S.EFlags;
    if (PAE) {
      Controls.PDPTE = {0x11001, 0, 0, 0};
      X86LegacyTestState::put(0x10000, 0x11001);
      X86LegacyTestState::put(0x11000, 0x12007);
      for (unsigned I = 0; I != 32; ++I)
        X86LegacyTestState::put(PT + I * PW, I * 4096 | 7);
    }
    llvm::cantFail(M->bindControls(Controls));
    for (unsigned V = 0; V != 256; ++V) {
      const uint32_t HandlerPC = 0x4000 + V * 2;
      X86LegacyTestState::put(
          0x6000 + V * 8,
          HandlerPC | (uint64_t{8} << 16) |
              (uint64_t(BadWakeGate && V == Wake ? 0x0e : 0x8e) << 40));
      X86LegacyTestState::put(HandlerPC, 0x80e6, GuestScalarWidth::I16);
    }
    for (unsigned I = 0; I != Bytes.size(); ++I)
      X86LegacyTestState::put(PC + I, Bytes[I], GuestScalarWidth::I8);
    if (FetchFault)
      X86LegacyTestState::put(
          PT + PW, 0, PAE ? GuestScalarWidth::I64 : GuestScalarWidth::I32);
    const auto Image = M->physicalMemory().snapshotMemoryRegions();
    std::memset(RAM, 0, MemorySize);
    std::copy(Image.front().Bytes.begin(), Image.front().Bytes.end(), RAM);
    kvm_sregs Special = Initial;
    const auto NativeSegment = [](const X86LegacySegmentCache &C) {
      kvm_segment R{};
      R.base = C.Base;
      R.limit = C.Limit;
      R.selector = C.Selector;
      R.type = C.Attributes & 15;
      R.s = (C.Attributes >> 4) & 1;
      R.dpl = (C.Attributes >> 5) & 3;
      R.present = (C.Attributes >> 7) & 1;
      R.avl = (C.Attributes >> 12) & 1;
      R.l = (C.Attributes >> 13) & 1;
      R.db = (C.Attributes >> 14) & 1;
      R.g = (C.Attributes >> 15) & 1;
      R.unusable = C.Unusable;
      return R;
    };
    std::array<kvm_segment *, 6> Segments{&Special.es, &Special.cs,
                                          &Special.ss, &Special.ds,
                                          &Special.fs, &Special.gs};
    for (unsigned I = 0; I != 6; ++I)
      *Segments[I] = NativeSegment(S.Core.Segments[I]);
    Special.ldt = NativeSegment(S.Core.LDTR);
    Special.tr = NativeSegment(S.TR);
    Special.gdt = {S.Core.GDTRBase, S.Core.GDTRLimit, {}};
    Special.idt = {S.IDTRBase, S.IDTRLimit, {}};
    Special.cr0 = Controls.CR0;
    Special.cr2 = S.CR2;
    Special.cr3 = Controls.CR3;
    Special.cr4 = Controls.CR4;
    Special.efer = Controls.EFER;
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
    X86LegacyExecutionResult R;
    if (!Prefix.empty())
      R = llvm::cantFail(executeX86LegacyInstruction(S, *M, Prefix, O));
    if (R.Status == X86LegacyExecutionStatus::Completed)
      R = llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    if (Run->exit_reason == KVM_EXIT_HLT) {
      kvm_vcpu_events Pending{};
      ASSERT_EQ(ioctl(CPU, KVM_GET_VCPU_EVENTS, &Pending), 0);
      // The HLT intercept can return before KVM injects the original TF
      // completion trap. Re-enter only for that independently queued cause.
      if (Pending.exception.pending || Pending.exception.injected) {
        ASSERT_EQ(Pending.exception.nr, 1);
        ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
      }
    }
    ASSERT_EQ(Run->exit_reason, unsigned(S.Halted ? KVM_EXIT_HLT : KVM_EXIT_IO))
        << "VM entry failure reason "
        << Run->fail_entry.hardware_entry_failure_reason;
    ASSERT_TRUE(R.InstructionAttempted);
    if (Wake) {
      ASSERT_TRUE(S.Halted);
      const auto Kind =
          Wake == 2 ? X86LegacyEventKind::NMI : X86LegacyEventKind::Interrupt;
      R = llvm::cantFail(
          deliverX86LegacyEvent(S, *M, {S.EIP, 0, 0, uint8_t(Wake), Kind}, O));
      ASSERT_TRUE(R.EventAccepted);
      if (Wake == 2)
        ASSERT_EQ(ioctl(CPU, KVM_NMI, 0), 0);
      else {
        kvm_interrupt IRQ{Wake};
        ASSERT_EQ(ioctl(CPU, KVM_INTERRUPT, &IRQ), 0);
      }
      ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
      ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_IO));
    }
    const bool Marker = Run->exit_reason == KVM_EXIT_IO;
    if (Marker) {
      ASSERT_EQ(Run->io.direction, KVM_EXIT_IO_OUT);
      ASSERT_EQ(Run->io.port, 0x80);
      ASSERT_EQ(Run->io.size, 1);
      ASSERT_EQ(Run->io.count, 1u);
      // KVM_EXIT_IO has not completed OUT yet. Finish just that operation;
      // immediate_exit prevents another guest instruction from running.
      Run->immediate_exit = 1;
      ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), -1);
      ASSERT_EQ(errno, EINTR);
      Run->immediate_exit = 0;
    }
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Regs), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_VCPU_EVENTS, &Events), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_DEBUGREGS, &Debug), 0);
    // The independent handler only executes OUT imm8, AL as a stop marker.
    EXPECT_EQ(Regs.rip - (Marker ? 2 : 0), S.EIP);
    EXPECT_EQ(Regs.rflags, S.EFlags);
    for (unsigned I = 0; I != 8; ++I)
      EXPECT_EQ(*GPR[I], S.Core.GPR[I]) << I;
    EXPECT_EQ(Special.cr2, S.CR2);
    EXPECT_EQ(bool(Events.nmi.masked), S.NMIBlocked);
    EXPECT_EQ(Events.interrupt.shadow,
              (S.SSBlocked ? KVM_X86_SHADOW_INT_MOV_SS : 0) |
                  (S.STIBlocked ? KVM_X86_SHADOW_INT_STI : 0));
    EXPECT_EQ(Debug.dr6, S.Debug->DR6);
    for (unsigned I = 0; I != 6; ++I) {
      const auto Expected = NativeSegment(S.Core.Segments[I]);
      EXPECT_EQ(Segments[I]->base, Expected.base);
      EXPECT_EQ(Segments[I]->limit, Expected.limit);
      EXPECT_EQ(Segments[I]->selector, Expected.selector);
      EXPECT_EQ(Segments[I]->type, Expected.type);
      EXPECT_EQ(Segments[I]->dpl, Expected.dpl);
      EXPECT_EQ(Segments[I]->db, Expected.db);
      EXPECT_EQ(Segments[I]->unusable, Expected.unusable);
    }
    // Compare both possible exception stacks and the GDT's accessed writes.
    const auto ModelRAM = M->physicalMemory().snapshotMemoryRegions();
    for (auto Range :
         {std::pair{0xcf00u, 0xe000u}, std::pair{0x7000u, 0x7100u}})
      EXPECT_TRUE(std::equal(RAM + Range.first, RAM + Range.second,
                             ModelRAM.front().Bytes.begin() + Range.first));
    EXPECT_EQ(S.Halted, R.Status == X86LegacyExecutionStatus::Halted);
    if (S.Halted)
      EXPECT_EQ(S.EIP, PC + Bytes.size());
    ++Comparisons;
  }
};

TEST_P(X86LegacyHaltNativeTest, OriginalPrefixesPrivilegeAndFetchFaults) {
  for (bool Code32 : {false, true})
    for (const std::vector<uint8_t> B : {std::vector<uint8_t>{0xf4},
                                         {0x66, 0xf4},
                                         {0x67, 0xf4},
                                         {0x64, 0xf4},
                                         {0xf2, 0xf4},
                                         {0xf3, 0xf4},
                                         {0xf0, 0xf4},
                                         {0x2e, 0x66, 0x67, 0xf3, 0xf4}})
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (bool Fault : {false, true})
          ASSERT_NO_FATAL_FAILURE(
              compare(B, Code32, CPL, 0x13202, 0, 0, Fault));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyHaltNativeTest, OriginalFlagsShadowsAndDebugTrap) {
  for (bool Code32 : {false, true})
    for (uint32_t Flags : {2u, 0x202u, 0x302u, 0x10302u, 0x10ed7u})
      for (unsigned Shadow : {0u, unsigned(KVM_X86_SHADOW_INT_STI),
                              unsigned(KVM_X86_SHADOW_INT_MOV_SS)}) {
        if (Shadow == KVM_X86_SHADOW_INT_STI && !(Flags & 0x200))
          continue;
        ASSERT_NO_FATAL_FAILURE(compare({0xf4}, Code32, 0, Flags, Shadow));
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyHaltNativeTest, OriginalIRQAndNMIWakeIncludingEntryFault) {
  for (bool Code32 : {false, true})
    for (unsigned Wake : {2u, 64u})
      for (uint32_t Flags : {2u, 0x202u})
        for (bool BadGate : {false, true}) {
          if (Wake != 2 && !(Flags & 0x200))
            continue;
          ASSERT_NO_FATAL_FAILURE(
              compare({0xf4}, Code32, 0, Flags, 0, Wake, false, BadGate));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyHaltNativeTest, OriginalWordBoundaryKeepsFollowingEIP) {
  for (bool Code32 : {false, true})
    for (const std::vector<uint8_t> Bytes :
         {std::vector<uint8_t>{0xf4}, {0x66, 0xf4}})
      ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, 0, 0x202, 0, 64, false,
                                      false, 0x10000 - Bytes.size()));
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyPaging, X86LegacyHaltNativeTest,
                         testing::Values(2, 3));
