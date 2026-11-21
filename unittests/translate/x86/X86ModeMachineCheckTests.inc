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
  for (auto M : {Mode::Protected32, Mode::Compatibility16, Mode::Long64}) {
    SCOPED_TRACE(unsigned(M));
    start(M);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    Machine.MCA.emplace();
    Machine.MCA->Banks.resize(2);
    Machine.MCA->Banks[1].Status = 0xb000000000000105ull;
    Machine.MCA->GlobalStatus = 7;
    add(0x1000, M, {0x0f, 0x32});
    add(0x1002, M, {0x0f, 0x30});
    add(0x1004, M, {0x0f, 0x30});
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
  }
}

#if defined(__linux__) && defined(__x86_64__)
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
#endif
