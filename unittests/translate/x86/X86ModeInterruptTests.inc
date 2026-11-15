//===- X86ModeInterruptTests.inc - External mixed-mode events ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Selected IRQ/NMI acceptance, inhibition, wake and retained frame work.
//===----------------------------------------------------------------------===//

class X86ModeInterruptTest : public X86ModeExecutionTest {
protected:
  using Mode = X86ExecutionMode;
  static constexpr Mode Modes[]{Mode::Protected16, Mode::Protected32,
                                Mode::Compatibility16, Mode::Compatibility32,
                                Mode::Long64};

  void start(Mode M) {
    resetMode();
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    R.RFlags = 0x202;
    if (M == Mode::Protected16 || M == Mode::Compatibility16)
      V.Core.CS.Attributes &= ~0x4000;
    if (M != Mode::Protected16 && M != Mode::Protected32)
      Controls = {0x80010031, 0x10000, 0x20, 0x500, 2};
    else
      for (unsigned Vector : {2u, 11u, 14u, 64u})
        put(0x9000 + Vector * 8, (0x4000 + Vector * 16) | (uint64_t(8) << 16) |
                                     (uint64_t(0x8e) << 40));
    if (M == Mode::Long64)
      V.Core.CS = {0, UINT32_MAX, 24, 0xa09b};
    V.Core.Core.CR2 = 0xabcdef0012345678ULL;
    for (unsigned I = 0; I != 8; ++I) {
      R.GPR[I] |= uint64_t(0xdead0000 + I) << 32;
      R.Undefined[I] = uint64_t(0x12340000 + I) << 32;
    }
    R.Undefined[4] = 0;
    if (M == Mode::Long64)
      R.GPR[4] = 0xf000;
    R.GPR[15] = 0x1111222233334444ULL;
  }

  void bindMemory() {
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  }

  std::vector<uint8_t> bytes(X86PagedMemory &M) {
    std::vector<uint8_t> Bytes(0x40000);
    for (const auto &Region : M.physicalMemory().snapshotMemoryRegions())
      std::copy(Region.Bytes.begin(), Region.Bytes.end(),
                Bytes.begin() + Region.Address);
    return Bytes;
  }

  uint64_t word(const std::vector<uint8_t> &Bytes, uint64_t Address,
                unsigned Width) {
    uint64_t Value = 0;
    for (unsigned I = 0; I != Width; ++I)
      Value |= uint64_t(Bytes[Address + I]) << (I * 8);
    return Value;
  }

  void unchanged(const X86ModeState &Before, const X86PagingState &OldControls,
                 const std::vector<uint8_t> &OldBytes) {
    EXPECT_EQ(Machine.Core.Core, Before.Core.Core);
    EXPECT_EQ(Machine.Core.SysenterCS, Before.Core.SysenterCS);
    EXPECT_EQ(Machine.Core.SysenterESP, Before.Core.SysenterESP);
    EXPECT_EQ(Machine.Core.SysenterEIP, Before.Core.SysenterEIP);
    EXPECT_EQ(Machine.Core.Reserved, Before.Core.Reserved);
    EXPECT_EQ(Machine.Debug, Before.Debug);
    EXPECT_EQ(Machine.STIBlocked, Before.STIBlocked);
    EXPECT_EQ(Machine.SSBlocked, Before.SSBlocked);
    EXPECT_EQ(Machine.Halted, Before.Halted);
    EXPECT_EQ(Machine.Shutdown, Before.Shutdown);
    EXPECT_EQ(Machine.Pending, Before.Pending);
    EXPECT_EQ(Memory->controls(), OldControls);
    EXPECT_EQ(bytes(*Memory), OldBytes);
  }

#if defined(__linux__) && defined(__x86_64__)
  void initializeOriginal(X64PagedNativeCPU &Native) {
    const auto &V = Machine.Core.Core;
    const auto &R = V.Core.Core.Core.Core;
    std::copy(RAM.Memory.front().Bytes.begin(), RAM.Memory.front().Bytes.end(),
              Native.RAM);
    auto S = Native.Initial;
    S.cs = X64PagedNativeCPU::segment(V.Core.CS);
    S.ss = S.ds = S.es = S.fs = S.gs = X64PagedNativeCPU::segment(V.Core.SS);
    S.gdt.base = V.GDTR.Base;
    S.gdt.limit = V.GDTR.Limit;
    S.idt.base = V.IDTR.Base;
    S.idt.limit = V.IDTR.Limit;
    S.tr = X64PagedNativeCPU::segment(V.TR);
    S.ldt.unusable = 1;
    S.cr0 = Controls.CR0;
    S.cr3 = Controls.CR3;
    S.cr4 = Controls.CR4;
    S.efer = Controls.EFER;
    S.cr2 = V.Core.Core.CR2;
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_SREGS, &S), 0) << errno;
    kvm_regs Registers{};
    Registers.rip = R.PC;
    Registers.rflags = R.RFlags;
    Registers.rbx = R.GPR[3];
    Registers.rsp = R.GPR[4];
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &Registers), 0) << errno;
  }

  void compareOriginal(X64PagedNativeCPU &Native, bool HandlerHLT) {
    auto &V = Machine.Core.Core;
    const auto &R = V.Core.Core.Core.Core;
    kvm_regs Registers{};
    kvm_sregs S{};
    kvm_vcpu_events Events{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Registers), 0) << errno;
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &S), 0) << errno;
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_VCPU_EVENTS, &Events), 0) << errno;
    // Handler entry comparison stops before its terminal HLT in the model.
    // Return-to-compatibility comparisons execute the original HLT on both.
    EXPECT_EQ(R.PC + HandlerHLT, Registers.rip);
    EXPECT_EQ(R.RFlags, Registers.rflags);
    EXPECT_EQ(Memory->controls().CR0, S.cr0);
    EXPECT_EQ(Memory->controls().CR3, S.cr3);
    EXPECT_EQ(Memory->controls().CR4, S.cr4);
    EXPECT_EQ(Memory->controls().EFER, S.efer);
    EXPECT_EQ(V.Core.Core.CR2, S.cr2);
    EXPECT_EQ(V.Core.CS.Selector, S.cs.selector);
    EXPECT_EQ(V.Core.CS.Base, S.cs.base);
    EXPECT_EQ(V.Core.CS.Limit, S.cs.limit);
    EXPECT_EQ(V.Core.SS.Selector, S.ss.selector);
    EXPECT_EQ(V.Core.SS.Base, S.ss.base);
    EXPECT_EQ(V.Core.SS.Limit, S.ss.limit);
    const auto Attributes = [](const kvm_segment &Segment) {
      return uint16_t(Segment.type | (Segment.s << 4) | (Segment.dpl << 5) |
                      (Segment.present << 7) | (Segment.avl << 12) |
                      (Segment.l << 13) | (Segment.db << 14) |
                      (Segment.g << 15));
    };
    EXPECT_EQ(V.Core.CS.Attributes, Attributes(S.cs));
    EXPECT_EQ(V.Core.SS.Attributes, Attributes(S.ss));
    EXPECT_EQ(bool(V.Unusable & 1), bool(S.cs.unusable));
    EXPECT_EQ(bool(V.Unusable & 2), bool(S.ss.unusable));
    EXPECT_EQ(V.GDTR.Base, S.gdt.base);
    EXPECT_EQ(V.GDTR.Limit, S.gdt.limit);
    EXPECT_EQ(V.IDTR.Base, S.idt.base);
    EXPECT_EQ(V.IDTR.Limit, S.idt.limit);
    EXPECT_EQ(V.TR.Base, S.tr.base);
    EXPECT_EQ(V.TR.Limit, S.tr.limit);
    EXPECT_EQ(V.TR.Selector, S.tr.selector);
    EXPECT_EQ(V.NMIBlocked, Events.nmi.masked);
    EXPECT_EQ(Machine.STIBlocked,
              bool(Events.interrupt.shadow & KVM_X86_SHADOW_INT_STI));
    EXPECT_EQ(Machine.SSBlocked,
              bool(Events.interrupt.shadow & KVM_X86_SHADOW_INT_MOV_SS));
    const std::array<uint64_t, 8> NativeGPR{
        Registers.rax, Registers.rcx, Registers.rdx, Registers.rbx,
        Registers.rsp, Registers.rbp, Registers.rsi, Registers.rdi};
    // Non64 GPR high halves and banks lost across LMA use explicit model
    // witnesses, not an assertion that this CPU chose the same arbitrary bits.
    for (unsigned I = 0; I != 8; ++I)
      EXPECT_EQ(uint32_t(R.GPR[I]), uint32_t(NativeGPR[I]));
    EXPECT_EQ(
        std::memcmp(bytes(*Memory).data(), Native.RAM, X64PagedNativeCPU::Size),
        0);
  }

  void injectOriginal(X64PagedNativeCPU &Native, bool NMI, uint8_t Vector) {
    if (NMI) {
      ASSERT_EQ(ioctl(Native.CPU, KVM_NMI, 0), 0) << errno;
    } else {
      kvm_interrupt Input{Vector};
      ASSERT_EQ(ioctl(Native.CPU, KVM_INTERRUPT, &Input), 0) << errno;
    }
    ASSERT_NO_FATAL_FAILURE(Native.run());
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
  }
#endif
};

TEST_F(X86ModeInterruptTest, FramesUseCommittedModeAndExplicitEventClass) {
  for (auto M : Modes)
    for (unsigned Case = 0; Case != 5; ++Case) {
      SCOPED_TRACE(unsigned(M));
      SCOPED_TRACE(Case);
      start(M);
      auto &V = Machine.Core.Core;
      auto &R = V.Core.Core.Core.Core;
      R.PC = 0x1234;
      if (M != Mode::Long64)
        V.Core.CS.Base = 0x20000;
      const uint8_t Vector = Case == 1 ? 14 : (Case == 2 || Case == 4) ? 64 : 2;
      V.NMIBlocked = Case == 4;
      const auto Kind = Case == 3 ? X86EventKind::NMI : X86EventKind::Interrupt;
      bindMemory();
      X86ModeExecution Execution(*Memory, Options);
      auto Got = Execution.interrupt(Machine, Kind, Vector);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
      ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_TRUE(Got->EventAccepted);
      EXPECT_EQ(Got->Attempts, 0u);
      EXPECT_EQ(Got->Completions, 0u);
      EXPECT_FALSE(Got->InstructionAttempted);
      EXPECT_FALSE(Got->InstructionCompleted);
      EXPECT_FALSE(Machine.Pending);
      EXPECT_EQ(V.NMIBlocked, Case >= 3);
      EXPECT_EQ(R.PC, 0x4000u + Vector * 16);
      EXPECT_EQ(V.Core.Core.CR2, 0xabcdef0012345678ULL);
      EXPECT_EQ(R.GPR[15], 0x1111222233334444ULL);
      const bool Wide = Memory->controls().EFER & 0x400;
      const unsigned Width = Wide ? 8 : 4;
      const uint64_t SP = Wide ? 0xefd8 : 0xeff4;
      EXPECT_EQ(uint32_t(R.GPR[4]), SP);
      const auto ResultBytes = bytes(*Memory);
      EXPECT_EQ(word(ResultBytes, SP, Width), 0x1234u);
      EXPECT_EQ(word(ResultBytes, SP + Width, Width),
                M == Mode::Long64 ? 24 : 8);
      EXPECT_EQ(word(ResultBytes, SP + 2 * Width, Width), 0x202u);
      if (Wide)
        EXPECT_EQ(word(ResultBytes, SP + 3 * Width, Width), 0xf000u);
    }
}

TEST_F(X86ModeInterruptTest, DeferredAndRefusedInputsHaveNoEffects) {
  for (auto M : Modes)
    for (unsigned Case = 0; Case != 14; ++Case) {
      SCOPED_TRACE(unsigned(M));
      SCOPED_TRACE(Case);
      start(M);
      auto &V = Machine.Core.Core;
      auto &R = V.Core.Core.Core.Core;
      auto Kind =
          Case >= 3 && Case <= 6 ? X86EventKind::NMI : X86EventKind::Interrupt;
      uint8_t Vector = Kind == X86EventKind::NMI ? 2 : 64;
      switch (Case) {
      case 0:
        R.RFlags &= ~0x200;
        Machine.Halted = true;
        break;
      case 1:
        Machine.STIBlocked = true;
        break;
      case 2:
      case 4:
        Machine.SSBlocked = true;
        break;
      case 3:
        V.NMIBlocked = 1;
        break;
      case 5:
      case 6:
        Machine.STIBlocked = true;
        if (Case == 5)
          Options.Legacy.NMIBlockedBySTI = true;
        break;
      case 7:
        Kind = X86EventKind::Fault;
        break;
      case 8:
        Kind = X86EventKind::NMI;
        Vector = 3;
        break;
      case 9:
        Machine.Halted = true;
        V.Core.CS.Selector |= 3;
        break;
      case 10:
        Machine.Halted = Machine.SSBlocked = true;
        break;
      case 11:
        V.NMIBlocked = 2;
        break;
      case 12:
        Machine.STIBlocked = true;
        R.RFlags &= ~0x200;
        break;
      case 13:
        Options.MXCSRMask = 0;
        break;
      }
      bindMemory();
      const auto Before = Machine;
      const auto BeforeBytes = bytes(*Memory);
      X86ModeExecution Execution(*Memory, Options);
      auto Got = Execution.interrupt(Machine, Kind, Vector);
      if (Case < 6) {
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDeferred);
        EXPECT_FALSE(Got->EventAccepted);
      } else {
        ASSERT_FALSE(bool(Got));
        llvm::consumeError(Got.takeError());
      }
      unchanged(Before, Controls, BeforeBytes);
    }
}

TEST_F(X86ModeInterruptTest, CompatibilityWitnessesOwnSameLevelAndISTStacks) {
  for (auto M : {Mode::Compatibility16, Mode::Compatibility32})
    for (bool IST : {false, true}) {
      start(M);
      auto &V = Machine.Core.Core;
      auto &R = V.Core.Core.Core.Core;
      Machine.Halted = true;
      R.PC = 0x1234;
      R.GPR[8] = 0x5555666677778888ULL;
      R.ZMM[8][3] = 0xab;
      if (IST) {
        put(0x9400, 0x4400 | (uint64_t(24) << 16) | (uint64_t(1) << 32) |
                        (uint64_t(0x8e) << 40));
        put(0xa024, 0x1e000);
      }
      bindMemory();
      X86ModeExecution Execution(*Memory, Options);
      auto Got = Execution.interrupt(Machine, X86EventKind::Interrupt, 64);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_TRUE(Got->EventAccepted);
      EXPECT_FALSE(Machine.Halted);
      const uint64_t SP = (IST ? 0x1e000 : 0xf000) - 40;
      EXPECT_EQ(R.GPR[4], SP);
      EXPECT_EQ(word(bytes(*Memory), SP + 24, 8), 0xf000u);
      for (unsigned I = 0; I != 8; ++I)
        if (I != 4)
          EXPECT_EQ(R.GPR[I] & ~uint64_t(UINT32_MAX), R.Undefined[I]);
      EXPECT_EQ(R.GPR[8], 0x5555666677778888ULL);
      EXPECT_EQ(R.GPR[15], 0x1111222233334444ULL);
      EXPECT_EQ(R.ZMM[8][3], 0xab);
    }
}

TEST_F(X86ModeInterruptTest, SelectedIRQCanPreemptAnInvalidNextFetch) {
  for (auto M : Modes) {
    start(M);
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    V.Core.CS.Limit = 0xfff;
    const uint64_t SavedPC = M == Mode::Long64 ? 0x800000000000ULL : 0x100000;
    R.PC = SavedPC;
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    auto Got = Execution.interrupt(Machine, X86EventKind::Interrupt, 64);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_TRUE(Got->EventAccepted);
    const bool Wide = Memory->controls().EFER & 0x400;
    EXPECT_EQ(word(bytes(*Memory), uint32_t(R.GPR[4]), Wide ? 8 : 4), SavedPC);
  }
}

TEST_F(X86ModeInterruptTest, BoundNMIChoiceCanEnterDuringSTI) {
  for (auto M : Modes) {
    start(M);
    Machine.STIBlocked = true;
    Options.Legacy.NMIBlockedBySTI = false;
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    auto Got = Execution.interrupt(Machine, X86EventKind::NMI, 2);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_TRUE(Got->EventAccepted);
    EXPECT_FALSE(Machine.STIBlocked);
    EXPECT_EQ(Machine.Core.Core.NMIBlocked, 1);
  }
}

TEST_F(X86ModeInterruptTest, AcceptedNMIResumesExactlyOnceWithZeroBudget) {
  for (auto M : Modes)
    for (bool PartialFrame : {false, true}) {
      SCOPED_TRACE(unsigned(M));
      SCOPED_TRACE(PartialFrame);
      start(M);
      Machine.Halted = true;
      const bool Wide = Controls.EFER & 0x400;
      const unsigned Width = Wide ? 8 : 4;
      const uint64_t Hole =
          PartialFrame ? (Wide ? 0xefd8 : 0xeff4) : 0x9000 + 2 * Width * 2;
      const auto Original = RAM.Memory.front().Bytes;
      RAM.Memory = {
          {0, MemoryPermission::Read | MemoryPermission::Write, 0,
           std::vector<uint8_t>(Original.begin(), Original.begin() + Hole)},
          {Hole + Width, MemoryPermission::Read | MemoryPermission::Write, 0,
           std::vector<uint8_t>(Original.begin() + Hole + Width,
                                Original.end())}};
      bindMemory();
      X86ModeExecution Execution(*Memory, Options);
      auto First = Execution.interrupt(Machine, X86EventKind::NMI, 2);
      ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
      ASSERT_EQ(First->Status, X86LegacyExecutionStatus::BackingFault);
      EXPECT_TRUE(First->EventAccepted);
      EXPECT_FALSE(Machine.Halted);
      EXPECT_EQ(Machine.Core.Core.NMIBlocked, 1);
      ASSERT_TRUE(Machine.Pending);
      uint64_t Budget = 0;
      for (unsigned Retry = 0; Retry != 2; ++Retry) {
        X86ModeExecution Rebuilt(*Memory, Options);
        auto Again = Rebuilt.run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
        EXPECT_EQ(Again->Status, X86LegacyExecutionStatus::BackingFault);
        EXPECT_FALSE(Again->EventAccepted);
        EXPECT_EQ(Again->Attempts, 0u);
        EXPECT_EQ(Again->Completions, 0u);
      }
      const auto Before = Machine;
      const auto BeforeControls = Memory->controls();
      const auto BeforeBytes = bytes(*Memory);
      auto Refused = Execution.interrupt(Machine, X86EventKind::Interrupt, 64);
      ASSERT_FALSE(bool(Refused));
      llvm::consumeError(Refused.takeError());
      unchanged(Before, BeforeControls, BeforeBytes);
      auto Changed = Machine;
      Changed.Core.Core.Core.Core.Core.Core.GPR[15] ^= 1;
      auto ChangedResult = Execution.run(Changed, {}, Budget);
      ASSERT_FALSE(bool(ChangedResult));
      llvm::consumeError(ChangedResult.takeError());
      Changed = Machine;
      Changed.Shutdown = true;
      ChangedResult = Execution.run(Changed, {}, Budget);
      ASSERT_FALSE(bool(ChangedResult));
      llvm::consumeError(ChangedResult.takeError());
      unchanged(Before, BeforeControls, BeforeBytes);
      auto Snapshot = RAM;
      Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
      Snapshot.Memory.push_back(
          {Hole, MemoryPermission::Read | MemoryPermission::Write, 0,
           std::vector<uint8_t>(Original.begin() + Hole,
                                Original.begin() + Hole + Width)});
      auto Repaired = llvm::cantFail(
          X86PagedMemory::create(Snapshot, Features, Memory->controls()));
      auto ChangedOptions = Options;
      ChangedOptions.Undefined.reset();
      ChangedOptions.Legacy.Segments.FullLimitChoice.reset();
      X86ModeExecution Rebuilt(*Repaired, ChangedOptions);
      auto Done = Rebuilt.run(Machine, {}, Budget);
      ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
      ASSERT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
      EXPECT_EQ(Done->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_EQ(Done->DeliveredVector, 2);
      EXPECT_FALSE(Done->EventAccepted);
      EXPECT_FALSE(Machine.Pending);
      EXPECT_EQ(Done->Attempts, 0u);
      EXPECT_EQ(Done->Completions, 0u);
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(Machine.Core.Core.NMIBlocked, 1);
    }
}

TEST_F(X86ModeInterruptTest, SecondaryFaultBudgetDoesNotConsumeAnotherIRQ) {
  for (auto M : Modes) {
    start(M);
    const bool Wide = Controls.EFER & 0x400;
    put(0x9000 + 64 * (Wide ? 16 : 8),
        0x4400 | (uint64_t(Wide ? 24 : 8) << 16) | (uint64_t(0x0e) << 40));
    Options.Legacy.EventDelivery.MaxAttempts = 1;
    Options.Long.EventDelivery.MaxAttempts = 1;
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    auto First = Execution.interrupt(Machine, X86EventKind::Interrupt, 64);
    ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
    ASSERT_EQ(First->Status, X86LegacyExecutionStatus::EventBudgetExhausted);
    EXPECT_TRUE(First->EventAccepted);
    ASSERT_TRUE(Machine.Pending);
    uint64_t Budget = 0;
    auto Done = Execution.run(Machine, {}, Budget);
    ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
    ASSERT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
    EXPECT_EQ(Done->Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_EQ(Done->DeliveredVector, 11);
    EXPECT_FALSE(Done->EventAccepted);
    EXPECT_EQ(Done->Attempts, 0u);
    EXPECT_EQ(Done->Completions, 0u);
    EXPECT_FALSE(Machine.Pending);
  }
}

TEST_F(X86ModeInterruptTest, LegacyOwnerRefusalRetainsAcceptedNMI) {
  start(Mode::Protected32);
  Machine.Halted = true;
  put(0x9010, (uint64_t(48) << 16) | (uint64_t(0x85) << 40));
  bindMemory();
  X86ModeExecution Execution(*Memory, Options);
  auto First = Execution.interrupt(Machine, X86EventKind::NMI, 2);
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  EXPECT_FALSE(First->OwnerError.empty());
  EXPECT_TRUE(First->EventAccepted);
  EXPECT_FALSE(Machine.Halted);
  EXPECT_EQ(Machine.Core.Core.NMIBlocked, 1);
  ASSERT_TRUE(Machine.Pending);
  uint64_t Budget = 0;
  auto Again = Execution.run(Machine, {}, Budget);
  ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
  EXPECT_FALSE(Again->OwnerError.empty());
  EXPECT_FALSE(Again->EventAccepted);
  EXPECT_EQ(Again->Attempts, 0u);
  EXPECT_EQ(Again->Completions, 0u);
  EXPECT_TRUE(Machine.Pending);
}

TEST_F(X86ModeInterruptTest, UnboundLegacyOwnerDoesNotPublishTheTrialBank) {
  for (auto M : {Mode::Protected16, Mode::Protected32}) {
    start(M);
    Machine.Halted = true;
    Options.Legacy.Events.reset();
    bindMemory();
    const auto Before = Machine;
    const auto BeforeBytes = bytes(*Memory);
    X86ModeExecution Execution(*Memory, Options);
    auto Got = Execution.interrupt(Machine, X86EventKind::Interrupt, 64);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    EXPECT_FALSE(Got->OwnerError.empty());
    EXPECT_FALSE(Got->EventAccepted);
    unchanged(Before, Controls, BeforeBytes);
  }
}

TEST_F(X86ModeInterruptTest, TaskPolicyCannotBypassCompleteBankOwnership) {
  for (auto M : {Mode::Protected16, Mode::Protected32}) {
    start(M);
    Machine.Halted = true;
    Machine.Debug.emplace();
    Options.Legacy.Debug = X86InstructionDebugProfile::Watchpoints;
    Options.Long.DebugProfile = X86InstructionDebugProfile::Watchpoints;
    Options.Legacy.Tasks.emplace();
    put(0x9010, (uint64_t(48) << 16) | (uint64_t(0x85) << 40));
    bindMemory();
    const auto Before = Machine;
    const auto BeforeBytes = bytes(*Memory);
    X86ModeExecution Execution(*Memory, Options);
    auto Got = Execution.interrupt(Machine, X86EventKind::NMI, 2);
    ASSERT_FALSE(bool(Got));
    const auto Error = llvm::toString(Got.takeError());
    EXPECT_NE(Error.find("task-state"), std::string::npos) << Error;
    unchanged(Before, Controls, BeforeBytes);
  }
}

TEST_F(X86ModeInterruptTest, ShutdownIsTerminalUntilExplicitReset) {
  for (auto M : Modes) {
    SCOPED_TRACE(unsigned(M));
    start(M);
    Machine.Core.Core.IDTR.Limit = 0;
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    auto First = Execution.interrupt(Machine, X86EventKind::Interrupt, 64);
    ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
    EXPECT_EQ(First->Status, X86LegacyExecutionStatus::Shutdown);
    EXPECT_TRUE(First->EventAccepted);
    ASSERT_TRUE(Machine.Shutdown);
    EXPECT_FALSE(Machine.Pending);
    const auto Before = Machine;
    const auto BeforeControls = Memory->controls();
    const auto BeforeBytes = bytes(*Memory);
    auto Refused = Execution.interrupt(Machine, X86EventKind::NMI, 2);
    ASSERT_FALSE(bool(Refused));
    llvm::consumeError(Refused.takeError());
    uint64_t Budget = 7;
    auto Again = Execution.run(Machine, {}, Budget);
    ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
    EXPECT_EQ(Again->Status, X86LegacyExecutionStatus::Shutdown);
    EXPECT_EQ(Again->Attempts, 0u);
    EXPECT_EQ(Again->Completions, 0u);
    EXPECT_EQ(Budget, 7u);
    unchanged(Before, BeforeControls, BeforeBytes);
  }
}

TEST_F(X86ModeInterruptTest, PaidAndUnpaidInstructionsPrecedeExternalInputs) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool Paid : {false, true}) {
    start(Paid ? Mode::Compatibility32 : Mode::Long64);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.PC = 0x1100;
    if (Paid) {
      put(0x8018, 0x00af9a000000ffffULL);
      add(0x1100, Mode::Compatibility32, {0xea, 0, 0x12, 0, 0, 0x18, 0});
    } else {
      put(0xf000, 0x1234);
      put(0xf008, 8);
      add(0x1100, Mode::Long64, {0x48, 0xcb});
      ASSERT_NO_FATAL_FAILURE(compileLong(false, "-O2", NEVERD_TEST_CLANG));
    }
    const uint64_t Address = Paid ? 0x8018 : 0xf000;
    const auto Original = RAM.Memory.front().Bytes;
    RAM.Memory = {
        {0, MemoryPermission::Read | MemoryPermission::Write, 0,
         std::vector<uint8_t>(Original.begin(), Original.begin() + Address)},
        {Address + 8, MemoryPermission::Read | MemoryPermission::Write, 0,
         std::vector<uint8_t>(Original.begin() + Address + 8, Original.end())}};
    if (Paid)
      RAM.Memory.push_back(
          {Address, MemoryPermission::Read, 0,
           std::vector<uint8_t>(Original.begin() + Address,
                                Original.begin() + Address + 8)});
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto First = Execution.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
    ASSERT_EQ(First->Status, X86LegacyExecutionStatus::BackingFault);
    ASSERT_TRUE(Machine.Pending);
    EXPECT_EQ(First->Attempts, unsigned(Paid));
    EXPECT_EQ(Budget, unsigned(!Paid));
    const auto Before = Machine;
    const auto BeforeControls = Memory->controls();
    const auto BeforeBytes = bytes(*Memory);
    for (bool Shutdown : {false, true}) {
      auto Changed = Machine;
      Changed.Halted = !Shutdown;
      Changed.Shutdown = Shutdown;
      auto Got = Execution.run(Changed, {}, Budget);
      ASSERT_FALSE(bool(Got));
      llvm::consumeError(Got.takeError());
      unchanged(Before, BeforeControls, BeforeBytes);
      EXPECT_EQ(Budget, unsigned(!Paid));
    }
    uint64_t ZeroBudget = 0;
    auto Changed = Machine;
    Changed.Core.Core.Core.Core.Core.Core.GPR[15] ^= 1;
    auto ChangedResult = Execution.run(Changed, {}, ZeroBudget);
    ASSERT_FALSE(bool(ChangedResult));
    llvm::consumeError(ChangedResult.takeError());
    EXPECT_EQ(ZeroBudget, 0u);
    unchanged(Before, BeforeControls, BeforeBytes);
    auto ChangedControls = BeforeControls;
    ChangedControls.RFlags ^= 0x200;
    llvm::cantFail(Memory->bindControls(ChangedControls));
    ChangedResult = Execution.run(Machine, {}, ZeroBudget);
    ASSERT_FALSE(bool(ChangedResult));
    llvm::consumeError(ChangedResult.takeError());
    unchanged(Before, ChangedControls, BeforeBytes);
    EXPECT_EQ(ZeroBudget, 0u);
    llvm::cantFail(Memory->bindControls(BeforeControls));
    for (auto Kind : {X86EventKind::Interrupt, X86EventKind::NMI}) {
      auto Got = Execution.interrupt(Machine, Kind,
                                     Kind == X86EventKind::NMI ? 2 : 64);
      ASSERT_FALSE(bool(Got));
      llvm::consumeError(Got.takeError());
      unchanged(Before, BeforeControls, BeforeBytes);
      EXPECT_EQ(Budget, unsigned(!Paid));
    }
    auto Snapshot = RAM;
    Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
    if (Paid)
      std::erase_if(Snapshot.Memory, [&](const auto &Region) {
        return Region.Address == Address;
      });
    Snapshot.Memory.push_back(
        {Address, MemoryPermission::Read | MemoryPermission::Write, 0,
         std::vector<uint8_t>(Original.begin() + Address,
                              Original.begin() + Address + 8)});
    auto Repaired = llvm::cantFail(
        X86PagedMemory::create(Snapshot, Features, BeforeControls));
    X86ModeExecution Resumed(*Repaired, Options);
    auto Done = Resumed.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
    ASSERT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
    EXPECT_EQ(Done->Completions, 1u);
    EXPECT_EQ(Done->Attempts, unsigned(!Paid));
    EXPECT_TRUE(Done->BudgetExhausted);
    EXPECT_EQ(Budget, 0u);
    EXPECT_FALSE(Machine.Pending);
    EXPECT_EQ(R.PC, Paid ? 0x1200u : 0x1234u);
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X86ModeInterruptTest, OriginalModeWritesSelectTheWakeFrame) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Compared = 0;
  for (bool ExitIA32e : {false, true})
    for (bool NMI : {false, true}) {
      SCOPED_TRACE(ExitIA32e);
      SCOPED_TRACE(NMI);
      X64PagedNativeCPU Native;
      ASSERT_NO_FATAL_FAILURE(Native.initialize());
      if (IsSkipped())
        return;
      ASSERT_NO_FATAL_FAILURE(Native.pinForReturnComparison());
      resetMode();
      roundtrip();
      uint64_t PC;
      const auto M = ExitIA32e ? Mode::Protected32 : Mode::Compatibility32;
      const uint8_t Vector = NMI ? 2 : 64;
      if (ExitIA32e) {
        PC = Program.back().Address;
        Program.pop_back();
        put(0x9000 + Vector * 8, (0x4000 + Vector * 16) | (uint64_t(8) << 16) |
                                     (uint64_t(0x8e) << 40));
      } else {
        PC = Program[10].Address;
        Program.resize(10);
      }
      PC = add(PC, M, {0xfb});
      add(PC, M, {0xf4});
      if (ExitIA32e)
        ASSERT_NO_FATAL_FAILURE(compileLong(false, "-O2", NEVERD_TEST_CLANG));
      ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
      ASSERT_NO_FATAL_FAILURE(Native.run());
      ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
      bindMemory();
      X86ModeExecution Execution(*Memory, Options);
      uint64_t Budget = Program.size();
      auto Halt = Execution.run(Machine, Program, Budget);
      ASSERT_TRUE(bool(Halt)) << llvm::toString(Halt.takeError());
      ASSERT_TRUE(Halt->OwnerError.empty()) << Halt->OwnerError;
      ASSERT_EQ(Halt->Status, X86LegacyExecutionStatus::Halted);
      ASSERT_TRUE(Machine.Halted);
      EXPECT_EQ(Budget, 0u);
      ASSERT_NO_FATAL_FAILURE(compareOriginal(Native, false));
      // The only writes after initialization inject the selected event. The
      // original stream owns all mode changes, STI/HLT and the event frame.
      ASSERT_NO_FATAL_FAILURE(injectOriginal(Native, NMI, Vector));
      auto Wake = Execution.interrupt(
          Machine, NMI ? X86EventKind::NMI : X86EventKind::Interrupt, Vector);
      ASSERT_TRUE(bool(Wake)) << llvm::toString(Wake.takeError());
      ASSERT_TRUE(Wake->OwnerError.empty()) << Wake->OwnerError;
      ASSERT_EQ(Wake->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_TRUE(Wake->EventAccepted);
      EXPECT_FALSE(Machine.Halted);
      EXPECT_EQ(Wake->Attempts, 0u);
      EXPECT_EQ(Wake->Completions, 0u);
      ASSERT_NO_FATAL_FAILURE(compareOriginal(Native, true));
      ++Compared;
    }
  RecordProperty("continuous_external_frame_comparisons", Compared);
  RecordProperty("original_initial_sregs_writes", Compared);
  RecordProperty("original_mode_rewrites", 0);
}

TEST_F(X86ModeInterruptTest, OriginalIRETUnblocksASecondNMIWithoutHostChanges) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Compared = 0;
  for (bool LLVM : {false, true}) {
    X64PagedNativeCPU Native;
    ASSERT_NO_FATAL_FAILURE(Native.initialize());
    if (IsSkipped())
      return;
    ASSERT_NO_FATAL_FAILURE(Native.pinForReturnComparison());
    resetMode();
    roundtrip();
    uint64_t PC = Program[10].Address;
    Program.resize(10);
    PC = add(PC, Mode::Compatibility32, {0xfb});
    for (unsigned I = 0; I != 3; ++I)
      PC = add(PC, Mode::Compatibility32, {0xf4});
    add(0x1100, Mode::Long64, {0x48, 0xcf});
    put(0x9020, 0x1100 | (uint64_t(24) << 16) | (uint64_t(0x8e) << 40));
    Options.Long.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
    ASSERT_NO_FATAL_FAILURE(compileLong(LLVM, "-O2", NEVERD_TEST_CLANG));
    ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
    ASSERT_NO_FATAL_FAILURE(Native.run());
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 12;
    auto InitialHalt = Execution.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(InitialHalt)) << llvm::toString(InitialHalt.takeError());
    ASSERT_TRUE(InitialHalt->OwnerError.empty()) << InitialHalt->OwnerError;
    ASSERT_TRUE(Machine.Halted);
    ASSERT_NO_FATAL_FAILURE(compareOriginal(Native, false));
    for (unsigned Event = 0; Event != 2; ++Event) {
      ASSERT_NO_FATAL_FAILURE(injectOriginal(Native, true, 2));
      auto Wake = Execution.interrupt(Machine, X86EventKind::NMI, 2);
      ASSERT_TRUE(bool(Wake)) << llvm::toString(Wake.takeError());
      ASSERT_EQ(Wake->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_TRUE(Wake->EventAccepted);
      EXPECT_EQ(Machine.Core.Core.NMIBlocked, 1);
      Budget = 2;
      auto Returned = Execution.run(Machine, Program, Budget);
      ASSERT_TRUE(bool(Returned)) << llvm::toString(Returned.takeError());
      ASSERT_TRUE(Returned->OwnerError.empty()) << Returned->OwnerError;
      EXPECT_EQ(Returned->Status, X86LegacyExecutionStatus::Halted);
      EXPECT_EQ(Returned->Attempts, 2u);
      EXPECT_EQ(Returned->Completions, 2u);
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(Machine.Core.Core.NMIBlocked, 0);
      EXPECT_TRUE(Machine.Halted);
      ASSERT_NO_FATAL_FAILURE(compareOriginal(Native, false));
      ++Compared;
    }
  }
  RecordProperty("continuous_external_iret_comparisons", Compared);
  RecordProperty("original_initial_sregs_writes", 2);
  RecordProperty("original_mode_rewrites", 0);
}
#endif
