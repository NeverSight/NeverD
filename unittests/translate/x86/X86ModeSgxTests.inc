//===- X86ModeSgxTests.inc - Software enclave thread transitions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original x64 entry, IRQ, IRETQ, ERESUME and enclave-selected EEXIT target.
//===----------------------------------------------------------------------===//

class X86ModeSgxTest : public X86ModeInterruptTest {
protected:
  static void field(X86SgxPage &Page, unsigned At, uint64_t Value,
                    unsigned Size) {
    for (unsigned I = 0; I != Size; ++I)
      Page.Bytes[At + I] = Value >> (8 * I);
  }

  void checkPlatformEvent(std::vector<X86SgxPage> Pages,
                          const std::array<uint8_t, 160> &X87,
                          uint64_t InterruptedPC, bool INIT) {
    auto Bank = llvm::cantFail(X86SgxMemory::create(std::move(Pages)));
    llvm::cantFail(Bank->bindX87(X87, 7));
    auto C = Controls;
    C.CR4 |= 0x40;
    auto O = Options;
    O.Legacy.ControlRegisters->OtherCR4Features |= 0x40;
    auto M = llvm::cantFail(
        X86PagedMemory::createWithSgx(RAM, Features, C, std::move(Bank)));
    X86ModeExecution Execution(*M, O);
    auto S = Machine;
    S.MCA.emplace();
    S.MCA->Banks.resize(1);
    auto &R = S.Core.Core.Core.Core.Core.Core;
    R.RFlags &= ~uint64_t(0x100);
    uint64_t Budget = 1;
    ASSERT_EQ(llvm::cantFail(Execution.run(S, Program, Budget)).Completions,
              1u);
    ASSERT_TRUE(M->sgxMemory()->active());
    ASSERT_EQ(R.PC, InterruptedPC);
    X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
    if (INIT) {
      S.Startup = X86StartupState{0x906a0, false};
      Inputs.INITPending = true;
    } else {
      Inputs.MachineCheck =
          X86MachineCheckReport{0, 0xb000000000000105ull, 0x1234, 0x22, 7};
    }
    Budget = 0;
    auto Event = llvm::cantFail(Execution.run(S, Program, Budget, Inputs));
    ASSERT_TRUE(Event.EventAccepted) << Event.OwnerError;
    if (INIT) {
      ASSERT_TRUE(S.Startup->WaitingForSIPI);
      EXPECT_FALSE(Inputs.INITPending);
      EXPECT_FALSE(M->sgxMemory()->active());
      EXPECT_TRUE(M->sgxMemory()->enabled());
      // Booted outside software reestablishes paging and resumes the same
      // imported thread. INIT left its EPCM pages and interrupted SSA intact.
      S = Machine;
      R.RFlags &= ~uint64_t(0x100);
      R.GPR[0] = 3;
      llvm::cantFail(M->bindControls(C));
      Budget = 1;
      auto Resumed = llvm::cantFail(Execution.run(S, Program, Budget));
      ASSERT_EQ(Resumed.Completions, 1u) << Resumed.OwnerError;
      EXPECT_TRUE(M->sgxMemory()->active());
      EXPECT_EQ(R.PC, InterruptedPC);
      EXPECT_EQ(R.GPR[9], 0x1122334455667788ULL);
      return;
    }
    ASSERT_EQ(Event.DeliveredVector, 18u) << Event.OwnerError;
    EXPECT_FALSE(Inputs.MachineCheck);
    EXPECT_FALSE(M->sgxMemory()->active());
    EXPECT_FALSE(M->sgxMemory()->enabled());
    EXPECT_EQ(word(bytes(*M), R.GPR[4], 8), 0x1800u);
    S = Machine;
    R.RFlags &= ~uint64_t(0x100);
    R.GPR[0] = 3;
    C.CR0 |= 8;
    llvm::cantFail(M->bindControls(C));
    Budget = 1;
    auto Disabled = llvm::cantFail(Execution.run(S, Program, Budget));
    EXPECT_EQ(Disabled.Completions, 0u);
    EXPECT_EQ(Disabled.DeliveredVector, 6u);
    EXPECT_FALSE(M->sgxMemory()->active());
  }

  void checkInitialResume(const X86SgxPage &Secs, X86SgxPage Tcs,
                          const X86SgxPage &Code, X86SgxPage SSA,
                          X86SgxPage LastSSA,
                          const std::array<uint8_t, 160> &X87, bool MultiPage,
                          bool AVX) {
    field(Tcs, 24, 1, 4);
    auto &GPR = MultiPage ? LastSSA : SSA;
    field(GPR, 4096 - 184 + 136, Code.Linear, 8);
    for (uint32_t MXCSR : {0x3f80u, 0x10000u}) {
      SCOPED_TRACE(MXCSR);
      field(SSA, 24, MXCSR, 4);
      auto Enclave =
          llvm::cantFail(X86SgxMemory::create({Secs, Tcs, Code, SSA, LastSSA}));
      llvm::cantFail(Enclave->bindX87(X87, 7));
      auto Mem = llvm::cantFail(X86PagedMemory::createWithSgx(
          RAM, Features, Controls, std::move(Enclave)));
      auto State = Machine;
      auto &CPU = State.Core.Core.Core.Core.Core;
      CPU.Core.GPR[0] = 3;
      CPU.Core.RFlags &= ~uint64_t(0x100);
      X86ModeExecution Execution(*Mem, Options);
      uint64_t Budget = 1;
      auto Step = Execution.run(State, Program, Budget);
      ASSERT_TRUE(bool(Step)) << llvm::toString(Step.takeError());
      ASSERT_TRUE(Step->OwnerError.empty()) << Step->OwnerError;
      if (MXCSR & ~Options.MXCSRMask) {
        EXPECT_EQ(Step->Completions, 0u);
        EXPECT_EQ(Step->DeliveredVector, 13u);
        EXPECT_FALSE(Mem->sgxMemory()->active());
        continue;
      }
      ASSERT_EQ(Step->Completions, 1u);
      ASSERT_TRUE(Mem->sgxMemory()->active());
      EXPECT_EQ(CPU.MXCSR, MXCSR);
      EXPECT_EQ(CPU.Core.ZMM[2][0], 0u);
      EXPECT_EQ(CPU.Core.ZMM[2][16], AVX ? 0u : 0xc7u);
      EXPECT_EQ(CPU.Core.ZMM[2][32], 0x59u);
    }
  }
};

TEST_F(X86ModeSgxTest,
       SoftwareEnclaveResumesFromSSAAfterIRQAndInstructionFaults) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  constexpr uint64_t Base = 0x40000, Root = 0x50000;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  struct FaultCase {
    std::vector<uint8_t> Bytes;
    bool NXE = false;
  };
  const FaultCase Cases[]{
      {{}},
      {{0x8e, 0xe0}},
      {{0xcc}},
      {{0xf1}},
      {{0xf4}},
      {{0xc5, 0xf8, 0x77}},
      {{0x89, 0x06}},
      {{0xf0, 0x0f, 0xff, 0xc0}},
      {{0x67, 0xf0, 0x0f, 0xff}},
      {{0x67, 0xf0, 0x0f, 0xff}, true},
      {std::vector<uint8_t>(x86::kMaxInstructionLen, 0x40)}};
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const auto &[FaultBytes, NXE] : Cases) {
        SCOPED_TRACE(testing::Message()
                     << Compiler << " llvm=" << LLVM << " nxe=" << NXE
                     << ", bytes=" << testing::PrintToString(FaultBytes));
        const bool LockedUD0 =
            !FaultBytes.empty() && FaultBytes.front() == 0xf0;
        // Only the address tail is on the next, non-executable EPC page.
        const bool TailPF = !FaultBytes.empty() && FaultBytes.front() == 0x67;
        const bool PageFault =
            TailPF || (!FaultBytes.empty() && FaultBytes.front() == 0x89);
        const bool Overlong = FaultBytes.size() == x86::kMaxInstructionLen;
        const bool GPFault =
            Overlong || (!FaultBytes.empty() && FaultBytes.front() == 0xf4);
        const uint64_t FaultAddress = TailPF ? Base + 0x2000 : Base + 0x1027;
        const uint32_t PageError = TailPF ? (NXE ? 0x8015 : 0x8005) : 0x8007;
        const bool ErrorCode = PageFault || GPFault;
        const bool AVX =
            LLVM && (FaultBytes.empty() || FaultBytes.front() != 0xc5);
        const uint64_t XFRM = AVX ? 7 : 3;
        const uint64_t EnclaveXCR0 = LLVM ? XFRM : 7;
        start(Mode::Long64);
        if (NXE) {
          Features.NX = true;
          Controls.EFER |= 1u << 11;
        }
        if (LockedUD0 || TailPF)
          Options.Long.UD0Fetch = (TailPF || LLVM)
                                      ? X86UD0FetchProfile::CompleteModRM
                                      : X86UD0FetchProfile::OpcodeOnly;
        Options.Long.PrefixLengthFaultAfter15Bytes = Overlong;
        Machine.SMM.emplace();
        Machine.Debug.emplace();
        Options.Legacy.Debug = Options.Long.DebugProfile =
            X86InstructionDebugProfile::Watchpoints;
        auto &V = Machine.Core.Core;
        auto &R = V.Core.Core.Core.Core;
        V.Core.CS = {0, UINT32_MAX, 0x33, 0xa0fb};
        V.Core.SS = V.DS = V.ES = {0, UINT32_MAX, 0x3b, 0xc0f3};
        V.FS = V.GS = {UINT32_MAX, 0x3b, 0xc0f3};
        V.DS.Attributes |= 0x1000;
        R.FSBase = 0x73000;
        R.GSBase = 0x74000;
        R.GPR[0] = 2;
        R.GPR[1] = 0x1800;
        R.GPR[3] = Base;
        const bool OutsideTF = LLVM && FaultBytes.empty();
        if (OutsideTF)
          R.RFlags |= 0x100;
        R.GPR[5] = 0xdeadbeef;
        R.GPR[6] = Base + 0x1027;
        R.GPR[9] = 0x1122334455667788ULL;
        R.ZMM[2][0] = 0xab;
        R.ZMM[2][16] = 0xc7;
        R.ZMM[2][32] = 0x59;
        constexpr uint32_t OSFXSR = 1u << 9, OSXSave = 1u << 18;
        Controls.CR4 |= OSFXSR | (LLVM ? OSXSave : 0);
        Options.Legacy.ControlRegisters->OtherCR4Features |= OSFXSR | OSXSave;
        put(0x8030, 0x00affb000000ffffULL);
        put(0x8038, 0x00cff3000000ffffULL);
        put(0x10000, 0x11007);
        put(0x11000, 0x12007);
        put(0x12000, 0x87);
        put(0x9060, 0x1100 | (uint64_t(24) << 16) | (uint64_t(0x8e) << 40));
        put(0x9400, 0x1100 | (uint64_t(24) << 16) | (uint64_t(0x8e) << 40));
        Options.Long.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
        put(0x90d0, 0x1100 | (uint64_t(24) << 16) | (uint64_t(0x8e) << 40));
        put(0x90e0, 0x1100 | (uint64_t(24) << 16) | (uint64_t(0x8e) << 40));
        uint64_t HandlerPC = 0x1100;
        if (ErrorCode)
          HandlerPC = add(HandlerPC, Mode::Long64, {0x48, 0x83, 0xc4, 8});
        add(HandlerPC, Mode::Long64, {0x48, 0xcf});
        ASSERT_NO_FATAL_FAILURE(compileLong(LLVM, "-O2", Compiler));
        auto Handlers = Program;
        Program = {{Base + 0x1000, Mode::Long64, {0xb8, 4, 0, 0, 0}},
                   {Base + 0x1005, Mode::Long64, {0xbb, 0, 0x20, 0, 0}}};
        ASSERT_NO_FATAL_FAILURE(
            compileLong(LLVM, "-O2", Compiler, false, Base + 0x1000));
        X86SgxPage Secs{.Physical = Root, .Type = X86SgxPageType::SECS};
        X86SgxPage Tcs{.Physical = Base,
                       .Linear = Base,
                       .OwnerSECS = Root,
                       .Type = X86SgxPageType::TCS};
        X86SgxPage Code{.Physical = Base + 0x1000,
                        .Linear = Base + 0x1000,
                        .OwnerSECS = Root,
                        .Permissions =
                            MemoryPermission::Execute | MemoryPermission::Read};
        X86SgxPage SSA{.Physical = Base + 0x2000,
                       .Linear = Base + 0x2000,
                       .OwnerSECS = Root,
                       .Permissions =
                           MemoryPermission::Read | MemoryPermission::Write};
        field(Secs, 0, 0x4000, 8);
        field(Secs, 8, Base, 8);
        field(Secs, 16, LLVM ? 2 : 1, 4);
        field(Secs, 20, LLVM ? 1 : 0, 4);
        field(Secs, 48, 5, 8);
        field(Secs, 56, XFRM, 8);
        field(Tcs, 16, 0x2000, 8);
        field(Tcs, 28, 1, 4);
        const auto EntryOffset = TailPF ? 0x2000u - FaultBytes.size()
                                 : FaultBytes.empty() ? 0x1000u
                                                      : 0x100du;
        field(Tcs, 32, EntryOffset, 8);
        field(Tcs, 48, 0x3000, 8);
        field(Tcs, 64, UINT32_MAX, 4);
        field(Tcs, 68, UINT32_MAX, 4);
        const std::vector<uint8_t> Enclu{0x0f, 1, 0xd7};
        std::copy(Program[0].Bytes.begin(), Program[0].Bytes.end(),
                  Code.Bytes.begin());
        std::copy(Program[1].Bytes.begin(), Program[1].Bytes.end(),
                  Code.Bytes.begin() + 5);
        std::copy(Enclu.begin(), Enclu.end(), Code.Bytes.begin() + 10);
        Program.push_back({Base + 0x100a, Mode::Long64, Enclu});
        if (!FaultBytes.empty()) {
          std::copy(FaultBytes.begin(), FaultBytes.end(),
                    Code.Bytes.begin() + (EntryOffset - 0x1000));
          X86ModeInstruction Fault{Base + EntryOffset, Mode::Long64,
                                   FaultBytes};
          if (PageFault || LockedUD0 || Overlong) {
            auto Other = std::move(Program);
            Program = {Fault};
            ASSERT_NO_FATAL_FAILURE(
                compileLong(LLVM, "-O2", Compiler, false, Fault.Address));
            Fault = Program.front();
            Program = std::move(Other);
          }
          Program.push_back(std::move(Fault));
        }
        Program.insert(Program.end(), Handlers.begin(), Handlers.end());
        add(0x1000, Mode::Long64, Enclu);
        add(0x1800, Mode::Long64, Enclu);
        std::fill_n(SSA.Bytes.end() - 184 - 16, 16, 0xae);
        auto LastSSA = SSA;
        LastSSA.Physical = LastSSA.Linear = Base + 0x3000;
        auto Enclaves = llvm::cantFail(
            X86SgxMemory::create({Secs, Tcs, Code, SSA, LastSSA}));
        std::array<uint8_t, 160> X87{};
        X87[0] = 0x7f;
        X87[1] = 3;
        X87[32] = 0x13;
        if (FaultBytes.empty() || Overlong || TailPF)
          for (bool INIT : {false, true})
            ASSERT_NO_FATAL_FAILURE(
                checkPlatformEvent({Secs, Tcs, Code, SSA, LastSSA}, X87,
                                   Base + EntryOffset, INIT));
        if (FaultBytes.empty())
          ASSERT_NO_FATAL_FAILURE(checkInitialResume(Secs, Tcs, Code, SSA,
                                                     LastSSA, X87, LLVM, AVX));
        llvm::cantFail(Enclaves->bindX87(X87, 7));
        Memory = llvm::cantFail(X86PagedMemory::createWithSgx(
            RAM, Features, Controls, std::move(Enclaves)));
        auto SMRAM = RAM;
        const uint64_t RsmPC = 0x38000u + unsigned(Overlong);
        if (Overlong) {
          // The same 0x40 byte is INC AX in the firmware's real mode.
          SMRAM.Memory.front().Bytes[0x38000] = 0x40;
          Program.push_back({0x38000, Mode::Real16, {0x40}});
        }
        SMRAM.Memory.front().Bytes[RsmPC] = 0x0f;
        SMRAM.Memory.front().Bytes[RsmPC + 1] = 0xaa;
        auto System =
            llvm::cantFail(X86PagedMemory::create(SMRAM, Features, Controls));
        X86ModeExecution Execution(*Memory, Options, *System);
        Program.push_back({RsmPC, Mode::Real16, {0x0f, 0xaa}});
        uint64_t Budget = 1;
        auto Enter = llvm::cantFail(Execution.run(Machine, Program, Budget));
        ASSERT_TRUE(Enter.OwnerError.empty()) << Enter.OwnerError;
        ASSERT_EQ(Enter.Completions, 1u);
        ASSERT_TRUE(Memory->sgxMemory()->active());
        EXPECT_EQ(R.PC, Base + EntryOffset);
        EXPECT_EQ(V.FS.Attributes, 0xd0f3u);
        EXPECT_EQ(V.GS.Attributes, 0xd0f3u);
        EXPECT_EQ(R.FSBase, Base + 0x3000);
        EXPECT_FALSE(R.RFlags & 0x100);
        EXPECT_EQ(R.GPR[1], 0x1003u);
        EXPECT_EQ(Memory->sgxMemory()->xcr0(), EnclaveXCR0);
        Machine.SSBlocked = true;
        auto Deferred = llvm::cantFail(
            Execution.interrupt(Machine, X86EventKind::Interrupt, 64));
        EXPECT_EQ(Deferred.Status, X86LegacyExecutionStatus::EventDeferred);
        ASSERT_TRUE(Memory->sgxMemory()->active());
        Machine.SSBlocked = false;
        Budget = 1;
        auto Event = llvm::cantFail(
            FaultBytes.empty()
                ? Execution.interrupt(Machine, X86EventKind::Interrupt, 64)
                : Execution.run(Machine, Program, Budget));
        ASSERT_TRUE(Event.OwnerError.empty()) << Event.OwnerError;
        ASSERT_EQ(Event.Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_EQ(Event.DeliveredVector, FaultBytes.empty() ? 64u
                                         : PageFault        ? 14u
                                         : GPFault          ? 13u
                                                            : 6u);
        EXPECT_EQ(Event.Completions, 0u);
        EXPECT_EQ(R.PC, 0x1100u);
        const auto OutsideFrame = bytes(*Memory);
        EXPECT_EQ(word(OutsideFrame, R.GPR[4] + (ErrorCode ? 24 : 16), 8) &
                      0x10000,
                  0u);
        EXPECT_EQ(bool(word(OutsideFrame, R.GPR[4] + (ErrorCode ? 24 : 16), 8) &
                       0x100),
                  OutsideTF);
        if (PageFault) {
          EXPECT_EQ(word(OutsideFrame, R.GPR[4], 8), PageError);
          EXPECT_EQ(V.Core.Core.CR2, FaultAddress & ~uint64_t{0xfff});
        } else if (GPFault) {
          EXPECT_EQ(word(OutsideFrame, R.GPR[4], 8), 0u);
        }
        EXPECT_EQ(R.GPR[0], 3u);
        EXPECT_EQ(R.GPR[1], 0x1800u);
        EXPECT_EQ(R.GPR[3], Base);
        EXPECT_EQ(R.GPR[9], 0u);
        EXPECT_EQ(R.ZMM[2][0], 0u);
        EXPECT_EQ(R.ZMM[2][16], AVX ? 0u : 0xc7u);
        EXPECT_EQ(R.ZMM[2][32], 0x59u);
        EXPECT_EQ(Memory->sgxMemory()->xcr0(), 7u);
        EXPECT_EQ((*Memory->sgxMemory()->x87())[32], 0u);
        EXPECT_EQ(R.FSBase, 0x73000u);
        ASSERT_FALSE(Memory->sgxMemory()->active());
        Budget = ErrorCode ? 3 : 2;
        auto Resume = llvm::cantFail(Execution.run(Machine, Program, Budget));
        ASSERT_TRUE(Resume.OwnerError.empty()) << Resume.OwnerError;
        ASSERT_EQ(Resume.Completions, ErrorCode ? 3u : 2u);
        ASSERT_TRUE(Memory->sgxMemory()->active());
        EXPECT_EQ(R.PC, Base + EntryOffset);
        EXPECT_EQ(bool(R.RFlags & 0x10000), !FaultBytes.empty());
        EXPECT_EQ(R.GPR[9], 0x1122334455667788ULL);
        EXPECT_EQ(R.ZMM[2][0], 0xabu);
        EXPECT_EQ(R.ZMM[2][16], 0xc7u);
        EXPECT_EQ(R.ZMM[2][32], 0x59u);
        EXPECT_EQ(Memory->sgxMemory()->xcr0(), EnclaveXCR0);
        EXPECT_EQ((*Memory->sgxMemory()->x87())[32], 0x13u);
        const uint64_t GprArea = Base + (LLVM ? 0x4000 : 0x3000) - 184;
        std::vector<uint8_t> SavedMisc(16), ExitInfo(4), SavedIP(8);
        ASSERT_EQ(llvm::cantFail(
                      Memory->load({GprArea + 136, MemoryAccessKind::Read, 3},
                                   SavedIP))
                      .Status,
                  X86PageWalkStatus::Completed);
        EXPECT_EQ(word(SavedIP, 0, 8), Base + EntryOffset);
        ASSERT_EQ(llvm::cantFail(
                      Memory->load({GprArea - 16, MemoryAccessKind::Read, 3},
                                   SavedMisc))
                      .Status,
                  X86PageWalkStatus::Completed);
        ASSERT_EQ(llvm::cantFail(
                      Memory->load({GprArea + 160, MemoryAccessKind::Read, 3},
                                   ExitInfo))
                      .Status,
                  X86PageWalkStatus::Completed);
        if (LLVM && ErrorCode) {
          EXPECT_EQ(word(ExitInfo, 0, 4),
                    PageFault ? 0x8000030eu : 0x8000030du);
          EXPECT_EQ(word(SavedMisc, 0, 8), PageFault ? FaultAddress : 0u);
          EXPECT_EQ(word(SavedMisc, 8, 4), PageFault ? PageError : 0u);
          EXPECT_EQ(word(SavedMisc, 12, 4), 0u);
        } else {
          EXPECT_EQ(word(ExitInfo, 0, 4),
                    !FaultBytes.empty() && !ErrorCode ? 0x80000306u : 0u);
          EXPECT_EQ(SavedMisc, std::vector<uint8_t>(
                                   16, LLVM && FaultBytes.empty() ? 0 : 0xae));
        }
        if (!FaultBytes.empty() && !Overlong)
          continue;
        X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
        Inputs.SMIPending = true;
        Budget = 0;
        auto Unbound = Options;
        Unbound.Legacy.ControlRegisters->OtherCR4Features &= ~OSFXSR;
        X86ModeExecution Incomplete(*Memory, Unbound, *System);
        auto Refused = Incomplete.run(Machine, Program, Budget, Inputs);
        ASSERT_FALSE(Refused);
        EXPECT_NE(llvm::toString(Refused.takeError()).find("interrupted CR4"),
                  std::string::npos);
        EXPECT_TRUE(Inputs.SMIPending);
        EXPECT_TRUE(Memory->sgxMemory()->active());
        EXPECT_FALSE(Machine.SMM->Active);
        EXPECT_EQ(R.PC, Base + EntryOffset);
        EXPECT_EQ(bytes(*System), SMRAM.Memory.front().Bytes);
        ASSERT_TRUE(
            llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs))
                .EventAccepted);
        EXPECT_FALSE(Inputs.SMIPending);
        EXPECT_FALSE(Memory->sgxMemory()->active());
        ASSERT_TRUE(Machine.SMM->Active);
        EXPECT_EQ(word(bytes(*System), 0x3ff78, 8), 0x1800u);
        EXPECT_EQ(word(bytes(*System), 0x3fff8, 8), 3u);
        if (Overlong) {
          ASSERT_EQ(llvm::cantFail(Execution.mode(Machine)), Mode::Real16);
          const auto BeforeAX = R.GPR[0];
          Budget = 1;
          const auto Firmware =
              llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
          ASSERT_EQ(Firmware.Completions, 1u) << Firmware.OwnerError;
          EXPECT_EQ(uint16_t(R.GPR[0]), uint16_t(BeforeAX + 1));
          EXPECT_TRUE(Machine.SMM->Active);
        }
        Budget = 2;
        auto Returned =
            llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
        ASSERT_EQ(Returned.Completions, 2u) << Returned.OwnerError;
        ASSERT_FALSE(Machine.SMM->Active);
        ASSERT_TRUE(Memory->sgxMemory()->active());
        EXPECT_EQ(R.PC, Base + EntryOffset);
        EXPECT_EQ(R.GPR[9], 0x1122334455667788ULL);
        ASSERT_EQ(llvm::cantFail(Execution.mode(Machine)), Mode::Long64);
        if (Overlong) {
          // SMI intervened at the restored fault RIP. RSM and ERESUME must
          // restore that same instruction before the next attempt raises #GP.
          Budget = 1;
          const auto Retried =
              llvm::cantFail(Execution.run(Machine, Program, Budget));
          ASSERT_TRUE(Retried.OwnerError.empty()) << Retried.OwnerError;
          EXPECT_EQ(Retried.DeliveredVector, 13u);
          EXPECT_EQ(Retried.Completions, 0u);
          EXPECT_EQ(word(bytes(*Memory), R.GPR[4], 8), 0u);
          EXPECT_FALSE(Memory->sgxMemory()->active());
          continue;
        }
        Budget = 3;
        auto Exit = llvm::cantFail(Execution.run(Machine, Program, Budget));
        ASSERT_TRUE(Exit.OwnerError.empty()) << Exit.OwnerError;
        ASSERT_EQ(Exit.Completions, 3u);
        EXPECT_FALSE(Memory->sgxMemory()->active());
        if (OutsideTF) {
          EXPECT_EQ(Exit.DeliveredVector, 1u);
          EXPECT_TRUE(Machine.Debug->DR6 & 0x4000);
          EXPECT_EQ(word(bytes(*Memory), R.GPR[4], 8), 0x2000u);
        } else
          EXPECT_EQ(R.PC, 0x2000u);
        EXPECT_EQ(R.GPR[1], 0x1800u);
        EXPECT_EQ(R.FSBase, 0x73000u);
        EXPECT_EQ(R.GSBase, 0x74000u);
        EXPECT_EQ(Memory->sgxMemory()->xcr0(), 7u);
      }
}

// Signed Intel SDK simulation exercises the same DIV ECX instruction and
// repairs RCX=0 to 7 before retry. Re-enter the original TCS using its next
// SSA slot so that only enclave code can edit the interrupted register bank.
TEST_F(X86ModeSgxThreadTest, NestedHandlerRepairsDivideAtOriginalInstruction) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);

  constexpr uint64_t AEP = 0x1800, ResumeSite = 0x1820, ExitTarget = 0x2000;
  constexpr uint64_t Marker = UINT64_C(0x1122334455667788);
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(testing::Message() << Compiler << " llvm=" << LLVM);
      startThread();
      auto Bindings = std::move(Program);
      // Each original code range has its own linear base and exported bodies.
      const auto Compile =
          [&](uint64_t PC, bool Private,
              std::initializer_list<std::vector<uint8_t>> Ops) {
            Program.clear();
            const uint64_t First = PC;
            for (const auto &Bytes : Ops) {
              if (Private)
                privateInstruction(unsigned(PC - Code.Linear), Bytes);
              else
                add(PC, Mode::Long64, Bytes);
              PC += Bytes.size();
            }
            ASSERT_NO_FATAL_FAILURE(
                compileLong(LLVM, "-O2", Compiler, false, First));
            Bindings.insert(Bindings.end(), Program.begin(), Program.end());
          };
      const auto Mov = [](uint8_t Opcode, uint32_t Value) {
        std::vector<uint8_t> Bytes(5);
        Bytes[0] = Opcode;
        llvm::support::endian::write32le(Bytes.data() + 1, Value);
        return Bytes;
      };
      const auto Entry = Code.Linear;
      const auto SavedGPR = Previous.Linear + GPR;
      // EENTER returns CSSA in EAX. A nested entry takes the handler branch.
      ASSERT_NO_FATAL_FAILURE(Compile(Entry, true,
                                      {{0x85, 0xc0},
                                       {0x75, 0x1a},
                                       Mov(0xb8, 84),
                                       {0x89, 0xf9},
                                       {0x31, 0xd2},
                                       {0xf7, 0xf1},
                                       {0x89, 0xc6},
                                       Mov(0xb8, 4),
                                       Mov(0xbb, ExitTarget)}));
      ASSERT_NO_FATAL_FAILURE(Compile(
          Entry + 30, true,
          {{0xc7, 0x46, 8, 7, 0, 0, 0}, Mov(0xbb, ResumeSite), Mov(0xb8, 4)}));
      put(0x9000, 0x1100 | (uint64_t(24) << 16) | (uint64_t(0x8e) << 40));
      Options.Long.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
      ASSERT_NO_FATAL_FAILURE(Compile(0x1100, false, {{0x48, 0xcf}}));
      ASSERT_NO_FATAL_FAILURE(
          Compile(AEP, false, {Mov(0xb8, 2), Mov(0xbe, SavedGPR)}));
      ASSERT_NO_FATAL_FAILURE(Compile(
          ResumeSite, false, {Mov(0xb8, 3), Mov(0xbb, Base), Mov(0xb9, AEP)}));

      Program = std::move(Bindings);
      privateInstruction(27, Enclu);
      privateInstruction(47, Enclu);
      add(AEP + 10, Mode::Long64, Enclu);
      add(ResumeSite + 15, Mode::Long64, Enclu);
      bindThread();
      X86ModeExecution Execution(*Memory, Options);
      auto &R = Machine.Core.Core.Core.Core.Core.Core;
      const auto Run = [&](uint64_t Budget) {
        return llvm::cantFail(Execution.run(Machine, Program, Budget));
      };
      const auto Complete = [&](uint64_t Count) {
        auto Result = Run(Count);
        ASSERT_TRUE(Result.OwnerError.empty()) << Result.OwnerError;
        ASSERT_EQ(Result.Completions, Count);
      };
      for (uint32_t Divisor : {7u, 0u, 0u}) {
        SCOPED_TRACE(Divisor);
        R.PC = 0x1000;
        R.GPR[0] = 2;
        R.GPR[1] = AEP;
        R.GPR[3] = Base;
        R.GPR[7] = Divisor;
        R.GPR[9] = Marker;
        ASSERT_NO_FATAL_FAILURE(Complete(6));
        ASSERT_TRUE(Memory->sgxMemory()->active());
        ASSERT_EQ(R.PC, Entry + 13);
        if (!Divisor) {
          const auto Fault = Run(1);
          ASSERT_TRUE(Fault.OwnerError.empty()) << Fault.OwnerError;
          ASSERT_EQ(Fault.Status, X86LegacyExecutionStatus::EventDelivered);
          ASSERT_EQ(Fault.DeliveredVector, 0u);
          ASSERT_EQ(Fault.Completions, 0u);
          ASSERT_FALSE(Memory->sgxMemory()->active());
          ASSERT_EQ(R.PC, 0x1100u);
          EXPECT_EQ(word(bytes(*Memory), R.GPR[4], 8), AEP);
          ASSERT_NO_FATAL_FAILURE(Complete(4));
          ASSERT_TRUE(Memory->sgxMemory()->active());
          ASSERT_EQ(R.PC, Entry);
          ASSERT_EQ(R.GPR[0], 1u);
          std::vector<uint8_t> SSA(184);
          ASSERT_EQ(
              llvm::cantFail(
                  Memory->load({SavedGPR, MemoryAccessKind::Read, 3}, SSA))
                  .Status,
              X86PageWalkStatus::Completed);
          EXPECT_EQ(word(SSA, 0, 8), 84u);
          EXPECT_EQ(word(SSA, 8, 8), 0u);
          EXPECT_EQ(word(SSA, 16, 8), 0u);
          EXPECT_EQ(word(SSA, 9 * 8, 8), Marker);
          EXPECT_EQ(word(SSA, 136, 8), Entry + 13);
          EXPECT_EQ(word(SSA, 160, 4), 0x80000300u);
          ASSERT_NO_FATAL_FAILURE(Complete(10));
          ASSERT_TRUE(Memory->sgxMemory()->active());
          ASSERT_EQ(R.PC, Entry + 13);
          EXPECT_EQ(R.GPR[0], 84u);
          EXPECT_EQ(R.GPR[1], 7u);
          EXPECT_EQ(R.GPR[2], 0u);
          EXPECT_EQ(R.GPR[9], Marker);
        }
        ASSERT_NO_FATAL_FAILURE(Complete(1));
        ASSERT_EQ(R.GPR[0], 12u);
        ASSERT_NO_FATAL_FAILURE(Complete(4));
        ASSERT_FALSE(Memory->sgxMemory()->active());
        EXPECT_EQ(R.PC, ExitTarget);
        EXPECT_EQ(R.GPR[6], 12u);
      }
    }
}
