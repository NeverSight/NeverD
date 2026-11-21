//===- X86ModeMachineCheckTests.inc - MCA reports and guest MSRs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Conventional bank logging, #MC priority, wake from HLT and guest recovery.
//===----------------------------------------------------------------------===//

TEST_F(X86ModeInterruptTest,
       MachineCheckReportsRetainBankOwnershipAndPriority) {
  start(Mode::Long64);
  auto &R = Machine.Core.Core.Core.Core.Core.Core;
  R.RFlags = 2;
  Machine.MCA.emplace();
  Machine.MCA->Banks.resize(2);
  Machine.MCA->GlobalControl = UINT64_MAX;
  Controls.CR4 |= 0x40;
  Options.Legacy.ControlRegisters->OtherCR4Features |= 0x40;
  bindMemory();
  X86ModeExecution Execution(*Memory, Options);
  X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
  constexpr uint64_t UC = 0xb000000000000105ULL,
                     Corrected = 0x9000000000000105ULL;
  const X86MachineCheckReport Error{1, UC, 0x8200, 0x30, 7};
  uint64_t Budget = 0;
  Inputs.MachineCheck = Error;
  Inputs.MachineCheck->Bank = 2;
  auto Refused = Execution.run(Machine, Program, Budget, Inputs);
  ASSERT_FALSE(Refused);
  llvm::consumeError(Refused.takeError());
  EXPECT_TRUE(Inputs.MachineCheck);
  EXPECT_EQ(Machine.MCA->Banks[1].Status, 0u);
  EXPECT_EQ(R.PC, 0x1000u);
  Inputs.MachineCheck = Error;
  Machine.MCA->GlobalControl = 0;
  auto Masked = llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
  EXPECT_TRUE(Masked.BudgetExhausted);
  EXPECT_FALSE(Inputs.MachineCheck);
  EXPECT_EQ(Machine.MCA->Banks[1].Status, 0u);
  Machine.MCA->GlobalControl = UINT64_MAX;
  Inputs.MachineCheck = X86MachineCheckReport{1, Corrected, 0x7400, 0x10, 0};
  EXPECT_TRUE(llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                  .BudgetExhausted);
  EXPECT_EQ(Machine.MCA->Banks[1].Status, Corrected);
  Inputs.MachineCheck = Error;
  Inputs.SMIPending = Inputs.NMIPending = true;
  Machine.Halted = true;
  auto Enter = llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
  EXPECT_TRUE(Enter.EventAccepted);
  ASSERT_EQ(Enter.DeliveredVector, 18u) << Enter.OwnerError;
  EXPECT_FALSE(Machine.Halted);
  EXPECT_TRUE(Inputs.SMIPending && Inputs.NMIPending);
  EXPECT_FALSE(Inputs.MachineCheck);
  EXPECT_EQ(word(bytes(*Memory), R.GPR[4], 8), 0x1000u);
  EXPECT_EQ(Machine.MCA->Banks[1].Status, UC | (uint64_t{1} << 62));
  EXPECT_EQ(Machine.MCA->Banks[1].Address, Error.Address);
  EXPECT_EQ(Machine.MCA->GlobalStatus, 7u);
  Inputs.SMIPending = Inputs.NMIPending = false;
  Inputs.MachineCheck = X86MachineCheckReport{1, Corrected, 1, 2, 0};
  llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
  EXPECT_EQ(Machine.MCA->Banks[1].Address, Error.Address);
  EXPECT_EQ(Machine.MCA->Banks[1].Misc, Error.Misc);
  Inputs.MachineCheck = Error;
  EXPECT_EQ(
      llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs)).Status,
      X86LegacyExecutionStatus::Shutdown);
  EXPECT_TRUE(Machine.Shutdown);
  EXPECT_FALSE(Inputs.MachineCheck);
}

TEST_F(X86ModeInterruptTest,
       MachineCheckMsrReadAndAcknowledgeUseOriginalInstructions) {
  for (auto M : Modes) {
    SCOPED_TRACE(unsigned(M));
    start(M);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    Machine.MCA.emplace();
    Machine.MCA->Banks.resize(2);
    Machine.MCA->Banks[1].Status = 0xb000000000000105ull;
    Machine.MCA->GlobalStatus = 7;
    Machine.MCA->CMCI = true;
    const bool Wide = Controls.EFER & 0x400;
    if (!Wide)
      put(0x9068, 0x40d0 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
    add(0x1000, M, {0x0f, 0x32});
    add(0x1002, M, {0x0f, 0x30});
    add(0x1004, M, {0x0f, 0x30});
    add(0x1006, M, {0x0f, 0x30});
    add(0x1008, M, {0x0f, 0x32});
    add(0x100a, M, {0x0f, 0x32});
    add(0x100c, M, {0x0f, 0x30});
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    R.GPR[1] = 0x405;
    uint64_t Budget = 1;
    auto Read = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Read.Completions, 1u) << Read.OwnerError;
    EXPECT_EQ(uint32_t(R.GPR[0]), 0x105u);
    EXPECT_EQ(uint32_t(R.GPR[2]), 0xb0000000u);
    R.GPR[0] = R.GPR[2] = 0;
    Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    EXPECT_EQ(Machine.MCA->Banks[1].Status, 0u);
    R.GPR[1] = 0x17a;
    Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    EXPECT_EQ(Machine.MCA->GlobalStatus, 0u);
    R.GPR[1] = 0x281;
    R.GPR[0] = 0x40000005;
    Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    R.GPR[0] = 0;
    Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    EXPECT_EQ(uint32_t(R.GPR[0]), 0x40000005u);
    R.GPR[1] = 0x179;
    Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    EXPECT_EQ(uint32_t(R.GPR[0]), 0x402u);
    R.GPR[1] = 0x281;
    R.GPR[2] = 1;
    Budget = 1;
    auto Fault = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Fault.Status, X86LegacyExecutionStatus::EventDelivered)
        << Fault.OwnerError;
    EXPECT_EQ(Fault.DeliveredVector, 13u);
    EXPECT_EQ(Fault.Completions, 0u);
    EXPECT_EQ(Machine.MCA->Banks[1].Control2, 0x40000005u);
    EXPECT_EQ(R.PC, 0x40d0u);
    const unsigned Width = Wide ? 8 : 4;
    const unsigned SP = Wide ? 0xefd0 : 0xeff0;
    EXPECT_EQ(uint32_t(R.GPR[4]), SP);
    const auto Frame = bytes(*Memory);
    EXPECT_EQ(word(Frame, SP, Width), 0u);
    EXPECT_EQ(word(Frame, SP + Width, Width), 0x100cu);
    EXPECT_EQ(word(Frame, SP + 2 * Width, Width), M == Mode::Long64 ? 24 : 8);
    EXPECT_EQ(word(Frame, SP + 3 * Width, Width), 0x10202u);
    if (Wide) {
      EXPECT_EQ(word(Frame, SP + 4 * Width, Width), 0xf000u);
      EXPECT_EQ(word(Frame, SP + 5 * Width, Width), 16u);
    }
    EXPECT_EQ(R.GPR[5] >> 32, M == Mode::Long64 ? 0xdead0005u : 0x12340005u);
    EXPECT_EQ(R.GPR[15], 0x1111222233334444ull);
  }
}

TEST_F(X86ModeInterruptTest, CorrectedErrorsCountToCmciAndKeepMaskedArrivals) {
  start(Mode::Long64);
  auto &R = Machine.Core.Core.Core.Core.Core.Core;
  R.RFlags = 2;
  Machine.MCA.emplace();
  auto &MCA = *Machine.MCA;
  MCA.CMCI = true;
  MCA.Banks.resize(2);
  MCA.Banks[0].CorrectedThresholdMask = 0;
  MCA.Banks[1].CorrectedThresholdMask = 7;
  bool Cleared = true;
  ASSERT_TRUE(MCA.writeMSR(0x280, 0x40007fff, Cleared));
  EXPECT_EQ(MCA.readMSR(0x280), 0u);
  ASSERT_TRUE(MCA.writeMSR(0x281, 0x40007ffb, Cleared));
  EXPECT_FALSE(Cleared);
  EXPECT_EQ(MCA.readMSR(0x281), 0x40000003u);
  EXPECT_EQ(MCA.readMSR(0x179), 0x402u);
  EXPECT_FALSE(MCA.writeMSR(0x281, UINT64_C(1) << 32, Cleared));
  EXPECT_EQ(MCA.readMSR(0x281), 0x40000003u);
  bindMemory();
  X86ModeExecution Execution(*Memory, Options);
  X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
  constexpr uint64_t Corrected = UINT64_C(0x9000000000000105);
  const X86MachineCheckReport Occurrence{1, Corrected, 0x1234, 0x55, 0, true};
  uint64_t Budget = 0;
  for (unsigned I = 1; I != 3; ++I) {
    Inputs.MachineCheck = Occurrence;
    auto Report =
        llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
    EXPECT_TRUE(Report.BudgetExhausted);
    EXPECT_FALSE(Report.EventAccepted);
    EXPECT_FALSE(Inputs.MachineCheck);
    EXPECT_EQ((MCA.Banks[1].Status >> 38) & 0x7fff, I);
  }
  Inputs.MachineCheck = Occurrence;
  for (const std::optional<uint32_t> LVT :
       {std::optional<uint32_t>{}, {7}, {0x500}}) {
    Inputs.LocalVectorCMCI = LVT;
    auto Denied = Execution.run(Machine, Program, Budget, Inputs);
    ASSERT_FALSE(Denied);
    llvm::consumeError(Denied.takeError());
    EXPECT_TRUE(Inputs.MachineCheck);
    EXPECT_EQ((MCA.Banks[1].Status >> 38) & 0x7fff, 2u);
    EXPECT_FALSE(Inputs.APIC.nextFixedInterrupt());
  }
  Inputs.LocalVectorCMCI = 0x71;
  EXPECT_TRUE(llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                  .BudgetExhausted);
  EXPECT_FALSE(Inputs.MachineCheck);
  EXPECT_EQ((MCA.Banks[1].Status >> 38) & 0x7fff, 3u);
  EXPECT_EQ(Inputs.APIC.nextFixedInterrupt(), 0x71u);
  R.RFlags |= 0x200;
  auto IRQ = llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
  EXPECT_TRUE(IRQ.EventAccepted);
  EXPECT_EQ(IRQ.DeliveredVector, 0x71u);
  EXPECT_FALSE(Inputs.APIC.nextFixedInterrupt());
  EXPECT_EQ(Inputs.APIC.state().ISR[3], 1u << 17);
  ASSERT_TRUE(MCA.writeMSR(0x405, 0, Cleared));
  EXPECT_EQ(MCA.Banks[1].Status, 0u);
  // The high count bit is sticky when the low count rolls over.
  MCA.Banks[1].Status = Corrected | (UINT64_C(0x7fff) << 38);
  EXPECT_EQ(llvm::cantFail(MCA.report(Occurrence, false)),
            X86MachineCheckAction::NoException);
  EXPECT_EQ((MCA.Banks[1].Status >> 38) & 0x7fff, 0x4000u);
}

TEST_F(X86ModeInterruptTest, CmciNmiAndSmiUseTheExistingEventLatches) {
  for (bool Smi : {false, true}) {
    SCOPED_TRACE(Smi);
    start(Mode::Long64);
    Machine.Debug.emplace();
    Options.Legacy.Debug = Options.Long.DebugProfile =
        X86InstructionDebugProfile::Watchpoints;
    Machine.SMM.emplace();
    Machine.MCA.emplace();
    Machine.MCA->CMCI = true;
    Machine.MCA->Banks.resize(1);
    bool Cleared;
    ASSERT_TRUE(Machine.MCA->writeMSR(0x280, 0x40000000, Cleared));
    bindMemory();
    auto System =
        llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, Options, *System);
    X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
    Inputs.LocalVectorCMCI = Smi ? 0x200 : 0x400;
    Inputs.MachineCheck =
        X86MachineCheckReport{0, UINT64_C(0xb000000000000105)};
    uint64_t Budget = 0;
    auto Delivered =
        llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
    EXPECT_TRUE(Delivered.EventAccepted);
    EXPECT_FALSE(Inputs.MachineCheck);
    EXPECT_FALSE(Inputs.NMIPending);
    EXPECT_FALSE(Inputs.SMIPending);
    EXPECT_EQ(Machine.SMM->Active, Smi);
    EXPECT_TRUE(Machine.Core.Core.NMIBlocked);
    if (!Smi)
      EXPECT_EQ(Delivered.DeliveredVector, 2u);
    EXPECT_EQ(Inputs.APIC.state().ISR, X86LocalApicFixedState{}.ISR);
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X86ModeInterruptTest, InvalidCmciMsrFaultFramesMatchKvmAcrossModes) {
  for (auto M : Modes) {
    SCOPED_TRACE(unsigned(M));
    X64PagedNativeCPU Native;
    ASSERT_NO_FATAL_FAILURE(Native.initialize());
    uint64_t Supported = 0;
    if (Native.CPU < 0 ||
        ioctl(Native.KVM, KVM_X86_GET_MCE_CAP_SUPPORTED, &Supported) < 0 ||
        !(Supported & 0x400))
      GTEST_SKIP() << "KVM CMCI unavailable";
    uint64_t Cap = 0x402;
    ASSERT_EQ(ioctl(Native.CPU, KVM_X86_SETUP_MCE, &Cap), 0);
    start(M);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    const bool Wide = Controls.EFER & 0x400;
    if (!Wide)
      put(0x9068, 0x40d0 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
    Machine.MCA.emplace();
    Machine.MCA->CMCI = true;
    Machine.MCA->Banks.resize(2);
    R.PC = 0x100c;
    R.GPR[0] = 0x402;
    R.GPR[1] = 0x281;
    R.GPR[2] = 1;
    add(R.PC, M, {0x0f, 0x30});
    ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
    kvm_regs Original{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Original), 0);
    Original.rax = R.GPR[0];
    Original.rcx = R.GPR[1];
    Original.rdx = R.GPR[2];
    Original.rsp = 0xf000;
    Original.rbp = M == Mode::Long64 ? R.GPR[5] : R.Undefined[5];
    Original.r15 = R.GPR[15];
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &Original), 0);
    ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Original), 0);

    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Fault = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Fault.Status, X86LegacyExecutionStatus::EventDelivered)
        << Fault.OwnerError;
    EXPECT_EQ(Fault.DeliveredVector, 13u);
    EXPECT_EQ(R.PC + 1, Original.rip);
    EXPECT_EQ(uint32_t(R.GPR[4]), Original.rsp);
    EXPECT_EQ(R.GPR[5], Original.rbp);
    EXPECT_EQ(R.GPR[15], Original.r15);
    const auto Image = bytes(*Memory);
    const unsigned Size = Wide ? 48 : 16;
    ASSERT_LE(Original.rsp, Image.size() - Size);
    EXPECT_EQ(std::memcmp(Image.data() + Original.rsp,
                          Native.RAM + Original.rsp, Size),
              0);
  }
}

TEST_F(X86ModeInterruptTest, MachineCheckInjectionAndHaltMatchKvmBanks) {
  enum Case { Deliver, Disabled, Nested, Corrected, Halted };
  for (auto Scenario : {Deliver, Disabled, Nested, Corrected, Halted}) {
    SCOPED_TRACE(int(Scenario));
    X64PagedNativeCPU Native;
    ASSERT_NO_FATAL_FAILURE(Native.initialize());
    if (Native.CPU < 0 ||
        ioctl(Native.KVM, KVM_CHECK_EXTENSION, KVM_CAP_MCE) < 2)
      GTEST_SKIP() << "KVM conventional MCA unavailable";
    uint64_t Cap = 0x102;
    ASSERT_EQ(ioctl(Native.CPU, KVM_X86_SETUP_MCE, &Cap), 0);
    start(Mode::Long64);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.RFlags = 2;
    Machine.MCA.emplace();
    Machine.MCA->Banks.resize(2);
    Machine.MCA->GlobalControl = UINT64_MAX;
    Options.Legacy.ControlRegisters->OtherCR4Features |= 0x40;
    if (Scenario != Disabled)
      Controls.CR4 |= 0x40;
    add(0x1000, Mode::Long64, {0xf4});
    add(0x4120, Mode::Long64, {0xf4});
    ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
    kvm_x86_mce Error{};
    Error.bank = 1;
    Error.status = 0xb000000000000105ull;
    Error.addr = 0x1234;
    Error.misc = 0x22;
    Error.mcg_status = 7;
    if (Scenario == Corrected)
      Error.status &= ~(uint64_t{1} << 61);
    uint64_t Budget;
    if (Scenario == Halted) {
      ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
      ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
      Budget = 1;
      ASSERT_EQ(llvm::cantFail(Execution.run(Machine, Program, Budget)).Status,
                X86LegacyExecutionStatus::Halted);
    }
    for (unsigned I = 0; I != (Scenario == Nested ? 2u : 1u); ++I) {
      ASSERT_EQ(ioctl(Native.CPU, KVM_X86_SET_MCE, &Error), 0);
      Inputs.MachineCheck = X86MachineCheckReport{
          Error.bank, Error.status, Error.addr, Error.misc, Error.mcg_status};
      Budget = 0;
      auto Arrive =
          llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
      ASSERT_TRUE(Arrive.OwnerError.empty()) << Arrive.OwnerError;
      ASSERT_FALSE(Inputs.MachineCheck);
      ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
      if (Machine.Shutdown) {
        ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_SHUTDOWN));
      } else {
        ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
        Budget = 1;
        ASSERT_EQ(
            llvm::cantFail(Execution.run(Machine, Program, Budget)).Status,
            X86LegacyExecutionStatus::Halted);
      }
    }
    kvm_regs Original{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Original), 0);
    EXPECT_EQ(R.PC, Original.rip);
    EXPECT_EQ(R.GPR[4], Original.rsp);
    if (Original.rsp != 0xf000)
      EXPECT_EQ(word(bytes(*Memory), R.GPR[4], 8),
                llvm::support::endian::read64le(Native.RAM + Original.rsp));
    struct {
      uint32_t Count = 4, Pad = 0;
      kvm_msr_entry Entries[4]{};
    } Registers;
    uint32_t Addresses[] = {0x17a, 0x405, 0x406, 0x407};
    for (unsigned I = 0; I != 4; ++I)
      Registers.Entries[I].index = Addresses[I];
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_MSRS, &Registers), 4);
    for (const auto &Reg : Registers.Entries)
      EXPECT_EQ(Machine.MCA->readMSR(Reg.index), Reg.data) << Reg.index;
  }
}
TEST_F(X86ModeInterruptTest, CmciNotificationsMatchKvmMcaAndLocalApic) {
  enum Case {
    Notify,
    Disabled,
    Masked,
    InjectedCorrected,
    Priority,
    IFBlocked,
    ZeroThreshold
  };
  for (auto Scenario : {Notify, Disabled, Masked, InjectedCorrected, Priority,
                        IFBlocked, ZeroThreshold}) {
    SCOPED_TRACE(int(Scenario));
    X64PagedNativeCPU Native;
    ASSERT_NO_FATAL_FAILURE(Native.initialize(0));
    uint64_t Supported = 0;
    if (Native.CPU < 0 ||
        ioctl(Native.KVM, KVM_X86_GET_MCE_CAP_SUPPORTED, &Supported) < 0 ||
        !(Supported & 0x400))
      GTEST_SKIP() << "KVM CMCI unavailable";
    uint64_t Cap = 0x502;
    ASSERT_EQ(ioctl(Native.CPU, KVM_X86_SETUP_MCE, &Cap), 0);
    start(Mode::Long64);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    if (Scenario == IFBlocked)
      R.RFlags &= ~uint64_t(0x200);
    Machine.MCA.emplace();
    Machine.MCA->Banks.resize(2);
    Machine.MCA->GlobalControl = UINT64_MAX;
    Machine.MCA->CMCI = true;
    const uint32_t CTL2 = (Scenario == Disabled ? 0u : 0x40000000u) |
                          (Scenario == ZeroThreshold ? 0u : 4u);
    bool Cleared;
    ASSERT_TRUE(Machine.MCA->writeMSR(0x281, CTL2, Cleared));
    const uint32_t LVT = 0x71 | (Scenario == Masked ? 0x10000 : 0);
    X86LocalApicFixedState APIC;
    if (Scenario == Priority) {
      APIC.TPR = 0x70;
      Machine.Core.Core.Core.Core.Core.CR8 = 7;
    }
    X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState(APIC)};
    Inputs.LocalVectorCMCI = LVT;
    constexpr uint64_t Handler = 0x4710;
    add(0x1000, Mode::Long64, {0xe6, 0x80});
    add(Handler, Mode::Long64, {0xe6, 0x81});
    ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
    kvm_lapic_state OriginalAPIC{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_LAPIC, &OriginalAPIC), 0);
    namespace le = llvm::support::endian;
    le::write32le(OriginalAPIC.regs + 0xf0, 0x1ff);
    le::write32le(OriginalAPIC.regs + 0x2f0, LVT);
    le::write32le(OriginalAPIC.regs + 0x80, APIC.TPR);
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_LAPIC, &OriginalAPIC), 0);
    struct {
      uint32_t Count = 1, Pad = 0;
      kvm_msr_entry Entries[2]{};
    } Registers;
    Registers.Entries[0].index = 0x281;
    Registers.Entries[0].data = CTL2;
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_MSRS, &Registers), 1);
    kvm_x86_mce Report{};
    Report.bank = 1;
    Report.status = Scenario == InjectedCorrected
                        ? UINT64_C(0x9000010000000105)
                        : UINT64_C(0xb000000000000105);
    Report.addr = 0x1234;
    Report.misc = 0x22;
    ASSERT_EQ(ioctl(Native.CPU, KVM_X86_SET_MCE, &Report), 0);
    Inputs.MachineCheck =
        X86MachineCheckReport{1, Report.status, Report.addr, Report.misc, 0};
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 0;
    auto Event =
        llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
    ASSERT_TRUE(Event.OwnerError.empty()) << Event.OwnerError;
    ASSERT_FALSE(Inputs.MachineCheck);
    ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_IO));
    EXPECT_EQ(Event.EventAccepted, Native.Run->io.port == 0x81);
    kvm_regs Original{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Original), 0);
    EXPECT_EQ(R.PC, Original.rip);
    EXPECT_EQ(R.GPR[4], Original.rsp);
    if (Event.EventAccepted) {
      EXPECT_EQ(Event.DeliveredVector, 0x71u);
      EXPECT_EQ(word(bytes(*Memory), R.GPR[4], 8),
                le::read64le(Native.RAM + Original.rsp));
    }
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_LAPIC, &OriginalAPIC), 0);
    for (unsigned I = 0; I != 8; ++I) {
      EXPECT_EQ(Inputs.APIC.state().IRR[I],
                le::read32le(OriginalAPIC.regs + 0x200 + 16 * I));
      EXPECT_EQ(Inputs.APIC.state().ISR[I],
                le::read32le(OriginalAPIC.regs + 0x100 + 16 * I));
    }
    Registers.Count = 2;
    Registers.Entries[1].index = 0x405;
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_MSRS, &Registers), 2);
    for (const auto &Entry : Registers.Entries)
      EXPECT_EQ(Machine.MCA->readMSR(Entry.index), Entry.data);
  }
}
#endif
