//===- X86ModeMachineStatusTests.inc - Mixed-mode machine status ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete control banks and paid operand progress through SMSW/LMSW/CLTS.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
class X86ModeMachineStatusTest : public X86ModeSegmentNativeTest {
protected:
  void startStatus(Mode M) {
    start(M);
    for (unsigned I = 0; I != 8; ++I) {
      registers().Undefined[I] = 0;
      registers().GPR[I] = uint32_t(registers().GPR[I]);
    }
    std::fill(std::begin(registers().GPR) + 8, std::end(registers().GPR), 0);
    registers().GPR[3] = 0x2000;
    Options.Legacy.MachineStatus = X86LegacyMachineStatusProfile::FullCR0;
    put(0x10000, 0x11023);
    put(0x11000, 0x12023);
    put(0x12000, 0xe3);
    if (M == Mode::Protected16 || M == Mode::Protected32)
      put(0x9000 + 6 * 8,
          0x4060 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
  }

  void userStatus(Mode M) {
    auto &V = Machine.Core.Core;
    V.Core.CS = {0, UINT32_MAX, 59, uint16_t(code32(M) ? 0xc0fb : 0x80fb)};
    V.Core.SS = V.DS = V.ES = {0, UINT32_MAX, 51, 0xc0f3};
    V.FS = V.GS = {UINT32_MAX, 51, 0xc0f3};
    put(0x8038, descriptor(0, UINT32_MAX, V.Core.CS.Attributes));
    put(0x8030, descriptor(0, UINT32_MAX, V.Core.SS.Attributes));
    put(0x10000, 0x11027);
    put(0x11000, 0x12027);
    put(0x12000, 0xe7);
    if (!(Controls.EFER & 0x400))
      put(0xa008, 16);
  }

  void initializeOracle(X64PagedNativeCPU &Native) {
    if (!neverd::test::hostCpuIsIntel())
      GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
    ASSERT_NO_FATAL_FAILURE(Native.initialize());
    if (IsSkipped())
      return;
    if (!neverd::test::supportsX86KVMPrefault(Native.KVM))
      GTEST_SKIP() << "Machine-status oracle requires KVM pre-fault memory";
    ASSERT_NO_FATAL_FAILURE(Native.pinForReturnComparison());
  }

  void original(X64PagedNativeCPU &Native) {
    ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
    if (Machine.Debug) {
      kvm_debugregs Debug{};
      std::copy(Machine.Debug->DR.begin(), Machine.Debug->DR.end(), Debug.db);
      Debug.dr6 = Machine.Debug->DR6;
      Debug.dr7 = Machine.Debug->DR7;
      ASSERT_EQ(ioctl(Native.CPU, KVM_SET_DEBUGREGS, &Debug), 0) << errno;
    }
    ASSERT_NO_FATAL_FAILURE(
        neverd::test::prefaultX86KVMBacking(Native.CPU, Native.Size));
    ASSERT_NO_FATAL_FAILURE(Native.run());
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
  }

  void compareComplete(X64PagedNativeCPU &Native, bool TerminalHalt = false) {
    ASSERT_NO_FATAL_FAILURE(compareOriginal(Native, TerminalHalt));
    if (Machine.Debug) {
      kvm_debugregs Debug{};
      ASSERT_EQ(ioctl(Native.CPU, KVM_GET_DEBUGREGS, &Debug), 0) << errno;
      for (unsigned I = 0; I != Machine.Debug->DR.size(); ++I)
        EXPECT_EQ(Machine.Debug->DR[I], Debug.db[I]);
      EXPECT_EQ(Machine.Debug->DR6, Debug.dr6);
      EXPECT_EQ(Machine.Debug->DR7, Debug.dr7);
    }
    kvm_regs G{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &G), 0) << errno;
    const std::array<uint64_t, 16> Values{
        G.rax, G.rcx, G.rdx, G.rbx, G.rsp, G.rbp, G.rsi, G.rdi,
        G.r8,  G.r9,  G.r10, G.r11, G.r12, G.r13, G.r14, G.r15};
    for (unsigned I = 0; I != Values.size(); ++I)
      EXPECT_EQ(registers().GPR[I], Values[I]) << I;
  }

  void matrix(bool CompareOwner) {
    unsigned Comparisons = 0;
    for (Mode M : Modes)
      for (unsigned Form = 0; Form != 3; ++Form)
        for (unsigned Initial = 0; Initial != 8; ++Initial)
          for (bool Operand32 : {false, true})
            for (bool Address32 : {false, true})
              for (bool OperandMemory : {false, true}) {
                SCOPED_TRACE(testing::Message()
                             << unsigned(M) << ":" << Form << ":" << Initial
                             << ":" << Operand32 << ":" << Address32 << ":"
                             << OperandMemory);
                X64PagedNativeCPU Native;
                ASSERT_NO_FATAL_FAILURE(initializeOracle(Native));
                if (IsSkipped())
                  return;
                startStatus(M);
                Controls.CR0 = (Controls.CR0 & ~uint64_t{14}) | Initial * 2;
                const uint32_t Input = 0xdeadfff0 | (Initial ^ 11);
                registers().GPR[6] = Input;
                put(0x2000, Input);
                const auto Before = Controls;
                const auto Bytes = neverd::test::legacyMachineStatusBytes(
                    Form, code32(M), Operand32, Address32,
                    OperandMemory ? -1 : 6, 0x3e);
                const auto PC = add(0x1000, M, Bytes);
                add(PC, M, {0xf4});
                ASSERT_NO_FATAL_FAILURE(original(Native));
                kvm_regs General{};
                ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &General), 0)
                    << errno;
                ASSERT_EQ(General.rip, PC + 1);
                kvm_sregs Special{};
                ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &Special), 0)
                    << errno;
                const uint64_t Expected =
                    Form == 1   ? (Before.CR0 & ~uint64_t{14}) | (Input & 14)
                    : Form == 2 ? Before.CR0 & ~uint64_t{8}
                                : Before.CR0;
                EXPECT_EQ(Special.cr0, Expected);
                EXPECT_EQ(Special.cr3, Before.CR3);
                EXPECT_EQ(Special.cr4, Before.CR4);
                EXPECT_EQ(Special.efer, Before.EFER);
                if (CompareOwner) {
                  Memory = llvm::cantFail(
                      X86PagedMemory::create(RAM, Features, Controls));
                  uint64_t Budget = Program.size();
                  auto Got = X86ModeExecution(*Memory, Options)
                                 .run(Machine, Program, Budget);
                  ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
                  ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
                  ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::Halted);
                  ASSERT_EQ(Got->Completions, Program.size());
                  ASSERT_NO_FATAL_FAILURE(compareComplete(Native));
                }
                ++Comparisons;
              }
    RecordProperty(CompareOwner ? "original_non64_status_owner_comparisons"
                                : "original_non64_status_observations",
                   Comparisons);
  }
};

TEST_F(X86ModeMachineStatusTest, ObserveOriginalWidthsAndControlBanks) {
  matrix(false);
}

TEST_F(X86ModeMachineStatusTest, OwnerMatchesOriginalWidthsAndControlBanks) {
  matrix(true);
}

TEST_F(X86ModeMachineStatusTest, OperandBackingKeepsPaidOriginalBytes) {
  unsigned Comparisons = 0;
  for (Mode M : Modes)
    for (unsigned Form : {0u, 1u})
      for (bool CrossPage : {false, true}) {
        SCOPED_TRACE(unsigned(M) * 4 + Form * 2 + CrossPage);
        startStatus(M);
        const uint64_t Address = CrossPage ? 0x2fff : 0x2000;
        const uint64_t Missing = Address + CrossPage;
        registers().GPR[3] = Address;
        put(Address, 0xfffe);
        const auto Bytes = neverd::test::legacyMachineStatusBytes(
            Form, code32(M), true, true, -1);
        add(0x1000, M, Bytes);
        const auto Initial = Machine;
        const auto Original = RAM;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = 1;
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Completions, 1u);
        const auto Expected = Machine;
        const auto ExpectedControls = Memory->controls();
        auto ExpectedRAM = Memory->physicalMemory().snapshotMemoryRegions();
        ExpectedRAM.front().Bytes[0x1000] = 0xff;
        Machine = Initial;
        RAM = Original;
        omitBacking(RAM, Missing, 1);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        Budget = 1;
        Got = X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
        ASSERT_TRUE(Machine.Pending);
        EXPECT_EQ(Got->Attempts, 1u);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_EQ(Budget, 0u);
        const auto Frozen = Machine;
        registers().GPR[0] ^= uint64_t{1} << 32;
        Got = X86ModeExecution(*Memory, Options).run(Machine, {}, Budget);
        ASSERT_FALSE(bool(Got));
        llvm::consumeError(Got.takeError());
        Machine = Frozen;
        ASSERT_EQ(Memory->physicalMemory()
                      .storeScalar(0x1000, GuestScalarWidth::I8, 0xff)
                      .Status,
                  GuestMemoryAccessStatus::Completed);
        auto Changed = Options;
        Changed.Legacy.Scalars.reset();
        Changed.Legacy.MachineStatus.reset();
        for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
          Got = X86ModeExecution(*Memory, Changed).run(Machine, {}, Budget);
          ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
          EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
          EXPECT_EQ(Got->Attempts, 0u);
          EXPECT_EQ(Got->Completions, 0u);
          EXPECT_EQ(Machine.Core.Core, Frozen.Core.Core);
          EXPECT_EQ(Memory->controls().CR0, Controls.CR0);
        }
        auto Snapshot = RAM;
        Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
        Snapshot.Memory.push_back(
            {Missing,
             MemoryPermission::Read | MemoryPermission::Write,
             0,
             {Original.Memory.front().Bytes[Missing]}});
        auto Repaired = llvm::cantFail(
            X86PagedMemory::create(Snapshot, Features, Memory->controls()));
        Got = X86ModeExecution(*Repaired, Changed).run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        EXPECT_EQ(Got->Attempts, 0u);
        EXPECT_EQ(Got->Completions, 1u);
        EXPECT_FALSE(Machine.Pending);
        EXPECT_EQ(Machine.Core.Core, Expected.Core.Core);
        EXPECT_EQ(Repaired->controls(), ExpectedControls);
        for (const auto &Region :
             Repaired->physicalMemory().snapshotMemoryRegions())
          EXPECT_TRUE(
              std::equal(Region.Bytes.begin(), Region.Bytes.end(),
                         ExpectedRAM.front().Bytes.begin() + Region.Address));
        ++Comparisons;
      }
  RecordProperty("non64_status_operand_resume_comparisons", Comparisons);
}
TEST_F(X86ModeMachineStatusTest,
       GeneratedCode64ObservesCommittedMachineStatus) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Comparisons = 0;
  for (Mode M : {Mode::Compatibility16, Mode::Compatibility32})
    for (unsigned Case = 0; Case != 3; ++Case)
      for (bool Operand32 : {false, true})
        for (const auto &Compiler : Compilers)
          for (bool LLVM : {false, true})
            for (const std::string Optimization : {"-O0", "-O2"}) {
              SCOPED_TRACE(unsigned(M) * 6 + Case * 2 + Operand32);
              X64PagedNativeCPU Native;
              ASSERT_NO_FATAL_FAILURE(initializeOracle(Native));
              if (IsSkipped())
                return;
              startStatus(M);
              Controls.CR0 |= 8;
              registers().GPR[6] = 0xdeadfff4;
              put(0x2000, 0xdeadfff4);
              auto PC = add(0x1000, M,
                            neverd::test::legacyMachineStatusBytes(
                                Case == 2 ? 2 : 1, code32(M), Operand32, true,
                                Case == 1 ? -1 : 6));
              PC = add(PC, M,
                       neverd::test::legacyMachineStatusBytes(0, code32(M),
                                                              true, true, 1));
              if (code32(M))
                add(PC, M, {0xea, 0, 0x11, 0, 0, 0x18, 0});
              else
                add(PC, M, {0xea, 0, 0x11, 0x18, 0});
              PC = add(0x1100, Mode::Long64,
                       {0x48, 0x89, 0x0c, 0x25, 0, 0x30, 2, 0});
              PC = add(PC, Mode::Long64,
                       {0x48, 0x89, 0x34, 0x25, 8, 0x30, 2, 0});
              RAM.Memory.front().Bytes[PC] = 0xf4;
              ASSERT_NO_FATAL_FAILURE(
                  compileLong(LLVM, Optimization, Compiler));
              ASSERT_NO_FATAL_FAILURE(original(Native));
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              uint64_t Budget = Program.size();
              auto Got = X86ModeExecution(*Memory, Options)
                             .run(Machine, Program, Budget);
              ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
              ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
              ASSERT_EQ(Got->Completions, Program.size());
              ASSERT_TRUE(Got->BudgetExhausted);
              ASSERT_NO_FATAL_FAILURE(compareComplete(Native, true));
              EXPECT_EQ(physical(0x23000, GuestScalarWidth::I64),
                        Memory->controls().CR0);
              EXPECT_EQ(physical(0x23008, GuestScalarWidth::I64), 0xdeadfff4u);
              ++Comparisons;
            }
  RecordProperty("original_non64_status_generated_comparisons", Comparisons);
}

TEST_F(X86ModeMachineStatusTest, OriginalPrivilegeAndFetchFaultPriority) {
  unsigned Comparisons = 0;
  for (Mode M : Modes)
    for (unsigned Form : {1u, 2u})
      for (bool FetchFault : {false, true}) {
        if (FetchFault && (M == Mode::Protected16 || M == Mode::Protected32))
          continue;
        SCOPED_TRACE(unsigned(M) * 8 + Form * 2 + FetchFault);
        X64PagedNativeCPU Native;
        ASSERT_NO_FATAL_FAILURE(initializeOracle(Native));
        if (IsSkipped())
          return;
        startStatus(M);
        userStatus(M);
        Controls.CR0 |= 8;
        registers().GPR[3] = 0x210000;
        if (FetchFault) {
          Controls.EFER |= 0x800;
          put(0x12000, 0x13027);
          for (unsigned I = 0; I != 512; ++I)
            put(0x13000 + I * 8, (uint64_t(I) << 12) | 0x67);
          put(0x13008, 0);
        }
        add(0x1000, M,
            neverd::test::legacyMachineStatusBytes(Form, code32(M), true, true,
                                                   -1));
        ASSERT_NO_FATAL_FAILURE(original(Native));
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = 1;
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_EQ(Got->DeliveredVector, FetchFault ? 14 : 13);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_EQ(Memory->controls().CR0, Controls.CR0);
        ASSERT_NO_FATAL_FAILURE(compareComplete(Native, true));
        ++Comparisons;
      }
  RecordProperty("original_non64_status_priority_comparisons", Comparisons);
}

TEST_F(X86ModeMachineStatusTest, OriginalUMIPPrecedesUnmappedOperand) {
  unsigned A, B, C, D;
  if (!__get_cpuid_count(7, 0, &A, &B, &C, &D) || !(C & 4))
    GTEST_SKIP() << "Original UMIP comparison requires host UMIP";
  unsigned Comparisons = 0;
  for (Mode M : {Mode::Compatibility16, Mode::Compatibility32})
    for (bool UMIP : {false, true}) {
      X64PagedNativeCPU Native;
      ASSERT_NO_FATAL_FAILURE(initializeOracle(Native));
      if (IsSkipped())
        return;
      startStatus(M);
      userStatus(M);
      if (UMIP)
        Controls.CR4 |= 1u << 11;
      registers().GPR[3] = 0x210000;
      add(0x1000, M,
          neverd::test::legacyMachineStatusBytes(0, code32(M), true, true, -1));
      ASSERT_NO_FATAL_FAILURE(original(Native));
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      uint64_t Budget = 1;
      auto Got =
          X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
      EXPECT_EQ(Got->DeliveredVector, UMIP ? 13 : 14);
      EXPECT_EQ(Got->Completions, 0u);
      ASSERT_NO_FATAL_FAILURE(compareComplete(Native, true));
      ++Comparisons;
    }
  RecordProperty("original_non64_status_umip_comparisons", Comparisons);
}

TEST_F(X86ModeMachineStatusTest, OriginalCommittedControlsPrecedeSingleStep) {
  unsigned Comparisons = 0;
  for (Mode M : Modes)
    for (unsigned Case = 0; Case != 3; ++Case)
      for (bool Shadow : {false, true}) {
        SCOPED_TRACE(unsigned(M) * 6 + Case * 2 + Shadow);
        X64PagedNativeCPU Native;
        ASSERT_NO_FATAL_FAILURE(initializeOracle(Native));
        if (IsSkipped())
          return;
        startStatus(M);
        Machine.Debug.emplace();
        Options.Legacy.Debug = X86LegacyDebugProfile::DebugRegisters;
        Options.Long.DebugProfile = X86InstructionDebugProfile::DebugRegisters;
        Controls.CR0 |= 8;
        registers().GPR[0] = 16;
        registers().GPR[6] = 4;
        put(0x2000, 4);
        // Install the bank before setting TF with real guest instructions.
        auto PC = add(0x1000, M, {0x0f, 0x21, 0xfa});
        std::vector<uint8_t> Push{0x68, 2, 1, 0, 0}, Pop{0x9d};
        if (!code32(M)) {
          Push.insert(Push.begin(), 0x66);
          Pop.insert(Pop.begin(), 0x66);
        }
        PC = add(PC, M, Push);
        PC = add(PC, M, Pop);
        if (Shadow)
          PC = add(PC, M, {0x8e, 0xd0});
        PC = add(PC, M,
                 neverd::test::legacyMachineStatusBytes(Case == 2 ? 2 : 1,
                                                        code32(M), true, true,
                                                        Case == 1 ? -1 : 6));
        RAM.Memory.front().Bytes[PC] = 0xf4;
        ASSERT_NO_FATAL_FAILURE(original(Native));
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = Program.size();
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        EXPECT_EQ(Got->DeliveredVector, 1);
        EXPECT_EQ(Got->Completions, Program.size());
        EXPECT_EQ(Memory->controls().CR0,
                  Case == 2 ? Controls.CR0 & ~uint64_t{8}
                            : (Controls.CR0 & ~uint64_t{14}) | 4);
        ASSERT_NO_FATAL_FAILURE(compareComplete(Native, true));
        ++Comparisons;
      }
  RecordProperty("original_non64_status_completion_comparisons", Comparisons);
}

TEST_F(X86ModeMachineStatusTest, CachedPAEAndInactivePDPTEBanksArePreserved) {
  for (Mode M : Modes)
    for (unsigned Form : {1u, 2u}) {
      startStatus(M);
      Controls.CR0 |= 0x80000008;
      Controls.CR4 |= 0x20;
      Controls.PDPTE = {0x12001, 0, 0, 0};
      if (!(Controls.EFER & 0x400)) {
        Controls.CR3 = 0x18000;
        // The cached entry is valid; a fresh CR3-image reload would fail.
        put(0x18000, 0);
      }
      registers().GPR[6] = 4;
      add(0x1000, M,
          neverd::test::legacyMachineStatusBytes(Form, code32(M), true, true,
                                                 6));
      const auto ExpectedPDPTE = Controls.PDPTE;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      uint64_t Budget = 1;
      auto Got =
          X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
      EXPECT_EQ(Got->Completions, 1u);
      auto Expected = Controls;
      Expected.CR0 = Form == 2 ? Controls.CR0 & ~uint64_t{8}
                               : (Controls.CR0 & ~uint64_t{14}) | 4;
      EXPECT_EQ(Memory->controls(), Expected);
      EXPECT_EQ(Memory->controls().PDPTE, ExpectedPDPTE);
    }
}

TEST_F(X86ModeMachineStatusTest,
       OperandResumeRetiresBeforeSuspendedDebugEntry) {
  unsigned Comparisons = 0;
  for (Mode M : Modes)
    for (unsigned Form : {0u, 1u})
      for (bool Frame : {false, true}) {
        SCOPED_TRACE(unsigned(M) * 4 + Form * 2 + Frame);
        startStatus(M);
        Machine.Debug.emplace();
        Options.Legacy.Debug = X86LegacyDebugProfile::Watchpoints;
        Options.Long.DebugProfile = X86InstructionDebugProfile::Watchpoints;
        registers().RFlags |= 0x100;
        Machine.Debug->DR[0] = 0x2000;
        Machine.Debug->DR7 = 0x70401;
        put(0x2000, 4);
        add(0x1000, M,
            neverd::test::legacyMachineStatusBytes(Form, code32(M), true, true,
                                                   -1));
        const auto Initial = Machine;
        const auto Original = RAM;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = 1;
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
        ASSERT_EQ(Got->DeliveredVector, 1);
        EXPECT_EQ(Machine.Debug->DR6 & 0x400f, 0x4001u);
        const auto Expected = Machine;
        const auto ExpectedControls = Memory->controls();
        const auto ExpectedRAM =
            Memory->physicalMemory().snapshotMemoryRegions();
        Machine = Initial;
        RAM = Original;
        const bool Wide = Controls.EFER & 0x400;
        const unsigned Size = Frame ? (Wide ? 8 : 4) : (Wide ? 16 : 8);
        const uint64_t Entry = Frame ? 0xf000 - Size : 0x9000 + Size;
        omitBacking(RAM, 0x2000, 2);
        omitBacking(RAM, Entry, Size);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        Budget = 1;
        Got = X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
        ASSERT_TRUE(Machine.Pending);
        EXPECT_EQ(Got->Attempts, 1u);
        EXPECT_EQ(Got->Completions, 0u);
        auto Snapshot = RAM;
        Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
        Snapshot.Memory.push_back(
            {0x2000,
             MemoryPermission::Read | MemoryPermission::Write,
             0,
             {4, 0}});
        auto OperandReady = llvm::cantFail(
            X86PagedMemory::create(Snapshot, Features, Memory->controls()));
        Got = X86ModeExecution(*OperandReady, Options).run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
        EXPECT_EQ(Got->Attempts, 0u);
        EXPECT_EQ(Got->Completions, 1u);
        EXPECT_EQ(OperandReady->controls().CR0, ExpectedControls.CR0);
        ASSERT_TRUE(Machine.Pending);
        for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
          Got =
              X86ModeExecution(*OperandReady, Options).run(Machine, {}, Budget);
          ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
          EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
          EXPECT_EQ(Got->Attempts, 0u);
          EXPECT_EQ(Got->Completions, 0u);
        }
        Snapshot.Memory =
            OperandReady->physicalMemory().snapshotMemoryRegions();
        Snapshot.Memory.push_back(
            {Entry, MemoryPermission::Read | MemoryPermission::Write, 0,
             std::vector<uint8_t>(Original.Memory.front().Bytes.begin() + Entry,
                                  Original.Memory.front().Bytes.begin() +
                                      Entry + Size)});
        auto Repaired = llvm::cantFail(X86PagedMemory::create(
            Snapshot, Features, OperandReady->controls()));
        Got = X86ModeExecution(*Repaired, Options).run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_EQ(Got->Attempts, 0u);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_FALSE(Machine.Pending);
        EXPECT_EQ(Machine.Core.Core, Expected.Core.Core);
        EXPECT_EQ(Machine.Debug, Expected.Debug);
        EXPECT_EQ(Repaired->controls(), ExpectedControls);
        for (const auto &Region :
             Repaired->physicalMemory().snapshotMemoryRegions())
          EXPECT_TRUE(
              std::equal(Region.Bytes.begin(), Region.Bytes.end(),
                         ExpectedRAM.front().Bytes.begin() + Region.Address));
        ++Comparisons;
      }
  RecordProperty("non64_status_debug_resume_comparisons", Comparisons);
}

#endif
