//===- X86ModeStartupTests.inc - Processor INIT and startup IPI -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Test BSP/AP initialization, SMM deferral and original KVM reset images.
//===----------------------------------------------------------------------===//

TEST_F(X86ModeInterruptTest, StartupInputsRequireBindingAndRestartShutdown) {
  start(Mode::Long64);
  Machine.Halted = true;
  bindMemory();
  X86ModeExecution Execution(*Memory, Options);
  X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
  Inputs.INITPending = true;
  const auto Before = Machine;
  const auto OldRAM = bytes(*Memory);
  uint64_t Budget = 3;
  auto Refused = Execution.run(Machine, Program, Budget, Inputs);
  ASSERT_FALSE(Refused);
  llvm::consumeError(Refused.takeError());
  ASSERT_TRUE(Inputs.INITPending);
  unchanged(Before, Controls, OldRAM);
  Machine.Halted = false;
  Machine.Shutdown = true;
  Machine.Startup = X86StartupState{0x906a0, false};
  Inputs.NMIPending = true;
  ASSERT_EQ(Inputs.APIC.receiveFixedInterrupt(64, true, true),
            X86LocalApicArrival::Queued);
  ASSERT_TRUE(llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                  .EventAccepted);
  EXPECT_FALSE(Inputs.INITPending || Inputs.NMIPending);
  EXPECT_FALSE(Machine.Halted || Machine.Shutdown);
  EXPECT_EQ(Inputs.APIC.state(), X86LocalApicFixedState{});
  EXPECT_TRUE(Machine.Startup->WaitingForSIPI);
  EXPECT_EQ(llvm::cantFail(Execution.run(Machine, Program, Budget)).Status,
            X86LegacyExecutionStatus::EventDeferred);
  EXPECT_EQ(
      llvm::cantFail(Execution.interrupt(Machine, X86EventKind::NMI, 2)).Status,
      X86LegacyExecutionStatus::EventDeferred);
  auto &V = Machine.Core.Core;
  auto &R = V.Core.Core.Core.Core;
  EXPECT_EQ(R.PC, 0xfff0u);
  EXPECT_EQ(R.GPR[2], 0x906a0u);
  Inputs.StartupVector = 0;
  ASSERT_TRUE(llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                  .EventAccepted);
  EXPECT_FALSE(Machine.Startup->WaitingForSIPI);
  EXPECT_EQ(V.Core.CS.Base, 0u);
  EXPECT_EQ(R.PC, 0u);
  Inputs.StartupVector = 0xff;
  Budget = 0;
  ASSERT_TRUE(llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                  .BudgetExhausted);
  EXPECT_FALSE(Inputs.StartupVector);
  EXPECT_EQ(V.Core.CS.Base, 0u);
  EXPECT_EQ(bytes(*Memory), OldRAM);
}

TEST_F(X86ModeInterruptTest, StartupInitStaysLatchedInSmmUntilRsmCompletes) {
  start(Mode::Long64);
  Machine.Startup = X86StartupState{0x906a0, true};
  Machine.SMM.emplace();
  Machine.Debug.emplace();
  Options.Legacy.Debug = Options.Long.DebugProfile =
      X86InstructionDebugProfile::Watchpoints;
  auto Firmware = RAM;
  Firmware.Memory.front().Bytes[0x38000] = 0x0f;
  Firmware.Memory.front().Bytes[0x38001] = 0xaa;
  Program = {{0x38000, Mode::Real16, {0x0f, 0xaa}}};
  bindMemory();
  auto System =
      llvm::cantFail(X86PagedMemory::create(Firmware, Features, Controls));
  X86ModeExecution Execution(*Memory, Options, *System);
  ASSERT_TRUE(llvm::cantFail(Execution.systemManagementInterrupt(Machine))
                  .EventAccepted);
  X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
  Inputs.INITPending = true;
  Inputs.StartupVector = 0x71;
  uint64_t Budget = 0;
  ASSERT_TRUE(llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                  .BudgetExhausted);
  EXPECT_TRUE(Machine.SMM->Active && Inputs.INITPending);
  EXPECT_FALSE(Inputs.StartupVector);
  Budget = 1;
  auto Restart =
      llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
  ASSERT_TRUE(Restart.EventAccepted) << Restart.OwnerError;
  EXPECT_EQ(Restart.Attempts, 1u);
  EXPECT_EQ(Restart.Completions, 1u);
  EXPECT_FALSE(Machine.SMM->Active || Inputs.INITPending);
  EXPECT_FALSE(Machine.Startup->WaitingForSIPI);
  EXPECT_EQ(llvm::cantFail(Execution.mode(Machine)), Mode::Real16);
  EXPECT_EQ(Machine.Core.Core.Core.CS.Base, 0xffff0000u);
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X86ModeInterruptTest,
       StartupInitAndSipiMatchKvmBspAndApplicationProcessor) {
  for (unsigned CPUIndex : {0u, 1u}) {
    SCOPED_TRACE(CPUIndex);
    X64PagedNativeCPU Native;
    ASSERT_NO_FATAL_FAILURE(Native.initialize(CPUIndex));
    if (Native.CPU < 0)
      GTEST_SKIP() << "KVM CPU unavailable";
    auto *CPUID =
        reinterpret_cast<const kvm_cpuid2 *>(Native.CPUIDStorage.data());
    auto Entries = llvm::ArrayRef(CPUID->entries, CPUID->nent);
    auto Signature =
        std::find_if(Entries.begin(), Entries.end(),
                     [](const auto &E) { return E.function == 1; });
    ASSERT_NE(Signature, Entries.end());
    start(Mode::Long64);
    Machine.Startup = X86StartupState{Signature->eax, CPUIndex == 0};
    Machine.Debug.emplace();
    Options.Legacy.Debug = Options.Long.DebugProfile =
        X86InstructionDebugProfile::Watchpoints;
    Controls.CR0 |= 0x60000000;
    ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
    std::vector<uint8_t> ResetPage(4096);
    ResetPage[0xff0] = 0xe6;
    ResetPage[0xff1] = 0x80;
    ASSERT_NO_FATAL_FAILURE(Native.addMemory(0xfffff000, ResetPage));
    constexpr uint8_t Vector = 0x37;
    Native.RAM[Vector << 12] = 0xe6;
    Native.RAM[(Vector << 12) + 1] = 0x80;
    kvm_mp_state MP{KVM_MP_STATE_RUNNABLE};
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_MP_STATE, &MP), 0);
    kvm_vcpu_events Events{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_VCPU_EVENTS, &Events), 0);
    Events.flags = KVM_VCPUEVENT_VALID_SMM | KVM_VCPUEVENT_VALID_SIPI_VECTOR;
    Events.smi.latched_init = 1;
    Events.sipi_vector = Vector;
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    if (CPUIndex) {
      MP.mp_state = KVM_MP_STATE_SIPI_RECEIVED;
      ASSERT_EQ(ioctl(Native.CPU, KVM_SET_MP_STATE, &MP), 0);
    }
    ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_IO));
    ASSERT_EQ(Native.Run->io.port, 0x80u);

    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    bindMemory();
    X86ModeExecution Exec(*Memory, Options);
    X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
    Inputs.INITPending = true;
    uint64_t Budget = 0;
    ASSERT_TRUE(llvm::cantFail(Exec.run(Machine, Program, Budget, Inputs))
                    .EventAccepted);
    EXPECT_EQ(Machine.Startup->WaitingForSIPI, CPUIndex != 0);
    if (CPUIndex) {
      Inputs.StartupVector = Vector;
      ASSERT_TRUE(llvm::cantFail(Exec.run(Machine, Program, Budget, Inputs))
                      .EventAccepted);
    }
    kvm_regs Registers{};
    kvm_sregs System{};
    kvm_debugregs Debug{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &System), 0);
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_DEBUGREGS, &Debug), 0);
    const uint64_t GPR[]{
        Registers.rax, Registers.rcx, Registers.rdx, Registers.rbx,
        Registers.rsp, Registers.rbp, Registers.rsi, Registers.rdi,
        Registers.r8,  Registers.r9,  Registers.r10, Registers.r11,
        Registers.r12, Registers.r13, Registers.r14, Registers.r15};
    EXPECT_EQ(llvm::ArrayRef(R.GPR), llvm::ArrayRef(GPR));
    EXPECT_EQ(R.PC, Registers.rip);
    EXPECT_EQ(R.RFlags, Registers.rflags);
    const auto &C = Memory->controls();
    EXPECT_EQ(C.CR0, System.cr0);
    EXPECT_EQ(V.Core.Core.CR2, System.cr2);
    EXPECT_EQ(C.CR3, System.cr3);
    EXPECT_EQ(C.CR4, System.cr4);
    EXPECT_EQ(C.EFER, System.efer);
    EXPECT_EQ(V.Core.Core.Core.CR8, System.cr8);
    EXPECT_EQ(Machine.Debug->DR6, Debug.dr6);
    EXPECT_EQ(Machine.Debug->DR7, Debug.dr7);
    EXPECT_EQ(V.GDTR.Base, System.gdt.base);
    EXPECT_EQ(V.GDTR.Limit, System.gdt.limit);
    EXPECT_EQ(V.IDTR.Base, System.idt.base);
    EXPECT_EQ(V.IDTR.Limit, System.idt.limit);
    const X64ContinuationSegment Segments[]{
        V.Core.CS,
        V.Core.SS,
        V.DS,
        V.ES,
        {R.FSBase, V.FS.Limit, V.FS.Selector, V.FS.Attributes},
        {R.GSBase, V.GS.Limit, V.GS.Selector, V.GS.Attributes},
        V.LDTR,
        V.TR};
    const kvm_segment Original[]{System.cs, System.ss, System.ds,  System.es,
                                 System.fs, System.gs, System.ldt, System.tr};
    for (unsigned I = 0; I != 8; ++I) {
      const auto Cache = X64PagedNativeCPU::segment(Segments[I]);
      EXPECT_EQ(Cache.selector, Original[I].selector);
      EXPECT_EQ(Cache.base, Original[I].base);
      EXPECT_EQ(Cache.limit, Original[I].limit);
      EXPECT_EQ(Cache.type, Original[I].type);
      EXPECT_EQ(Cache.s, Original[I].s);
      EXPECT_EQ(Cache.present, Original[I].present);
      EXPECT_EQ(Cache.db, Original[I].db);
      EXPECT_EQ(Cache.g, Original[I].g);
      EXPECT_EQ((V.Unusable >> I) & 1, Original[I].unusable);
    }
  }
}
#endif
