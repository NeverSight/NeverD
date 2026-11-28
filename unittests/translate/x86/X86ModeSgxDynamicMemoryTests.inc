//===- X86ModeSgxDynamicMemoryTests.inc - SGX2 enclave pages --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify EDMM through original ENCLU, subsequent EPC accesses and AEX.
//===----------------------------------------------------------------------===//

TEST_F(X86ModeSgxThreadTest,
       AcceptedPagesRetainTheirBytesAndExtendPermissions) {
  namespace le = llvm::support::endian;
  for (bool Copy : {false, true}) {
    SCOPED_TRACE(Copy);
    startThread();
    privateInstruction(0, {0x67, 0x0f, 1, 0xd7});
    privateInstruction(4, Enclu);
    Next.Pending = true;
    Next.Bytes.fill(0xa5);
    // EACCEPT must match the imported RW page. EACCEPTCOPY supplies RX.
    le::write64le(Code.Bytes.data() + 512, Copy ? 0x205 : 0x20b);
    // Extend a readable page with just the missing bit. Existing bits survive.
    le::write64le(Code.Bytes.data() + 576, Copy ? 2 : 4);
    bindThread({.SGX2 = true});
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    std::array<uint8_t, 4096> Data{};
    const auto Before = llvm::cantFail(
        Memory->load({Next.Linear, MemoryAccessKind::Read, 3}, Data));
    ASSERT_TRUE(Before.Exception);
    EXPECT_EQ(Before.Exception->ErrorCode, 0x8005u);
    R.GPR[0] = 0xabcdef1200000000ULL | (Copy ? 7 : 5);
    R.GPR[3] = Code.Linear + 512;
    R.GPR[1] = Next.Linear;
    R.GPR[2] = Code.Linear;
    R.RFlags |= 0x108d5;
    const uint64_t Flags = R.RFlags;
    Budget = 1;
    const auto Done = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Done.Completions, 1u) << Done.OwnerError;
    EXPECT_EQ(R.PC, Code.Linear + 4);
    EXPECT_EQ(R.GPR[0], 0u);
    EXPECT_EQ(R.RFlags, Flags & ~uint64_t{0x108d5});
    ASSERT_EQ(llvm::cantFail(
                  Memory->load({Next.Linear, MemoryAccessKind::Read, 3}, Data))
                  .Status,
              X86PageWalkStatus::Completed);
    EXPECT_EQ(Data, Copy ? Code.Bytes : Next.Bytes);
    const auto MissingAccess =
        Copy ? MemoryAccessKind::Write : MemoryAccessKind::Execute;
    EXPECT_TRUE(
        llvm::cantFail(Memory->probe({Next.Linear, MissingAccess, 3}, 1))
            .Exception);
    R.GPR[0] = 0xffaa000000000006ULL;
    R.GPR[3] = Code.Linear + 576;
    R.RFlags = Flags;
    Budget = 1;
    auto Modified = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Modified.Completions, 1u) << Modified.OwnerError;
    EXPECT_EQ(R.PC, Code.Linear + 7);
    EXPECT_EQ(R.GPR[0], 0xffaa000000000006ULL);
    EXPECT_EQ(R.RFlags, Flags & ~uint64_t{0x10000});
    std::array<uint8_t, 1> Marker{0x5a};
    EXPECT_EQ(
        llvm::cantFail(
            Memory->store({Next.Linear, MemoryAccessKind::Write, 3}, Marker))
            .Status,
        X86PageWalkStatus::Completed);
    EXPECT_EQ(
        llvm::cantFail(
            Memory->load({Next.Linear, MemoryAccessKind::Execute, 3}, Marker))
            .Status,
        X86PageWalkStatus::Completed);
    EXPECT_EQ(Marker[0], 0x5a);
  }
}

TEST_F(X86ModeSgxThreadTest, AcceptDistinguishesPageAttributesFromTracking) {
  enum Scenario {
    Mismatch,
    NotTracked,
    Unbound,
    Restricted,
    Trim,
    NewTCS,
    BadTCS,
    CopyMismatch
  };
  for (auto Case : {Mismatch, NotTracked, Unbound, Restricted, Trim, NewTCS,
                    BadTCS, CopyMismatch}) {
    SCOPED_TRACE(unsigned(Case));
    startThread();
    privateInstruction(0, Enclu);
    privateInstruction(3, Enclu);
    Next.Pending = true;
    Next.Bytes.fill(0x63);
    Next.TrackingComplete = Case != NotTracked;
    if (Case == Unbound)
      Next.TrackingComplete.reset();
    uint64_t Flags = 0x20b;
    if (Case == Mismatch)
      Flags = 0x209; // Requested read, imported read/write.
    if (Case == CopyMismatch || Case == Restricted || Case == NotTracked ||
        Case == Unbound) {
      Next.Pending = false;
      Next.PermissionRestricted = Case != CopyMismatch;
      Flags = Next.PermissionRestricted ? 0x223 : 0x207;
    }
    if (Case == Trim || Case == NewTCS || Case == BadTCS) {
      Next.Pending = false;
      Next.Modified = true;
      Next.Type = Case == Trim ? X86SgxPageType::Trimmed : X86SgxPageType::TCS;
      Next.Permissions = MemoryPermission::None;
      Next.Bytes = TCS.Bytes;
      if (Case == BadTCS)
        Next.Bytes[4080] = 1;
      Flags = Case == Trim ? 0x410 : 0x110;
    }
    llvm::support::endian::write64le(Code.Bytes.data() + 512, Flags);
    bindThread({.SGX2 = true});
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    const uint64_t Input = Case == CopyMismatch ? 7 : 5;
    R.GPR[0] = Input;
    R.GPR[3] = Code.Linear + 512;
    R.GPR[1] = Next.Linear;
    R.GPR[2] = Code.Linear;
    R.RFlags |= 0x108d5;
    const auto PriorFlags = R.RFlags, Prior = Memory->sgxMemory()->generation();
    Budget = 1;
    const auto Done = llvm::cantFail(Execution.run(Machine, Program, Budget));
    if (Case == BadTCS) {
      EXPECT_EQ(Done.DeliveredVector, 13u);
      EXPECT_EQ(Done.Completions, 0u);
      continue;
    }
    if (Case == Unbound) {
      EXPECT_EQ(Done.Status, X86LegacyExecutionStatus::ModeUnavailable);
      EXPECT_EQ(Done.Completions, 0u);
      EXPECT_EQ(R.PC, Code.Linear);
      EXPECT_EQ(R.GPR[0], Input);
      EXPECT_EQ(R.RFlags, PriorFlags);
      EXPECT_EQ(Memory->sgxMemory()->generation(), Prior);
      llvm::cantFail(
          Memory->sgxMemory()->bindPageTracking(Next.Physical, Root, true));
      Budget = 1;
      auto Retry = llvm::cantFail(Execution.run(Machine, Program, Budget));
      ASSERT_EQ(Retry.Completions, 1u) << Retry.OwnerError;
      EXPECT_EQ(R.GPR[0], 0u);
      EXPECT_EQ(R.PC, Code.Linear + 3);
      continue;
    }
    const unsigned Error = Case == Mismatch || Case == CopyMismatch ? 19
                           : Case == NotTracked                     ? 11
                                                                    : 0;
    ASSERT_EQ(Done.Completions, 1u) << Done.OwnerError;
    EXPECT_EQ(R.PC, Code.Linear + 3);
    EXPECT_EQ(R.GPR[0], Error);
    EXPECT_EQ(R.RFlags, (PriorFlags & ~uint64_t{0x108d5}) | (Error ? 0x40 : 0));
    EXPECT_EQ(Memory->sgxMemory()->generation(), Prior + !Error);
    if (Case == NotTracked) {
      auto &EPC = *Memory->sgxMemory();
      auto WrongOwner = EPC.bindPageTracking(Next.Physical, Root + 4096, true);
      ASSERT_TRUE(bool(WrongOwner));
      llvm::consumeError(std::move(WrongOwner));
      EXPECT_EQ(EPC.generation(), Prior);
      llvm::cantFail(EPC.bindPageTracking(Next.Physical, Root, true));
      llvm::cantFail(EPC.bindPageTracking(Next.Physical, Root, true));
      EXPECT_EQ(EPC.generation(), Prior + 1);
      R.GPR[0] = 5;
      Budget = 1;
      auto Retry = llvm::cantFail(Execution.run(Machine, Program, Budget));
      ASSERT_EQ(Retry.Completions, 1u) << Retry.OwnerError;
      EXPECT_EQ(R.GPR[0], 0u);
      EXPECT_EQ(R.PC, Code.Linear + 6);
    }
    if (Case == CopyMismatch) {
      std::array<uint8_t, 4096> Bytes{};
      ASSERT_EQ(
          llvm::cantFail(
              Memory->load({Next.Linear, MemoryAccessKind::Read, 3}, Bytes))
              .Status,
          X86PageWalkStatus::Completed);
      EXPECT_EQ(Bytes, Next.Bytes);
    }
  }
}

TEST_F(X86ModeSgxThreadTest, DynamicPageFaultsKeepOperandOrderAndOriginalPC) {
  constexpr uint64_t Info = Base + 0x1400, Dest = Base + 0x3000,
                     Missing = Base + 0x4000;
  struct Case {
    unsigned Leaf;
    bool SGX2;
    uint64_t BX, CX, DX, Flags;
    uint8_t Vector;
    uint32_t Error;
    uint64_t Address;
  };
  for (const auto &C :
       {Case{5, false, Missing, 1, 1, 0, 13, 0, 0},
        Case{6, false, Missing, 1, 1, 0, 13, 0, 0},
        Case{7, false, Missing, 1, 1, 0, 13, 0, 0},
        Case{5, true, Missing, 1, 1, 0, 14, 0x8005, Missing},
        Case{6, true, Missing, 1, 1, 0, 13, 0, 0},
        Case{7, true, Missing, Dest, 1, 0, 13, 0, 0},
        Case{7, true, Info, Missing, Dest, 0x10000, 14, 0x8007, Missing},
        Case{7, true, Info, Dest, Missing, 0x10000, 14, 0x8005, Missing},
        Case{5, true, Info, Missing, 0, 0x10000, 13, 0, 0},
        Case{5, true, Info, Missing, 0, 0, 14, 0x8005, Missing},
        Case{5, true, Info, Dest, 0, 0, 13, 0, 0},
        Case{6, true, Info, Missing, 0, 0x10000, 14, 0x8005, Missing},
        Case{6, true, Info, Dest, 0, 0x10000, 13, 0, 0},
        Case{7, true, Info, Dest, Base, 0x207, 14, 0x8005, Base},
        Case{7, true, Info, Dest, Base, 0x202, 13, 0, 0}}) {
    SCOPED_TRACE(testing::Message()
                 << C.Leaf << " BX=" << C.BX << " CX=" << C.CX << " DX=" << C.DX
                 << " flags=" << C.Flags);
    startThread();
    privateInstruction(0, {0x67, 0x0f, 1, 0xd7});
    namespace le = llvm::support::endian;
    le::write32le(Secs.Bytes.data() + 20, 1);
    le::write64le(Code.Bytes.data() + 0x400, C.Flags);
    const auto Outside = Machine;
    bindThread({.SGX2 = C.SGX2});
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = C.Leaf;
    R.GPR[3] = C.BX;
    R.GPR[1] = C.CX;
    R.GPR[2] = C.DX;
    Budget = 1;
    auto Fault = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Fault.DeliveredVector, C.Vector) << Fault.OwnerError;
    ASSERT_EQ(Fault.Completions, 0u);
    auto Frame = bytes(*Memory);
    EXPECT_EQ(word(Frame, R.GPR[4], 8), C.Error);
    EXPECT_EQ(word(Frame, R.GPR[4] + 8, 8), 0x1800u);
    Machine = Outside;
    llvm::cantFail(Memory->bindControls(Controls));
    R.GPR[0] = 3;
    Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    EXPECT_EQ(R.PC, Code.Linear);
    EXPECT_EQ(R.GPR[0], C.Leaf);
    EXPECT_EQ(R.GPR[3], C.BX);
    std::array<uint8_t, 16> ExInfo{};
    ASSERT_EQ(llvm::cantFail(Memory->load({Previous.Linear + GPR - 16,
                                           MemoryAccessKind::Read, 3},
                                          ExInfo))
                  .Status,
              X86PageWalkStatus::Completed);
    EXPECT_EQ(le::read64le(ExInfo.data()), C.Address);
    EXPECT_EQ(le::read32le(ExInfo.data() + 8), C.Error);
  }
}

TEST_F(X86ModeSgxThreadTest, TimestampAdmissionUsesEnclaveFeaturesAndTsd) {
  for (bool SGX2 : {false, true})
    for (bool TSDisablesUser : {false, true})
      for (auto Bytes : {std::vector<uint8_t>{0x0f, 0x31},
                         std::vector<uint8_t>{0x0f, 1, 0xf9}}) {
        SCOPED_TRACE(testing::Message()
                     << SGX2 << ' ' << TSDisablesUser << ' ' << Bytes.size());
        startThread();
        privateInstruction(0, Bytes);
        if (TSDisablesUser) {
          Controls.CR4 |= 4;
          Options.Legacy.ControlRegisters->OtherCR4Features |= 4;
        }
        bindThread({.SGX2 = SGX2});
        X86ModeExecution Execution(*Memory, Options);
        uint64_t Budget = 1;
        ASSERT_EQ(
            llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
            1u);
        Budget = 1;
        auto Step = Execution.run(Machine, Program, Budget);
        if (SGX2 && !TSDisablesUser) {
          // This case admits the timestamp instruction. No generated
          // timestamp body was supplied by this fixture.
          ASSERT_FALSE(bool(Step));
          EXPECT_NE(llvm::toString(Step.takeError())
                        .find("matching generated source body"),
                    std::string::npos);
          EXPECT_TRUE(Memory->sgxMemory()->active());
        } else {
          ASSERT_TRUE(bool(Step)) << llvm::toString(Step.takeError());
          EXPECT_EQ(Step->Completions, 0u);
          EXPECT_EQ(Step->DeliveredVector, SGX2 ? 13u : 6u) << Step->OwnerError;
          EXPECT_FALSE(Memory->sgxMemory()->active());
        }
      }
}

TEST_F(X86ModeSgxThreadTest, CompiledBodiesUseTheAcceptedPrivatePage) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  constexpr uint64_t Value = 0x9173aaf301020304;
  namespace le = llvm::support::endian;
  unsigned Completed = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const auto *Level : {"-O0", "-O2"}) {
        SCOPED_TRACE(testing::Message()
                     << Compiler << " llvm=" << LLVM << ' ' << Level);
        startThread();
        privateInstruction(0, {0x67, 0x0f, 1, 0xd7});
        auto Transfers = std::move(Program);
        Program.clear();
        // Read the copied marker, then store it in a different qword in the
        // accepted page. RCX still contains the EACCEPTCOPY destination.
        privateInstruction(4, {0x48, 0x8b, 0x81, 0, 1, 0, 0});
        privateInstruction(11, {0x48, 0x89, 0x81, 8, 1, 0, 0});
        ASSERT_NO_FATAL_FAILURE(
            compileLong(LLVM, Level, Compiler, false, Code.Linear + 4));
        Program.insert(Program.end(), Transfers.begin(), Transfers.end());
        le::write64le(Code.Bytes.data() + 256, Value);
        le::write64le(Code.Bytes.data() + 512, 0x203);
        Next.Pending = true;
        Next.Bytes.fill(0xa5);
        bindThread({.SGX2 = true});
        X86ModeExecution Execution(*Memory, Options);
        uint64_t Budget = 1;
        ASSERT_EQ(
            llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
            1u);
        auto &R = Machine.Core.Core.Core.Core.Core.Core;
        R.GPR[0] = 7;
        R.GPR[3] = Code.Linear + 512;
        R.GPR[1] = Next.Linear;
        R.GPR[2] = Code.Linear;
        Budget = 3;
        auto Done = Execution.run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
        ASSERT_EQ(Done->Completions, 3u) << Done->OwnerError;
        EXPECT_EQ(R.PC, Code.Linear + 18);
        EXPECT_EQ(R.GPR[0], Value);
        std::array<uint8_t, 8> Data{};
        ASSERT_EQ(llvm::cantFail(
                      Memory->load(
                          {Next.Linear + 264, MemoryAccessKind::Read, 3}, Data))
                      .Status,
                  X86PageWalkStatus::Completed);
        EXPECT_EQ(le::read64le(Data.data()), Value);
        ASSERT_EQ(llvm::cantFail(
                      Memory->load(
                          {Code.Linear + 264, MemoryAccessKind::Read, 3}, Data))
                      .Status,
                  X86PageWalkStatus::Completed);
        EXPECT_EQ(le::read64le(Data.data()), 0u);
        ++Completed;
      }
  RecordProperty("compiled_executions", Completed);
  EXPECT_EQ(Completed, Compilers.size() * 4);
}
