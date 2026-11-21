//===- X86ModeSgxTests.inc - Software enclave thread transitions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original x64 entry, IRQ, IRETQ, ERESUME and enclave-selected EEXIT target.
//===----------------------------------------------------------------------===//

TEST_F(X86ModeInterruptTest,
       SoftwareEnclaveResumesFromSSAAfterIRQAndInstructionFaults) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  constexpr uint64_t Base = 0x40000, Root = 0x50000;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::vector<uint8_t> FaultBytes : {std::vector<uint8_t>{},
                                                    {0x8e, 0xe0},
                                                    {0xcc},
                                                    {0xf1},
                                                    {0xf4},
                                                    {0x89, 0x06}}) {
        SCOPED_TRACE(testing::Message()
                     << Compiler << " llvm=" << LLVM
                     << ", instruction length=" << FaultBytes.size());
        const bool PageFault =
            !FaultBytes.empty() && FaultBytes.front() == 0x89;
        const bool GPFault = !FaultBytes.empty() && FaultBytes.front() == 0xf4;
        const bool ErrorCode = PageFault || GPFault;
        start(Mode::Long64);
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
        constexpr uint32_t OSFXSR = 1u << 9;
        Controls.CR4 |= OSFXSR;
        Options.Legacy.ControlRegisters->OtherCR4Features |= OSFXSR;
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
        const auto Field = [](X86SgxPage &Page, unsigned At, uint64_t Value,
                              unsigned Size) {
          for (unsigned I = 0; I != Size; ++I)
            Page.Bytes[At + I] = Value >> (8 * I);
        };
        Field(Secs, 0, 0x4000, 8);
        Field(Secs, 8, Base, 8);
        Field(Secs, 16, LLVM ? 2 : 1, 4);
        Field(Secs, 20, LLVM ? 1 : 0, 4);
        Field(Secs, 48, 5, 8);
        Field(Secs, 56, 3, 8);
        Field(Tcs, 16, 0x2000, 8);
        Field(Tcs, 28, 1, 4);
        const auto EntryOffset = FaultBytes.empty() ? 0x1000u : 0x100du;
        Field(Tcs, 32, EntryOffset, 8);
        Field(Tcs, 48, 0x3000, 8);
        Field(Tcs, 64, UINT32_MAX, 4);
        Field(Tcs, 68, UINT32_MAX, 4);
        const std::vector<uint8_t> Enclu{0x0f, 1, 0xd7};
        std::copy(Program[0].Bytes.begin(), Program[0].Bytes.end(),
                  Code.Bytes.begin());
        std::copy(Program[1].Bytes.begin(), Program[1].Bytes.end(),
                  Code.Bytes.begin() + 5);
        std::copy(Enclu.begin(), Enclu.end(), Code.Bytes.begin() + 10);
        Program.push_back({Base + 0x100a, Mode::Long64, Enclu});
        if (!FaultBytes.empty()) {
          std::copy(FaultBytes.begin(), FaultBytes.end(),
                    Code.Bytes.begin() + 13);
          X86ModeInstruction Fault{Base + EntryOffset, Mode::Long64,
                                   FaultBytes};
          if (PageFault) {
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
        llvm::cantFail(Enclaves->bindX87(X87, 3));
        Memory = llvm::cantFail(X86PagedMemory::createWithSgx(
            RAM, Features, Controls, std::move(Enclaves)));
        auto SMRAM = RAM;
        SMRAM.Memory.front().Bytes[0x38000] = 0x0f;
        SMRAM.Memory.front().Bytes[0x38001] = 0xaa;
        auto System =
            llvm::cantFail(X86PagedMemory::create(SMRAM, Features, Controls));
        X86ModeExecution Execution(*Memory, Options, *System);
        Program.push_back({0x38000, Mode::Real16, {0x0f, 0xaa}});
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
          EXPECT_EQ(word(OutsideFrame, R.GPR[4], 8), 0x8007u);
          EXPECT_EQ(V.Core.Core.CR2, Base + 0x1000);
        }
        EXPECT_EQ(R.GPR[0], 3u);
        EXPECT_EQ(R.GPR[1], 0x1800u);
        EXPECT_EQ(R.GPR[3], Base);
        EXPECT_EQ(R.GPR[9], 0u);
        EXPECT_EQ(R.ZMM[2][0], 0u);
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
        EXPECT_EQ((*Memory->sgxMemory()->x87())[32], 0x13u);
        const uint64_t GprArea = Base + (LLVM ? 0x4000 : 0x3000) - 184;
        std::vector<uint8_t> SavedMisc(16), ExitInfo(4);
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
          EXPECT_EQ(word(SavedMisc, 0, 8), PageFault ? Base + 0x1027 : 0u);
          EXPECT_EQ(word(SavedMisc, 8, 4), PageFault ? 0x8007u : 0u);
          EXPECT_EQ(word(SavedMisc, 12, 4), 0u);
        } else {
          EXPECT_EQ(word(ExitInfo, 0, 4),
                    !FaultBytes.empty() && !ErrorCode ? 0x80000306u : 0u);
          EXPECT_EQ(SavedMisc, std::vector<uint8_t>(
                                   16, LLVM && FaultBytes.empty() ? 0 : 0xae));
        }
        if (!FaultBytes.empty())
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
        Budget = 2;
        auto Returned =
            llvm::cantFail(Execution.run(Machine, Program, Budget, Inputs));
        ASSERT_EQ(Returned.Completions, 2u) << Returned.OwnerError;
        ASSERT_FALSE(Machine.SMM->Active);
        ASSERT_TRUE(Memory->sgxMemory()->active());
        EXPECT_EQ(R.PC, Base + EntryOffset);
        EXPECT_EQ(R.GPR[9], 0x1122334455667788ULL);
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
      }
}
