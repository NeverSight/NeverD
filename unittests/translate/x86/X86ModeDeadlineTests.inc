//===- X86ModeDeadlineTests.inc - Local APIC TSC deadline -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original deadline MSR instructions and fixed delivery across CPU modes.
//===----------------------------------------------------------------------===//

TEST_F(X86ModeInterruptTest, DeadlineMsrReadsWritesAndMaskingUseGuestTime) {
  constexpr uint64_t Deadline = 0x1000000f3;
  for (auto M : Modes) {
    SCOPED_TRACE(unsigned(M));
    start(M);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.RFlags &= ~uint64_t{0x200};
    R.GPR[0] = uint32_t(Deadline);
    R.GPR[1] = X86TscDeadlineTimer::MSR;
    R.GPR[2] = Deadline >> 32;
    add(0x1000, M, {0x0f, 0x30});
    add(0x1002, M, {0x0f, 0x32});
    auto &Timer = Machine.DeadlineTimer.emplace();
    ASSERT_TRUE(Timer.setLocalVector(true, 64, true));
    X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
    Inputs.SampledTSC = Deadline - 1;
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 2;
    auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
    ASSERT_EQ(Step.Completions, 2u) << Step.OwnerError;
    EXPECT_EQ(uint32_t(R.GPR[0]), uint32_t(Deadline));
    EXPECT_EQ(uint32_t(R.GPR[2]), Deadline >> 32);
    EXPECT_EQ(Timer.read(), Deadline);
    Inputs.SampledTSC = Deadline;
    ASSERT_TRUE(llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                    .BudgetExhausted);
    EXPECT_EQ(Timer.read(), 0u);
    ASSERT_TRUE(Timer.setLocalVector(true, 64, false));
    EXPECT_FALSE(Inputs.APIC.nextFixedInterrupt());
    Timer.write(Deadline + 1);
    ASSERT_TRUE(Timer.setLocalVector(true, 65, false));
    EXPECT_EQ(Timer.read(), Deadline + 1);
    EXPECT_FALSE(Timer.setLocalVector(true, 1, false));
    Inputs.SampledTSC = Deadline + 2;
    ASSERT_TRUE(llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                    .BudgetExhausted);
    EXPECT_EQ(Inputs.APIC.nextFixedInterrupt(), 65u);
    EXPECT_EQ(Timer.read(), 0u);
    Timer.write(Deadline + 3);
    ASSERT_TRUE(Timer.setLocalVector(false, 66, false));
    EXPECT_EQ(Timer.read(), 0u);
    Timer.write(Deadline + 4);
    EXPECT_EQ(Timer.read(), 0u);
    ASSERT_TRUE(Timer.setLocalVector(true, 66, false));
    Timer.write(Deadline + 5);
    Timer.write(0);
    EXPECT_EQ(Timer.read(), 0u);
  }
}

TEST_F(X86ModeInterruptTest, DeadlineAtHaltWaitsThroughSmiAndRsm) {
  start(Mode::Long64);
  auto &R = Machine.Core.Core.Core.Core.Core.Core;
  Machine.SMM.emplace();
  Machine.Debug.emplace();
  Machine.DeadlineTimer.emplace();
  ASSERT_TRUE(Machine.DeadlineTimer->setLocalVector(true, 64, false));
  Options.Legacy.Debug = Options.Long.DebugProfile =
      X86InstructionDebugProfile::Watchpoints;
  R.GPR[0] = 10;
  R.GPR[1] = X86TscDeadlineTimer::MSR;
  R.GPR[2] = 1;
  add(0x1000, Mode::Long64, {0x0f, 0x30});
  add(0x1002, Mode::Long64, {0xf4});
  auto Firmware = RAM;
  Firmware.Memory.front().Bytes[0x38000] = 0x0f;
  Firmware.Memory.front().Bytes[0x38001] = 0xaa;
  Program.push_back({0x38000, Mode::Real16, {0x0f, 0xaa}});
  bindMemory();
  auto System =
      llvm::cantFail(X86PagedMemory::create(Firmware, Features, Controls));
  X86ModeExecution Execution(*Memory, Options, *System);
  X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
  Inputs.SampledTSC = uint64_t{1} << 32;
  uint64_t Budget = 2;
  auto Halt = llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
  ASSERT_TRUE(Machine.Halted) << Halt.OwnerError;
  EXPECT_EQ(Halt.Completions, 2u);
  EXPECT_EQ(R.PC, 0x1003u);
  EXPECT_EQ(Machine.DeadlineTimer->read(), *Inputs.SampledTSC + 10);
  Inputs.SMIPending = true;
  *Inputs.SampledTSC += 10;
  auto Smi = llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
  ASSERT_TRUE(Smi.EventAccepted && Machine.SMM->Active) << Smi.OwnerError;
  EXPECT_FALSE(Machine.Halted);
  EXPECT_EQ(Machine.DeadlineTimer->read(), 0u);
  EXPECT_EQ(Inputs.APIC.nextFixedInterrupt(), 64u);
  EXPECT_EQ(Inputs.APIC.state().ISR[2], 0u);
  Budget = 1;
  auto Wake = llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
  ASSERT_EQ(Wake.DeliveredVector, 64u) << Wake.OwnerError;
  EXPECT_EQ(Wake.Completions, 1u);
  EXPECT_FALSE(Machine.SMM->Active);
  EXPECT_EQ(Inputs.APIC.state().IRR[2], 0u);
  EXPECT_EQ(Inputs.APIC.state().ISR[2], 1u);
  EXPECT_EQ(word(bytes(*Memory), R.GPR[4], 8), 0x1003u);
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X86ModeInterruptTest, OriginalDeadlineMsrsAndPendingEdgesMatchKvm) {
  unsigned Comparisons = 0;
  for (bool DeadlineMode : {false, true})
    for (bool Masked : {false, true})
      for (bool Expire : {false, true}) {
        SCOPED_TRACE(testing::Message() << DeadlineMode << " masked=" << Masked
                                        << " expire=" << Expire);
        X64PagedNativeCPU Native;
        ASSERT_NO_FATAL_FAILURE(Native.initialize(0));
        if (Native.CPU < 0 || ioctl(Native.KVM, KVM_CHECK_EXTENSION,
                                    KVM_CAP_TSC_DEADLINE_TIMER) <= 0)
          GTEST_SKIP() << "KVM TSC-deadline timer unavailable";
        auto *CPUID =
            reinterpret_cast<kvm_cpuid2 *>(Native.CPUIDStorage.data());
        for (unsigned I = 0; I != CPUID->nent; ++I)
          if (CPUID->entries[I].function == 1)
            CPUID->entries[I].ecx |= 1u << 24;
        ASSERT_EQ(ioctl(Native.CPU, KVM_SET_CPUID2, CPUID), 0);
        start(Mode::Long64);
        auto &R = Machine.Core.Core.Core.Core.Core.Core;
        R.RFlags &= ~uint64_t{0x200};
        R.GPR[1] = X86TscDeadlineTimer::MSR;
        add(0x1000, Mode::Long64, {0x0f, 0x30});
        add(0x1002, Mode::Long64, {0xe6, 0xe1});
        add(0x1004, Mode::Long64, {0x0f, 0x32});
        add(0x1006, Mode::Long64, {0xe6, 0xe2});
        add(0x1008, Mode::Long64, {0xeb, 0xfa});
        ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
        kvm_lapic_state LAPIC{};
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_LAPIC, &LAPIC), 0);
        namespace le = llvm::support::endian;
        le::write32le(LAPIC.regs + 0xf0, 0x1ff);
        le::write32le(LAPIC.regs + 0x320, 64 | (Masked ? 1u << 16 : 0) |
                                              (DeadlineMode ? 2u << 17 : 0));
        ASSERT_EQ(ioctl(Native.CPU, KVM_SET_LAPIC, &LAPIC), 0);
        struct {
          uint32_t Count = 1, Padding = 0;
          kvm_msr_entry TSC{.index = 0x10};
        } Time;
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_MSRS, &Time), 1);
        const uint64_t Value = Expire ? 1 : Time.TSC.data + (uint64_t{1} << 38);
        kvm_regs Regs{};
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Regs), 0);
        Regs.rax = R.GPR[0] = uint32_t(Value);
        Regs.rdx = R.GPR[2] = Value >> 32;
        Regs.rcx = X86TscDeadlineTimer::MSR;
        ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &Regs), 0);
        ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
        ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_IO));
        ASSERT_EQ(Native.Run->io.port, 0xe1);
        // Timer firing can lag the TSC comparison. Reenter at a bounded I/O
        // exit. No additional writes to the timer occur in the observation
        // loop.
        for (unsigned Wait = 0; Wait != 512; ++Wait) {
          ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
          ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_IO));
          ASSERT_EQ(Native.Run->io.port, 0xe2);
          ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Regs), 0);
          if (!Expire || !DeadlineMode || !(Regs.rax | Regs.rdx))
            break;
        }
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_LAPIC, &LAPIC), 0);
        ASSERT_TRUE(Machine.DeadlineTimer.emplace().setLocalVector(DeadlineMode,
                                                                   64, Masked));
        X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
        Inputs.SampledTSC = Time.TSC.data;
        bindMemory();
        X86ModeExecution Execution(*Memory, Options);
        uint64_t Budget = 1;
        ASSERT_EQ(
            llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                .Completions,
            1u);
        R.PC = 0x1004;
        Budget = 1;
        ASSERT_EQ(
            llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                .Completions,
            1u);
        EXPECT_EQ(R.GPR[0], Regs.rax);
        EXPECT_EQ(R.GPR[2], Regs.rdx);
        EXPECT_EQ(Inputs.APIC.state().IRR[2], le::read32le(LAPIC.regs + 0x220));
        EXPECT_EQ(Inputs.APIC.state().IRR[2],
                  unsigned(DeadlineMode && Expire && !Masked));
        ++Comparisons;
      }
  RecordProperty("cpu_comparisons", Comparisons);
  EXPECT_EQ(Comparisons, 8u);
}
#endif
