//===- X86ModeDebugRegisterTests.inc - Mixed-mode MOV-DR -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Full debug-bank writes, native transitions and retained debug events.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
class X86ModeDebugRegisterTest : public X86ModeSegmentNativeTest {
protected:
  void startDebug(Mode M) {
    start(M);
    auto &R = registers();
    for (unsigned I = 0; I != 8; ++I) {
      R.Undefined[I] = 0;
      R.GPR[I] = uint32_t(R.GPR[I]);
    }
    std::fill(std::begin(R.GPR) + 8, std::end(R.GPR), 0);
    Machine.Debug.emplace();
    for (unsigned I = 0; I != 4; ++I)
      Machine.Debug->DR[I] = 0xffff800100002305ULL + I * 0x100;
    Options.Legacy.Debug = X86InstructionDebugProfile::DebugRegisters;
    Options.Long.DebugProfile = X86InstructionDebugProfile::DebugRegisters;
    Options.Legacy.GeneralDetectFrame = X86GeneralDetectFrame::ClearResume;
    Options.Long.GeneralDetectFrame = X86GeneralDetectFrame::ClearResume;
    // Both runs start with identical page A/D bits; the oracle's terminal HLT
    // does not add a page-walk difference to a stop at handler entry.
    put(0x10000, 0x11023);
    put(0x11000, 0x12023);
    put(0x12000, 0xe3);
    if (M == Mode::Protected16 || M == Mode::Protected32)
      put(0x9000 + 6 * 8,
          0x4060 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
  }

  static std::vector<uint8_t> move(unsigned Debug, bool Write,
                                   unsigned Prefix = 0, unsigned Mod = 3) {
    std::vector<uint8_t> Bytes;
    if (Prefix == 1)
      Bytes.push_back(0x66);
    else if (Prefix == 2)
      Bytes.push_back(0x67);
    else if (Prefix == 3)
      Bytes = {0xf3, 0x64, 0x66, 0x67};
    Bytes.insert(Bytes.end(), {0x0f, uint8_t(Write ? 0x23 : 0x21),
                               uint8_t((Mod << 6) | (Debug << 3) | 1)});
    return Bytes;
  }

  void initializeDebugOracle(X64PagedNativeCPU &Native) {
    if (!neverd::test::hostCpuIsIntel())
      GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
    ASSERT_NO_FATAL_FAILURE(Native.initialize());
    if (IsSkipped())
      return;
    if (!neverd::test::supportsX86KVMPrefault(Native.KVM))
      GTEST_SKIP() << "MOV-DR oracle requires KVM pre-fault memory";
    ASSERT_NO_FATAL_FAILURE(Native.pinForReturnComparison());
  }

  void originalDebug(X64PagedNativeCPU &Native) {
    ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
    kvm_debugregs D{};
    for (unsigned I = 0; I != 4; ++I)
      D.db[I] = Machine.Debug->DR[I];
    D.dr6 = Machine.Debug->DR6;
    D.dr7 = Machine.Debug->DR7;
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_DEBUGREGS, &D), 0) << errno;
    ASSERT_NO_FATAL_FAILURE(
        neverd::test::prefaultX86KVMBacking(Native.CPU, Native.Size));
    ASSERT_NO_FATAL_FAILURE(Native.run());
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
  }

  void compareDebugOriginal(X64PagedNativeCPU &Native,
                            bool TerminalHalt = false) {
    ASSERT_NO_FATAL_FAILURE(compareOriginal(Native, TerminalHalt));
    kvm_debugregs D{};
    kvm_regs G{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_DEBUGREGS, &D), 0) << errno;
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &G), 0) << errno;
    for (unsigned I = 0; I != 4; ++I)
      EXPECT_EQ(Machine.Debug->DR[I], D.db[I]);
    EXPECT_EQ(Machine.Debug->DR6, D.dr6);
    EXPECT_EQ(Machine.Debug->DR7, D.dr7);
    const std::array<uint64_t, 16> GPR{G.rax, G.rcx, G.rdx, G.rbx, G.rsp, G.rbp,
                                       G.rsi, G.rdi, G.r8,  G.r9,  G.r10, G.r11,
                                       G.r12, G.r13, G.r14, G.r15};
    for (unsigned I = 0; I != GPR.size(); ++I)
      EXPECT_EQ(registers().GPR[I], GPR[I]) << I;
  }

  void registerMatrix(bool CompareOwner) {
    unsigned Comparisons = 0;
    for (Mode M : Modes)
      for (unsigned Debug = 0; Debug != 8; ++Debug)
        for (bool Write : {false, true})
          for (unsigned Prefix = 0; Prefix != 4; ++Prefix) {
            SCOPED_TRACE((unsigned(M) * 8 + Debug) * 8 + Write * 4 + Prefix);
            X64PagedNativeCPU Native;
            ASSERT_NO_FATAL_FAILURE(initializeDebugOracle(Native));
            if (IsSkipped())
              return;
            startDebug(M);
            const auto Before = *Machine.Debug;
            registers().GPR[1] =
                Debug < 4 ? uint32_t(Before.DR[Debug]) : 0x2305;
            const auto Bytes = move(Debug, Write, Prefix, Prefix);
            const auto PC = add(0x1000, M, Bytes);
            add(PC, M, {0xf4});
            ASSERT_NO_FATAL_FAILURE(originalDebug(Native));
            kvm_debugregs D{};
            kvm_regs G{};
            ASSERT_EQ(ioctl(Native.CPU, KVM_GET_DEBUGREGS, &D), 0) << errno;
            ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &G), 0) << errno;
            for (unsigned I = 0; I != 4; ++I)
              EXPECT_EQ(D.db[I], Write && I == Debug ? uint32_t(Before.DR[I])
                                                     : Before.DR[I]);
            if (!Write) {
              const unsigned Index =
                  Debug == 4 || Debug == 5 ? Debug + 2 : Debug;
              const uint32_t Value = Index < 4    ? Before.DR[Index]
                                     : Index == 6 ? Before.DR6
                                                  : Before.DR7;
              EXPECT_EQ(G.rcx, Value);
            }
            EXPECT_EQ(G.rip, PC + 1);
            if (CompareOwner) {
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              uint64_t Budget = Program.size();
              auto Got = X86ModeExecution(*Memory, Options)
                             .run(Machine, Program, Budget);
              ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
              ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
              ASSERT_EQ(Got->Completions, Program.size());
              ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::Halted);
              ASSERT_NO_FATAL_FAILURE(compareDebugOriginal(Native));
            }
            ++Comparisons;
          }
    RecordProperty(CompareOwner ? "original_non64_debug_owner_comparisons"
                                : "original_non64_debug_observations",
                   Comparisons);
  }
};

TEST_F(X86ModeDebugRegisterTest, ObserveOriginalRegisterTransfers) {
  registerMatrix(false);
}

TEST_F(X86ModeDebugRegisterTest, OwnerMatchesOriginalRegisterTransfers) {
  registerMatrix(true);
}

TEST_F(X86ModeDebugRegisterTest,
       GeneratedLongModeObservesCompleteDebugAddresses) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Comparisons = 0;
  for (Mode M : {Mode::Compatibility16, Mode::Compatibility32})
    for (unsigned Debug = 0; Debug != 4; ++Debug)
      for (bool Write : {false, true})
        for (const auto &Compiler : Compilers)
          for (bool LLVM : {false, true})
            for (const std::string Optimization : {"-O0", "-O2"}) {
              SCOPED_TRACE(unsigned(M) * 8 + Debug * 2 + Write);
              X64PagedNativeCPU Native;
              ASSERT_NO_FATAL_FAILURE(initializeDebugOracle(Native));
              if (IsSkipped())
                return;
              startDebug(M);
              registers().GPR[1] = uint32_t(Machine.Debug->DR[Debug]);
              const auto PC = add(0x1000, M, move(Debug, Write, 3, 0));
              if (code32(M))
                add(PC, M, {0xea, 0, 0x11, 0, 0, 0x18, 0});
              else
                add(PC, M, {0xea, 0, 0x11, 0x18, 0});
              uint64_t At = 0x1100;
              for (unsigned I = 0; I != 4; ++I) {
                At =
                    add(At, Mode::Long64, {0x0f, 0x21, uint8_t(0xc0 | I << 3)});
                At = add(At, Mode::Long64,
                         {0x48, 0x89, 0x04, 0x25, uint8_t(I * 8), 0x30, 2, 0});
              }
              RAM.Memory.front().Bytes[At] = 0xf4;
              ASSERT_NO_FATAL_FAILURE(
                  compileLong(LLVM, Optimization, Compiler, true));
              ASSERT_NO_FATAL_FAILURE(originalDebug(Native));
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              uint64_t Budget = Program.size();
              auto Got = X86ModeExecution(*Memory, Options)
                             .run(Machine, Program, Budget);
              ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
              ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
              ASSERT_EQ(Got->Completions, Program.size());
              ASSERT_TRUE(Got->BudgetExhausted);
              ASSERT_NO_FATAL_FAILURE(compareDebugOriginal(Native, true));
              for (unsigned I = 0; I != 4; ++I)
                EXPECT_EQ(physical(0x23000 + I * 8, GuestScalarWidth::I64),
                          Machine.Debug->DR[I]);
              ++Comparisons;
            }
  RecordProperty("original_non64_debug_generated_comparisons", Comparisons);
}

TEST_F(X86ModeDebugRegisterTest, OriginalImmediateDebugUsesTheCommittedBank) {
  unsigned Comparisons = 0;
  for (Mode M : Modes)
    for (unsigned Case = 0; Case != 4; ++Case)
      for (bool Prefix : {false, true}) {
        SCOPED_TRACE(unsigned(M) * 8 + Case * 2 + Prefix);
        X64PagedNativeCPU Native;
        ASSERT_NO_FATAL_FAILURE(initializeDebugOracle(Native));
        if (IsSkipped())
          return;
        startDebug(M);
        registers().GPR[1] = Case == 2 ? 0x400 : 0x401;
        // Release KVM's lazy debug-register interception with GD clear. Set
        // the real guest bank and TF before the measured instruction.
        uint64_t PC = add(0x1000, M, {0x0f, 0x21, 0xfa});
        PC = add(PC, M, {0x0f, 0x23, 0xf9});
        std::vector<uint8_t> Immediate{0xb9, 0, 0, 0, 0};
        std::vector<uint8_t> Push{0x68, 2, 1, 0, 0}, Pop{0x9d};
        if (!code32(M)) {
          Immediate.insert(Immediate.begin(), 0x66);
          Push.insert(Push.begin(), 0x66);
          Pop.insert(Pop.begin(), 0x66);
        }
        const auto ImmediatePC = PC;
        const auto ImmediateIndex = Program.size();
        PC = add(PC, M, Immediate);
        PC = add(PC, M, Push);
        PC = add(PC, M, Pop);
        const auto End = add(PC, M, move(Case >= 2 ? 7 : 0, Case != 1, Prefix));
        Machine.Debug->DR[0] = (Case < 2 ? uint64_t{1} << 32 : 0) | End;
        const uint32_t Value = Case < 2 ? End : Case == 2 ? 0x401 : 0x400;
        for (unsigned I = 0; I != 4; ++I) {
          const auto Offset = Immediate.size() - 4 + I;
          Program[ImmediateIndex].Bytes[Offset] = Value >> (I * 8);
          RAM.Memory.front().Bytes[ImmediatePC + Offset] = Value >> (I * 8);
        }
        RAM.Memory.front().Bytes[End] = 0xf4;
        ASSERT_NO_FATAL_FAILURE(originalDebug(Native));
        kvm_debugregs D{};
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_DEBUGREGS, &D), 0) << errno;
        EXPECT_EQ(D.dr6, 0xffff4ff0u | (Case == 0 || Case == 2 ? 1 : 0));
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = Program.size();
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_EQ(Got->Completions, Program.size());
        EXPECT_EQ(Got->DeliveredVector, 1);
        EXPECT_EQ(bool(Got->DebugAddressWritten), Case == 0);
        ASSERT_NO_FATAL_FAILURE(compareDebugOriginal(Native, true));
        ++Comparisons;
      }
  RecordProperty("original_non64_debug_completion_comparisons", Comparisons);
}

TEST_F(X86ModeDebugRegisterTest, OriginalGeneralDetectAndOpcodePriority) {
  unsigned Comparisons = 0;
  for (Mode M : Modes)
    for (unsigned Case = 0; Case != 5; ++Case) {
      SCOPED_TRACE(unsigned(M) * 5 + Case);
      X64PagedNativeCPU Native;
      ASSERT_NO_FATAL_FAILURE(initializeDebugOracle(Native));
      if (IsSkipped())
        return;
      startDebug(M);
      const auto Before = Machine.Debug->DR;
      registers().GPR[1] = 0x2400;
      uint64_t PC = add(0x1000, M, {0x0f, 0x21, 0xfa});
      PC = add(PC, M, {0x0f, 0x23, 0xf9});
      if (Case == 4) {
        registers().GPR[3] = 16;
        PC = add(PC, M, {0x8e, 0xd3}); // Establish a real MOV-SS shadow.
      }
      auto Bytes = move(Case == 2 || Case == 3 ? 4 : 0, true);
      if (Case == 1)
        Bytes.insert(Bytes.begin(), 0xf0);
      if (Case == 2)
        Controls.CR4 |= 8;
      add(PC, M, Bytes);
      ASSERT_NO_FATAL_FAILURE(originalDebug(Native));
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      uint64_t Budget = Program.size();
      auto Got =
          X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
      ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_EQ(Got->Completions, Program.size() - 1);
      EXPECT_EQ(Got->DeliveredVector, Case == 1 || Case == 2 ? 6 : 1);
      EXPECT_FALSE(Got->DebugAddressWritten);
      EXPECT_EQ(Machine.Debug->DR, Before);
      ASSERT_NO_FATAL_FAILURE(compareDebugOriginal(Native, true));
      ++Comparisons;
    }
  RecordProperty("original_non64_debug_fault_comparisons", Comparisons);
}

TEST_F(X86ModeDebugRegisterTest, CompletedWriteSurvivesDebugEntryBackingStops) {
  unsigned Comparisons = 0;
  for (Mode M : Modes)
    for (bool GeneralDetect : {false, true})
      for (unsigned Stop = 0; Stop != 3; ++Stop) {
        SCOPED_TRACE(unsigned(M) * 6 + GeneralDetect * 3 + Stop);
        startDebug(M);
        registers().GPR[1] = 0x1003;
        registers().RFlags |= GeneralDetect ? 0 : 0x100;
        Machine.Debug->DR[0] = (uint64_t{1} << 32) | 0x1003;
        Machine.Debug->DR7 = GeneralDetect ? 0x2401 : 0x401;
        add(0x1000, M, move(0, true));
        const auto Initial = Machine;
        const auto Original = RAM;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = 1;
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
        const auto Expected = Machine;
        const auto ExpectedRAM =
            Memory->physicalMemory().snapshotMemoryRegions();
        Machine = Initial;
        RAM = Original;
        const bool Wide = Controls.EFER & 0x400;
        const unsigned Width = Wide ? 8 : 4;
        const unsigned Gate = Wide ? 16 : 8;
        const uint64_t Address = !Stop ? 0x9000 + Gate : 0xf000 - Stop * Width;
        const unsigned Size = !Stop ? Gate : Width;
        omitBacking(RAM, Address, Size);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        Budget = 1;
        Got = X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
        ASSERT_TRUE(Machine.Pending);
        EXPECT_EQ(Got->Attempts, 1u);
        EXPECT_EQ(Got->Completions, GeneralDetect ? 0u : 1u);
        EXPECT_EQ(bool(Got->DebugAddressWritten), !GeneralDetect);
        EXPECT_EQ(Machine.Debug->DR[0],
                  GeneralDetect ? Initial.Debug->DR[0] : 0x1003u);
        EXPECT_EQ(Budget, 0u);
        auto Snapshot = RAM;
        Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
        auto Frozen = Machine;
        Machine.Debug->DR[0] ^= uint64_t{1} << 32;
        Got = X86ModeExecution(*Memory, Options).run(Machine, {}, Budget);
        ASSERT_FALSE(bool(Got));
        llvm::consumeError(Got.takeError());
        Machine = Frozen;
        auto Changed = Options;
        Changed.Legacy.GeneralDetectFrame = X86GeneralDetectFrame::FaultResume;
        for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
          Got = X86ModeExecution(*Memory, Changed).run(Machine, {}, Budget);
          ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
          EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
          EXPECT_EQ(Got->Attempts, 0u);
          EXPECT_EQ(Got->Completions, 0u);
          EXPECT_FALSE(Got->DebugAddressWritten);
          EXPECT_EQ(Machine.Core.Core, Frozen.Core.Core);
          EXPECT_EQ(Machine.Debug, Frozen.Debug);
          const auto CurrentRAM =
              Memory->physicalMemory().snapshotMemoryRegions();
          ASSERT_EQ(CurrentRAM.size(), Snapshot.Memory.size());
          for (unsigned I = 0; I != CurrentRAM.size(); ++I) {
            EXPECT_EQ(CurrentRAM[I].Address, Snapshot.Memory[I].Address);
            EXPECT_EQ(CurrentRAM[I].Permissions,
                      Snapshot.Memory[I].Permissions);
            EXPECT_EQ(CurrentRAM[I].Bytes, Snapshot.Memory[I].Bytes);
          }
        }
        Snapshot.Memory.push_back(
            {Address, MemoryPermission::Read | MemoryPermission::Write, 0,
             std::vector<uint8_t>(
                 Original.Memory.front().Bytes.begin() + Address,
                 Original.Memory.front().Bytes.begin() + Address + Size)});
        auto Repaired = llvm::cantFail(
            X86PagedMemory::create(Snapshot, Features, Memory->controls()));
        Got = X86ModeExecution(*Repaired, Changed).run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_EQ(Got->Attempts, 0u);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_FALSE(Got->DebugAddressWritten);
        EXPECT_FALSE(Got->EventAccepted);
        EXPECT_FALSE(Machine.Pending);
        EXPECT_EQ(Machine.Core.Core, Expected.Core.Core);
        EXPECT_EQ(Machine.Debug, Expected.Debug);
        for (const auto &Region :
             Repaired->physicalMemory().snapshotMemoryRegions())
          EXPECT_TRUE(
              std::equal(Region.Bytes.begin(), Region.Bytes.end(),
                         ExpectedRAM.front().Bytes.begin() + Region.Address));
        ++Comparisons;
      }
  RecordProperty("non64_debug_resume_comparisons", Comparisons);
}

TEST_F(X86ModeDebugRegisterTest, OriginalPrivilegeAndUndefinedOpcodePrecedeGD) {
  unsigned Comparisons = 0;
  for (Mode M : Modes)
    for (unsigned Case = 0; Case != 3; ++Case) {
      SCOPED_TRACE(unsigned(M) * 3 + Case);
      X64PagedNativeCPU Native;
      ASSERT_NO_FATAL_FAILURE(initializeDebugOracle(Native));
      if (IsSkipped())
        return;
      startDebug(M);
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
      Machine.Debug->DR7 = 0x2400;
      const auto Before = Machine.Debug->DR;
      auto Bytes = move(Case == 2 ? 4 : 0, true);
      if (Case == 1)
        Bytes.insert(Bytes.begin(), 0xf0);
      if (Case == 2)
        Controls.CR4 |= 8;
      add(0x1000, M, Bytes);
      ASSERT_NO_FATAL_FAILURE(originalDebug(Native));
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      uint64_t Budget = 1;
      auto Got =
          X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
      ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_EQ(Got->Completions, 0u);
      EXPECT_EQ(Got->DeliveredVector, Case ? 6 : 13);
      EXPECT_FALSE(Got->DebugAddressWritten);
      EXPECT_EQ(Machine.Debug->DR, Before);
      ASSERT_NO_FATAL_FAILURE(compareDebugOriginal(Native, true));
      ++Comparisons;
    }
  RecordProperty("original_non64_debug_privilege_comparisons", Comparisons);
}

TEST_F(X86ModeDebugRegisterTest, FullAndLowDebugViewsMustAgreeBeforeFetch) {
  for (Mode M : Modes)
    for (unsigned Mismatch = 0; Mismatch != 5; ++Mismatch) {
      startDebug(M);
      X86LegacyExecutionState Low;
      Low.EIP = registers().PC;
      Low.EFlags = registers().RFlags;
      for (unsigned I = 0; I != 6; ++I) {
        const auto C = cache(I);
        Low.Core.Segments[I] = {uint32_t(C.Base), C.Limit, C.Selector,
                                C.Attributes, false};
      }
      Low.Debug.emplace();
      for (unsigned I = 0; I != 4; ++I)
        Low.Debug->DR[I] = Machine.Debug->DR[I];
      auto O = Options.Legacy;
      O.Non64Mode = X86Non64ModeBinding{Machine.Core.Core.GDTR.Base,
                                        Machine.Core.Core.LDTR.Base,
                                        Machine.Debug->DR, false};
      if (Mismatch < 4)
        O.Non64Mode->DebugAddresses[Mismatch] ^= 1;
      const auto Code = move(0, false);
      add(0x1000, M, Code);
      const auto Before = Low;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      const auto BeforeRAM = Memory->physicalMemory().snapshotMemoryRegions();
      auto Got = executeX86Non64Instruction(Low, *Memory, Code, O);
      if (Mismatch < 4) {
        ASSERT_FALSE(bool(Got));
        EXPECT_NE(
            llvm::toString(Got.takeError()).find("debug addresses disagree"),
            std::string::npos);
        EXPECT_EQ(Low.EIP, Before.EIP);
        EXPECT_EQ(Low.Debug, Before.Debug);
        EXPECT_EQ(Memory->controls(), Controls);
        EXPECT_EQ(
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            BeforeRAM.front().Bytes);
      } else {
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        EXPECT_TRUE(Got->InstructionCompleted);
        EXPECT_EQ(Low.Core.GPR[1], uint32_t(Machine.Debug->DR[0]));
        EXPECT_FALSE(Got->DebugAddressWritten);
      }
    }
}
#endif
