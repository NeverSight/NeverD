//===- X86ModeExecutionTests.inc - Continuous mode transitions -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original protected/compatibility/long instruction streams and source bodies.
//===----------------------------------------------------------------------===//

class X86ModeExecutionTest : public X64PagedExecutionTest {
protected:
  X86ModeState Machine;
  X86ModeExecutionOptions Options;
  std::vector<X86ModeInstruction> Program;

  void resetMode() {
    reset({});
    Machine = {};
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    R.PC = 0x1000;
    R.GPR[3] = 0x12345678;
    R.GPR[4] = 0xf000;
    V.Core.Core.Core.MXCSR = 0x1f80;
    V.Core.CS = {0, UINT32_MAX, 8, 0xc09b};
    V.Core.SS = V.DS = V.ES = {0, UINT32_MAX, 16, 0xc093};
    V.FS = V.GS = {UINT32_MAX, 16, 0xc093};
    V.GDTR = {0x8000, 0x3f};
    V.IDTR = {0x9000, 0xfff};
    V.TR = {0xa000, 0x67, 40, 0x8b};
    V.Unusable = 64;
    std::fill(RAM.Memory.front().Bytes.begin(), RAM.Memory.front().Bytes.end(),
              0);
    put(0x8008, 0x00cf9b000000ffffULL);
    put(0x8010, 0x00cf93000000ffffULL);
    put(0x8018, 0x00af9b000000ffffULL);
    put(0x10000, 0x11003);
    put(0x11000, 0x12003);
    put(0x12000, 0x83);
    put(0xa004, 0x1f000);
    for (unsigned Vector = 0; Vector != 256; ++Vector) {
      put(0x9000 + Vector * 16, (0x4000 + Vector * 16) | (uint64_t(24) << 16) |
                                    (uint64_t(0x8e) << 40));
      RAM.Memory.front().Bytes[0x4000 + Vector * 16] = 0xf4;
    }
    Controls = {0x10031, 0, 0, 0, 2};
    Options = {};
    Options.MXCSRMask = 0xffff;
    Options.PagingAddressBits = 48;
    Options.Undefined.emplace();
    Options.Legacy.Events = X86LegacyEventProfile::OrderedFrame;
    Options.Legacy.Segments.Profile =
        X86LegacySegmentProfile::WordStackOffsetFirst;
    Options.Legacy.Segments.AlignmentPriority =
        X86AlignmentPriority::AfterPaging;
    Options.Legacy.Segments.FullLimitChoice = X86FullSegmentLimitChoice::Wrap;
    Options.Legacy.Scalars = X86LegacyScalarProfile::OrderedRAM;
    Options.Legacy.LogicalAuxiliaryCarry = false;
    Options.Legacy.ControlRegisters.emplace();
    Options.Legacy.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
    Options.Long.FarReturnProfile = X64PagedFarReturnProfile::SelectorFirst;
    Program.clear();
  }

  uint64_t add(uint64_t PC, X86ExecutionMode Mode, std::vector<uint8_t> Bytes) {
    std::copy(Bytes.begin(), Bytes.end(),
              RAM.Memory.front().Bytes.begin() + PC);
    const auto Next = PC + Bytes.size();
    Program.push_back({PC, Mode, std::move(Bytes)});
    return Next;
  }

  void roundtrip() {
    using M = X86ExecutionMode;
    uint64_t PC = 0x1000;
    for (const std::vector<uint8_t> Bytes :
         {std::vector<uint8_t>{0xb9, 0x80, 0, 0, 0xc0},
          {0xb8, 0, 1, 0, 0},
          {0x31, 0xd2},
          {0x0f, 0x30},
          {0xb8, 0x20, 0, 0, 0},
          {0x0f, 0x22, 0xe0},
          {0xb8, 0, 0, 1, 0},
          {0x0f, 0x22, 0xd8},
          {0xb8, 0x31, 0, 1, 0x80},
          {0x0f, 0x22, 0xc0}})
      PC = add(PC, M::Protected32, Bytes);
    add(PC, M::Compatibility32, {0xea, 0, 0x11, 0, 0, 0x18, 0});
    PC = 0x1100;
    for (const std::vector<uint8_t> Bytes :
         {std::vector<uint8_t>{0x48, 0xb8, 0x44, 0x44, 0x33, 0x33, 0x22, 0x22,
                               0x11, 0x11},
          {0x49, 0xb8, 0x55, 0x55, 0x66, 0x66, 0x77, 0x77, 0x88, 0x88},
          {0x48, 0xbf, 0, 0x10, 2, 0, 0, 0, 0, 0},
          {0x48, 0x89, 0x07},
          {0x6a, 8},
          {0x68, 0, 0x12, 0, 0},
          {0x48, 0xcb}})
      PC = add(PC, M::Long64, Bytes);
    PC = add(0x1200, M::Compatibility32, {0x40});
    PC = add(PC, M::Compatibility32, {0xb8, 0x31, 0, 1, 0});
    PC = add(PC, M::Compatibility32, {0x0f, 0x22, 0xc0});
    for (const std::vector<uint8_t> Bytes :
         {std::vector<uint8_t>{0xb8, 0, 0, 0, 0},
          {0xba, 0, 0, 0, 0},
          {0xb9, 0x80, 0, 0, 0xc0},
          {0x0f, 0x30},
          {0x43},
          {0xf4}})
      PC = add(PC, M::Protected32, Bytes);
  }

  void compileLong(bool LLVM, const std::string &Optimization,
                   const std::string &Compiler) {
    llvm::json::Array PCs;
    std::vector<uint8_t> Code;
    for (const auto &I : Program)
      if (I.Mode == X86ExecutionMode::Long64) {
        PCs.push_back(I.Address);
        Code.insert(Code.end(), I.Bytes.begin(), I.Bytes.end());
      }
    llvm::json::Object Settings{
        {"profile", "kernel-events-v6"}, {"mxcsr_mask", "0xffff"},
        {"paging_address_bits", 48},     {"paged_memory", true},
        {"scheduled_events", true},      {"instructions", std::move(PCs)}};
    std::string Request;
    llvm::raw_string_ostream OS(Request);
    OS << llvm::formatv("{0}", llvm::json::Value(std::move(Settings)));
    auto Export = exportX64Continuation(image(Code, 0x1100), Request, LLVM);
    ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
    const auto Manifest = llvm::cantFail(llvm::json::parse(Export->Manifest));
    ASSERT_EQ(Manifest.getAsObject()->getInteger("native_requests"), 0)
        << Export->Manifest;
    const auto *Units = Manifest.getAsObject()->getArray("units");
    ASSERT_NE(Units, nullptr);
    std::vector<std::string> Symbols;
    std::string Adapters;
    for (const auto &I : Program) {
      if (I.Mode != X86ExecutionMode::Long64)
        continue;
      std::string Symbol;
      for (const auto &Unit : *Units) {
        const auto *Object = Unit.getAsObject();
        ASSERT_NE(Object, nullptr);
        const auto PC = Object->getString("pc");
        const auto Name = Object->getString("name");
        ASSERT_TRUE(PC && Name);
        uint64_t Address = 0;
        ASSERT_FALSE(PC->getAsInteger(0, Address));
        if (Address == I.Address)
          Symbol = Name->str();
      }
      ASSERT_FALSE(Symbol.empty());
      Symbols.push_back(Symbol);
      // Call the export's exact-type adapters. Calling generated typed-pointer
      // bodies through a void-pointer function cast would be C undefined
      // behavior.
      Adapters += "\nuint64_t " + Symbol +
                  "_mode_body(void *s, void *e) { return " + Symbol +
                  "_paged_adapter(s, e); }\nuint64_t " + Symbol +
                  "_mode_validate(void *s, void *e) { return " + Symbol +
                  "_validate_paged_adapter(s, e); }\n";
    }
    llvm::sys::DynamicLibrary DL;
    const auto Name =
        std::string("mode-") + (LLVM ? "llvm" : "high") + Optimization +
        (Compiler == NEVERD_TEST_CLANG ? "-clang" : "-neverc") + "-" +
        testing::UnitTest::GetInstance()->current_test_info()->name() + "-" +
        std::to_string(Compilation++);
    ASSERT_NO_FATAL_FAILURE(
        compileProgram(*Export, Name, Compiler, Optimization, DL, Adapters));
    unsigned Index = 0;
    for (auto &I : Program)
      if (I.Mode == X86ExecutionMode::Long64) {
        const auto &Symbol = Symbols[Index++];
        I.Body = reinterpret_cast<X64PagedBody>(
            DL.getAddressOfSymbol((Symbol + "_mode_body").c_str()));
        I.Validate = reinterpret_cast<X64PagedBody>(
            DL.getAddressOfSymbol((Symbol + "_mode_validate").c_str()));
        ASSERT_NE(I.Body, nullptr) << Symbol;
        ASSERT_NE(I.Validate, nullptr) << Symbol;
      }
  }
};

TEST_F(X86ModeExecutionTest,
       ContinuousRoundtripUsesOriginalModesAndSourceBodies) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Completed = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string Optimization : {"-O0", "-O2"}) {
        resetMode();
        roundtrip();
        ASSERT_NO_FATAL_FAILURE(compileLong(LLVM, Optimization, Compiler));
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        const auto &R = Machine.Core.Core.Core.Core.Core.Core;
        unsigned Seen = 0;
        for (unsigned Step = 0; Step != Program.size(); ++Step) {
          X86ModeExecution Execution(*Memory, Options);
          auto Mode = Execution.mode(Machine);
          ASSERT_TRUE(bool(Mode)) << llvm::toString(Mode.takeError());
          Seen |= 1u << unsigned(*Mode);
          uint64_t Budget = 1;
          auto Result = Execution.run(Machine, Program, Budget);
          ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
          ASSERT_TRUE(Result->OwnerError.empty()) << Result->OwnerError;
          ASSERT_FALSE(Result->MissingInstruction)
              << llvm::formatv("PC={0:x}", R.PC).str();
          ASSERT_EQ(Budget, 0u);
          ASSERT_EQ(Result->Attempts, 1u);
          ASSERT_EQ(Result->Completions, 1u);
          if (Machine.Halted) {
            EXPECT_EQ(Step + 1, Program.size());
            break;
          }
        }
        EXPECT_EQ(Seen,
                  (1u << unsigned(X86ExecutionMode::Protected32)) |
                      (1u << unsigned(X86ExecutionMode::Compatibility32)) |
                      (1u << unsigned(X86ExecutionMode::Long64)));
        EXPECT_TRUE(Machine.Halted);
        EXPECT_FALSE(Machine.Pending);
        EXPECT_EQ(Memory->controls().CR0, 0x10031u);
        EXPECT_EQ(Memory->controls().EFER, 0u);
        EXPECT_EQ(R.GPR[3], 0x12345679u);
        EXPECT_EQ(R.GPR[4], 0xf000u);
        EXPECT_EQ(Memory->physicalMemory()
                      .loadScalar(0x21000, GuestScalarWidth::I64)
                      .Value,
                  0x1111222233334444ULL);
        ++Completed;
      }
  RecordProperty("continuous_source_roundtrips", Completed);
}

TEST_F(X86ModeExecutionTest, CommittedControlsSelectTheDebugEventOwner) {
  for (bool Activate : {false, true}) {
    resetMode();
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    Machine.Debug.emplace();
    Options.Legacy.Debug = X86InstructionDebugProfile::InstructionStep;
    Options.Long.DebugProfile = X86InstructionDebugProfile::InstructionStep;
    R.RFlags = 0x102;
    R.GPR[0] = Activate ? 0x80010031 : 0x10031;
    Controls = {Activate ? 0x10031u : 0x80010031u, 0x10000, 0x20,
                Activate ? 0x100u : 0x500u, 0x102};
    if (!Activate)
      put(0x9008, 0x4010 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
    add(0x1000,
        Activate ? X86ExecutionMode::Protected32
                 : X86ExecutionMode::Compatibility32,
        {0x0f, 0x22, 0xc0});
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Got = Execution.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_TRUE(Got->InstructionCompleted);
    EXPECT_EQ(Got->Attempts, 1u);
    EXPECT_EQ(Got->DeliveredVector, 1);
    EXPECT_EQ(R.PC, 0x4010u);
    EXPECT_EQ(Machine.Core.Core.Core.CS.Selector, Activate ? 24 : 8);
    EXPECT_EQ(Memory->controls().EFER, Activate ? 0x500u : 0x100u);
    EXPECT_TRUE(Machine.Debug->DR6 & 0x4000);
    EXPECT_FALSE(Machine.Pending);
  }
}

TEST_F(X86ModeExecutionTest, IllegalControlWriteFaultsInTheOriginalMode) {
  for (bool Active : {false, true}) {
    resetMode();
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = Active ? 0 : 0x80010031;
    Controls = {Active ? 0x80010031u : 0x10031u, 0x10000, Active ? 0x20u : 0u,
                Active ? 0x500u : 0x100u, 2};
    if (!Active)
      put(0x9000 + 13 * 8,
          0x40d0 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
    add(0x1000,
        Active ? X86ExecutionMode::Compatibility32
               : X86ExecutionMode::Protected32,
        {0x0f, 0x22, uint8_t(Active ? 0xe0 : 0xc0)});
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Got = Execution.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_FALSE(Got->InstructionCompleted);
    EXPECT_EQ(Got->DeliveredVector, 13);
    EXPECT_EQ(R.PC, 0x40d0u);
    EXPECT_EQ(Machine.Core.Core.Core.CS.Selector, Active ? 24 : 8);
    EXPECT_EQ(Memory->controls().CR0, Controls.CR0);
    EXPECT_EQ(Memory->controls().CR4, Controls.CR4);
    EXPECT_EQ(Memory->controls().EFER, Controls.EFER);
  }
}

TEST_F(X86ModeExecutionTest, FirstCompatibilityFetchUsesTheNewPageTables) {
  resetMode();
  auto &R = Machine.Core.Core.Core.Core.Core.Core;
  R.PC = 0x1ffd;
  R.GPR[0] = 0x80010031;
  Controls = {0x10031, 0x10000, 0x20, 0x100, 2};
  put(0x12000, 0x13003);
  for (unsigned I = 0; I != 64; ++I)
    put(0x13000 + I * 8, I == 2 ? 0 : I * 4096 | 3);
  add(0x1ffd, X86ExecutionMode::Protected32, {0x0f, 0x22, 0xc0});
  add(0x2000, X86ExecutionMode::Compatibility32, {0x90});
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X86ModeExecution Execution(*Memory, Options);
  uint64_t Budget = 2;
  auto Got = Execution.run(Machine, Program, Budget);
  ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
  ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(Got->Attempts, 2u);
  EXPECT_EQ(Got->Completions, 1u);
  EXPECT_EQ(Got->DeliveredVector, 14);
  EXPECT_EQ(R.PC, 0x40e0u);
  EXPECT_EQ(Machine.Core.Core.Core.Core.CR2, 0x2000u);
  EXPECT_EQ(Memory->controls().EFER, 0x500u);
  EXPECT_EQ(Machine.Core.Core.Core.CS.Selector, 24);
}

TEST_F(X86ModeExecutionTest, WideDescriptorFaultKeepsTheCompleteCR2) {
  resetMode();
  Machine.Core.Core.GDTR.Base = 0x100008000ULL;
  Controls = {0x80010031, 0x10000, 0x20, 0x500, 2};
  add(0x1000, X86ExecutionMode::Compatibility32,
      {0xea, 0, 0x11, 0, 0, 0x18, 0});
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X86ModeExecution Execution(*Memory, Options);
  uint64_t Budget = 1;
  auto Got = Execution.run(Machine, Program, Budget);
  ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
  ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::Shutdown);
  EXPECT_EQ(Machine.Core.Core.Core.Core.CR2, 0x100008018ULL);
  EXPECT_EQ(Machine.Core.Core.GDTR.Base, 0x100008000ULL);
  EXPECT_FALSE(Got->InstructionCompleted);
}

TEST_F(X86ModeExecutionTest, FarResumeKeepsFullBankAndDescriptorInput) {
  resetMode();
  auto &V = Machine.Core.Core;
  auto &R = V.Core.Core.Core.Core;
  V.GDTR.Base = 0xffff800000008000ULL;
  V.Core.Core.CR2 = 0xabcdef0012345678ULL;
  R.FSBase = 0xffffdead12340000ULL;
  R.GSBase = 0xffffbeef56780000ULL;
  R.GPR[8] = 0x1111222233334444ULL;
  Machine.Core.SysenterESP = 0xffff800012340000ULL;
  for (unsigned I = 0; I != 8; ++I)
    R.Undefined[I] = (uint64_t(0xabc00000 + I) << 32);
  Controls = {0x80010031, 0x10000, 0x20, 0x500, 2};
  put(0x10800, 0x11003);
  // Code64 ignores the descriptor's small limit and nonzero base. Accessed
  // starts clear, so the descriptor read must survive a write-backing stop.
  put(0x8018, 0x00209a1200000001ULL);
  add(0x1000, X86ExecutionMode::Compatibility32,
      {0xea, 0, 0x11, 0, 0, 0x18, 0});
  const auto Bytes = RAM.Memory.front().Bytes;
  RAM.Memory = {
      {0, MemoryPermission::Read | MemoryPermission::Write, 0,
       std::vector<uint8_t>(Bytes.begin(), Bytes.begin() + 0x8018)},
      {0x8018, MemoryPermission::Read, 0,
       std::vector<uint8_t>(Bytes.begin() + 0x8018, Bytes.begin() + 0x8020)},
      {0x8020, MemoryPermission::Read | MemoryPermission::Write, 0,
       std::vector<uint8_t>(Bytes.begin() + 0x8020, Bytes.end())}};
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  uint64_t Budget = 1;
  X86ModeExecution Execution(*Memory, Options);
  auto First = Execution.run(Machine, Program, Budget);
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  ASSERT_EQ(First->Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(Machine.Pending);
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(First->Attempts, 1u);
  EXPECT_EQ(R.PC, 0x1000u);
  auto Again = Execution.run(Machine, {}, Budget);
  ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
  EXPECT_EQ(Again->Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_EQ(Again->Attempts, 0u);
  auto Changed = Machine;
  Changed.Core.Core.Core.Core.Core.Core.GPR[15] = 1;
  auto Refused = Execution.run(Changed, {}, Budget);
  ASSERT_FALSE(bool(Refused));
  llvm::consumeError(Refused.takeError());
  auto Snapshot = RAM;
  Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
  Snapshot.Memory[1].Permissions =
      MemoryPermission::Read | MemoryPermission::Write;
  auto Repaired = llvm::cantFail(
      X86PagedMemory::create(Snapshot, Features, Memory->controls()));
  auto ChangedOptions = Options;
  ChangedOptions.Legacy.Segments.FullLimitChoice.reset();
  ChangedOptions.Undefined.reset();
  X86ModeExecution Resumed(*Repaired, ChangedOptions);
  auto Done = Resumed.run(Machine, {}, Budget);
  ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
  ASSERT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
  EXPECT_TRUE(Done->BudgetExhausted);
  EXPECT_EQ(Done->Attempts, 0u);
  EXPECT_EQ(Done->Completions, 1u);
  EXPECT_FALSE(Machine.Pending);
  EXPECT_EQ(R.PC, 0x1100u);
  EXPECT_EQ(V.Core.CS.Selector, 24);
  EXPECT_EQ(V.Core.CS.Limit, 1u);
  EXPECT_EQ(V.Core.CS.Base, 0x120000u);
  EXPECT_EQ(V.GDTR.Base, 0xffff800000008000ULL);
  EXPECT_EQ(V.Core.Core.CR2, 0xabcdef0012345678ULL);
  EXPECT_EQ(R.FSBase, 0xffffdead12340000ULL);
  EXPECT_EQ(R.GSBase, 0xffffbeef56780000ULL);
  EXPECT_EQ(R.GPR[8], 0x1111222233334444ULL);
  EXPECT_EQ(Machine.Core.SysenterESP, 0xffff800012340000ULL);
  for (unsigned I = 0; I != 8; ++I)
    EXPECT_EQ(R.GPR[I] & ~uint64_t(UINT32_MAX), R.Undefined[I]);
}

TEST_F(X86ModeExecutionTest, InvalidFullMetadataStopsBeforeAnyEffects) {
  for (unsigned Case = 0; Case != 12; ++Case) {
    SCOPED_TRACE(Case);
    resetMode();
    auto &V = Machine.Core.Core;
    switch (Case) {
    case 0:
      V.NMIBlocked = 2;
      break;
    case 1:
      V.Core.Core.Core.CR8 = 16;
      break;
    case 2:
      V.Core.Core.Core.Reserved = 1;
      break;
    case 3:
      V.Core.Core.Core.ReservedTail = 1;
      break;
    case 4:
      V.Core.Core.Reserved = 1;
      break;
    case 5:
      V.Core.Reserved = 1;
      break;
    case 6:
      V.GDTR.Reserved = 1;
      break;
    case 7:
      V.IDTR.ReservedTail = 1;
      break;
    case 8:
      V.Reserved[13] = 1;
      break;
    case 9:
      Machine.Core.Reserved = 1;
      break;
    case 10:
      Machine.Core.SysenterCS = uint64_t(1) << 32;
      break;
    case 11:
      Options.Legacy.Segments.FullLimitChoice.reset();
      break;
    }
    V.Core.SS.Base = 0x4000;
    V.Core.Core.Core.Core.GPR[4] = 5;
    add(0x1000, X86ExecutionMode::Protected32, {0x60});
    const auto Before = Machine;
    const auto Bytes = RAM.Memory.front().Bytes;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Got = Execution.run(Machine, Program, Budget);
    ASSERT_FALSE(bool(Got));
    llvm::consumeError(Got.takeError());
    EXPECT_EQ(Budget, 1u);
    EXPECT_EQ(Machine.Core.Core, Before.Core.Core);
    EXPECT_EQ(Machine.Core.SysenterCS, Before.Core.SysenterCS);
    EXPECT_EQ(Machine.Core.Reserved, Before.Core.Reserved);
    EXPECT_FALSE(Machine.Pending);
    EXPECT_EQ(Memory->controls(), Controls);
    EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
              Bytes);
  }
}

TEST_F(X86ModeExecutionTest, CR2PublicationUsesTheWriteReceipt) {
  for (bool Write : {false, true}) {
    resetMode();
    Machine.Core.Core.Core.Core.CR2 = 0xabcdef0012345678ULL;
    Machine.Core.Core.Core.Core.Core.Core.GPR[0] = 0x12345678;
    add(0x1000, X86ExecutionMode::Protected32,
        Write ? std::vector<uint8_t>{0x0f, 0x22, 0xd0}
              : std::vector<uint8_t>{0x90});
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Got = Execution.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    EXPECT_EQ(Got->Completions, 1u);
    EXPECT_EQ(Machine.Core.Core.Core.Core.CR2,
              Write ? 0x12345678ULL : 0xabcdef0012345678ULL);
  }
}

TEST_F(X86ModeExecutionTest, EFERUsesBoundCapabilitiesAndReadOnlyActiveState) {
  for (unsigned Case = 0; Case != 11; ++Case) {
    SCOPED_TRACE(Case);
    resetMode();
    Features.NX = Case != 6;
    Features.LongMode = Case != 10;
    const bool Active = Case == 4 || Case == 5;
    if (Active)
      Controls = {0x80010031, 0x10000, 0x20, 0x500, 2};
    else
      for (unsigned Vector : {6u, 13u})
        put(0x9000 + Vector * 8, (0x4000 + Vector * 16) | (uint64_t(8) << 16) |
                                     (uint64_t(0x8e) << 40));
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    const uint64_t Value = Case == 0    ? 0x400
                           : Case == 1  ? 0x101
                           : Case == 2  ? 0x10
                           : Case == 3  ? uint64_t(1) << 32
                           : Case == 6  ? 0x800
                           : Case == 9  ? 1
                           : Case == 10 ? 0x100
                                        : 0;
    R.GPR[0] = uint32_t(Value);
    R.GPR[2] = Value >> 32;
    R.GPR[1] = Case == 8 ? 0xdeadbeef : 0xc0000080;
    Options.EFERSystemCall = Case == 1;
    std::vector<uint8_t> Bytes{0x0f, uint8_t(Case == 5 ? 0x32 : 0x30)};
    if (Case == 7)
      Bytes.insert(Bytes.begin(), 0xf0);
    add(0x1000,
        Active ? X86ExecutionMode::Compatibility32
               : X86ExecutionMode::Protected32,
        Bytes);
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Got = Execution.run(Machine, Program, Budget);
    if (Case == 8) {
      ASSERT_FALSE(bool(Got));
      llvm::consumeError(Got.takeError());
      EXPECT_EQ(Budget, 1u);
      EXPECT_EQ(R.PC, 0x1000u);
      EXPECT_EQ(Memory->controls(), Controls);
      EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
                RAM.Memory.front().Bytes);
      continue;
    }
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    EXPECT_EQ(Budget, 0u);
    const bool Fault = Case != 0 && Case != 1 && Case != 5;
    EXPECT_EQ(Got->Completions, unsigned(!Fault));
    if (Fault) {
      EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_EQ(Got->DeliveredVector, Case == 7 ? 6 : 13);
      EXPECT_EQ(Memory->controls().EFER, Controls.EFER);
    } else {
      EXPECT_TRUE(Got->BudgetExhausted);
      EXPECT_EQ(Memory->controls().EFER, Case == 1 ? 0x101 : Controls.EFER);
      if (Case == 5) {
        EXPECT_EQ(R.GPR[0], 0x500u);
        EXPECT_EQ(R.GPR[2], 0u);
      }
    }
  }
}

TEST_F(X86ModeExecutionTest, LegacyEventDoesNotAliasAHighDebugAddress) {
  for (bool High : {false, true}) {
    resetMode();
    Machine.Debug.emplace();
    Machine.Debug->DR[0] = (High ? uint64_t(1) << 32 : 0) | 0xeffc;
    Machine.Debug->DR7 = 0xd2401;
    Options.Legacy.Debug = X86InstructionDebugProfile::Watchpoints;
    Options.Long.DebugProfile = X86InstructionDebugProfile::Watchpoints;
    put(0x9008, 0x4010 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
    put(0x9200, 0x4400 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
    add(0x1000, X86ExecutionMode::Protected32, {0xcd, 0x40});
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Got = Execution.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    EXPECT_EQ(Got->Completions, 1u);
    EXPECT_EQ(Got->DeliveredVector, High ? 0x40 : 1);
    EXPECT_EQ(Machine.Core.Core.Core.Core.Core.Core.PC,
              High ? 0x4400u : 0x4010u);
    EXPECT_EQ(bool(Machine.Debug->DR7 & 0x2000), High);
    EXPECT_EQ(bool(Machine.Debug->DR6 & 1), !High);
    EXPECT_EQ(Machine.Debug->DR[0], (High ? uint64_t(1) << 32 : 0) | 0xeffc);
  }
}

TEST_F(X86ModeExecutionTest,
       SoftwareCompletionIsReportedOnceWhileDebugResumes) {
  resetMode();
  Controls = {0x80010031, 0x10000, 0x20, 0x500, 2};
  Machine.Debug.emplace();
  Machine.Debug->DR[0] = 0xeff8;
  Machine.Debug->DR7 = 0x90401;
  Options.Legacy.Debug = X86InstructionDebugProfile::Watchpoints;
  Options.Long.DebugProfile = X86InstructionDebugProfile::Watchpoints;
  add(0x1000, X86ExecutionMode::Compatibility32, {0xcd, 0x40});
  const auto Bytes = RAM.Memory.front().Bytes;
  RAM.Memory = {{0, MemoryPermission::Read | MemoryPermission::Write, 0,
                 std::vector<uint8_t>(Bytes.begin(), Bytes.begin() + 0x9010)},
                {0x9020, MemoryPermission::Read | MemoryPermission::Write, 0,
                 std::vector<uint8_t>(Bytes.begin() + 0x9020, Bytes.end())}};
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X86ModeExecution Execution(*Memory, Options);
  uint64_t Budget = 1;
  auto First = Execution.run(Machine, Program, Budget);
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  ASSERT_EQ(First->Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(Machine.Pending);
  EXPECT_EQ(First->Attempts, 1u);
  EXPECT_EQ(First->Completions, 1u);
  EXPECT_TRUE(First->EventAccepted);
  EXPECT_EQ(Budget, 0u);
  for (unsigned Retry = 0; Retry != 2; ++Retry) {
    X86ModeExecution Rebuilt(*Memory, Options);
    auto Again = Rebuilt.run(Machine, {}, Budget);
    ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
    EXPECT_EQ(Again->Status, X86LegacyExecutionStatus::BackingFault);
    EXPECT_EQ(Again->Attempts, 0u);
    EXPECT_EQ(Again->Completions, 0u);
    EXPECT_FALSE(Again->EventAccepted);
  }
  auto Snapshot = RAM;
  Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
  Snapshot.Memory.push_back(
      {0x9010, MemoryPermission::Read | MemoryPermission::Write, 0,
       std::vector<uint8_t>(Bytes.begin() + 0x9010, Bytes.begin() + 0x9020)});
  auto Repaired = llvm::cantFail(
      X86PagedMemory::create(Snapshot, Features, Memory->controls()));
  X86ModeExecution Resumed(*Repaired, Options);
  auto Done = Resumed.run(Machine, {}, Budget);
  ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
  EXPECT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
  EXPECT_EQ(Done->Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(Done->DeliveredVector, 1);
  EXPECT_EQ(Done->Attempts, 0u);
  EXPECT_EQ(Done->Completions, 0u);
  EXPECT_EQ(Machine.Core.Core.Core.Core.Core.Core.PC, 0x4010u);
  EXPECT_FALSE(Machine.Pending);
}

TEST_F(X86ModeExecutionTest, RetiredFarOwnerErrorRetiresTheOuterContinuation) {
  resetMode();
  Controls = {0x80010031, 0x10000, 0x20, 0x500, 2};
  // A present call gate is valid architectural data but outside this owner's
  // direct-code-transfer domain. It is first revealed after backing repair.
  put(0x8018, 0x00008c0000081100ULL);
  add(0x1000, X86ExecutionMode::Compatibility32,
      {0xea, 0, 0x11, 0, 0, 0x18, 0});
  const auto Bytes = RAM.Memory.front().Bytes;
  RAM.Memory = {{0, MemoryPermission::Read | MemoryPermission::Write, 0,
                 std::vector<uint8_t>(Bytes.begin(), Bytes.begin() + 0x8018)},
                {0x8020, MemoryPermission::Read | MemoryPermission::Write, 0,
                 std::vector<uint8_t>(Bytes.begin() + 0x8020, Bytes.end())}};
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X86ModeExecution Execution(*Memory, Options);
  uint64_t Budget = 1;
  auto First = Execution.run(Machine, Program, Budget);
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  ASSERT_EQ(First->Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(Machine.Pending);
  EXPECT_EQ(Budget, 0u);
  auto Snapshot = RAM;
  Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
  Snapshot.Memory.push_back(
      {0x8018, MemoryPermission::Read | MemoryPermission::Write, 0,
       std::vector<uint8_t>(Bytes.begin() + 0x8018, Bytes.begin() + 0x8020)});
  auto Repaired = llvm::cantFail(
      X86PagedMemory::create(Snapshot, Features, Memory->controls()));
  X86ModeExecution Resumed(*Repaired, Options);
  auto Done = Resumed.run(Machine, {}, Budget);
  ASSERT_FALSE(bool(Done));
  llvm::consumeError(Done.takeError());
  EXPECT_FALSE(Machine.Pending);
  EXPECT_EQ(Machine.Core.Core.Core.Core.Core.Core.PC, 0x1000u);
  auto Again = Resumed.run(Machine, Program, Budget);
  ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
  EXPECT_TRUE(Again->BudgetExhausted);
  EXPECT_EQ(Again->Attempts, 0u);
}

TEST_F(X86ModeExecutionTest, LongBodiesOwnShadowsAndIgnoreLegacyCodeBounds) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    resetMode();
    Options.Long.SegmentMoveProfile = X64PagedSegmentMoveProfile::SelectorFirst;
    Controls = {0x80010031, 0x10000, 0x20, 0x500, 2};
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    R.PC = 0x1100;
    R.GPR[0] = 16;
    V.Core.CS = {0x12340000, 1, 24, 0xa09b};
    uint64_t PC = add(0x1100, X86ExecutionMode::Long64, {0xfb});
    PC = add(PC, X86ExecutionMode::Long64, {0x90});
    PC = add(PC, X86ExecutionMode::Long64, {0x8e, 0xd0});
    add(PC, X86ExecutionMode::Long64, {0x90});
    ASSERT_NO_FATAL_FAILURE(compileLong(LLVM, "-O2", NEVERD_TEST_CLANG));
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    for (unsigned Step = 0; Step != Program.size(); ++Step) {
      X86ModeExecution Rebuilt(*Memory, Options);
      uint64_t Budget = 1;
      auto Got = Rebuilt.run(Machine, Program, Budget);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
      EXPECT_EQ(Got->Attempts, 1u);
      EXPECT_EQ(Got->Completions, 1u);
      EXPECT_EQ(Machine.STIBlocked, Step == 0);
      EXPECT_EQ(Machine.SSBlocked, Step == 2);
      EXPECT_EQ(V.Core.CS.Base, 0x12340000u);
      EXPECT_EQ(V.Core.CS.Limit, 1u);
    }
    EXPECT_EQ(R.PC, 0x1105u);
    EXPECT_TRUE(R.RFlags & 0x200);
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X86ModeExecutionTest, OriginalModeWritesChooseTheNativeEventFrame) {
  unsigned Compared = 0;
  for (unsigned Case = 0; Case != 4; ++Case) {
    SCOPED_TRACE(Case);
    X64PagedNativeCPU Native;
    ASSERT_NO_FATAL_FAILURE(Native.initialize());
    if (IsSkipped())
      return;
    ASSERT_NO_FATAL_FAILURE(Native.pinForReturnComparison());
    resetMode();
    roundtrip();
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    uint64_t PC = Program[9].Address;
    Program.resize(9);
    const uint8_t Vector = Case == 0   ? 0x40
                           : Case == 1 ? 6
                           : Case == 2 ? 13
                                       : 1;
    if (Case == 2) {
      Program[4].Bytes[1] = 0; // Original MOV EAX,0 disables PAE before PG.
      std::copy(Program[4].Bytes.begin(), Program[4].Bytes.end(),
                RAM.Memory.front().Bytes.begin() + Program[4].Address);
      put(0x9000 + Vector * 8, (0x4000 + Vector * 16) | (uint64_t(8) << 16) |
                                   (uint64_t(0x8e) << 40));
    }
    if (Case == 3) {
      Machine.Debug.emplace();
      Options.Legacy.Debug = X86InstructionDebugProfile::InstructionStep;
      Options.Long.DebugProfile = X86InstructionDebugProfile::InstructionStep;
      PC = add(PC, X86ExecutionMode::Protected32, {0x68, 2, 1, 0, 0});
      PC = add(PC, X86ExecutionMode::Protected32, {0x9d});
    }
    PC = add(PC, X86ExecutionMode::Protected32, {0x0f, 0x22, 0xc0});
    if (Case < 2)
      add(PC, X86ExecutionMode::Compatibility32,
          Case == 0 ? std::vector<uint8_t>{0xcd, Vector}
                    : std::vector<uint8_t>{0x0f, 0x0b});
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
    // The original stream performs every mode write and selects its own
    // protected/IA-32e IDT format. No SET_SREGS occurs after this initial
    // setup.
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_SREGS, &S), 0) << errno;
    kvm_regs Registers{};
    Registers.rip = R.PC;
    Registers.rflags = R.RFlags;
    Registers.rbx = R.GPR[3];
    Registers.rsp = R.GPR[4];
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &Registers), 0) << errno;
    ASSERT_NO_FATAL_FAILURE(Native.run());
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Registers), 0) << errno;
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &S), 0) << errno;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = Program.size();
    auto Got = Execution.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_EQ(Got->DeliveredVector, Vector);
    EXPECT_EQ(Got->Attempts, Program.size());
    EXPECT_EQ(Got->Completions, Program.size() - (Case == 1 || Case == 2));
    EXPECT_EQ(Budget, 0u);
    // The native terminal HLT advances PC by one; it changes neither the
    // installed frame nor the registers/flags compared here.
    EXPECT_EQ(R.PC + 1, Registers.rip);
    EXPECT_EQ(R.RFlags, Registers.rflags);
    EXPECT_EQ(uint32_t(R.GPR[0]), uint32_t(Registers.rax));
    EXPECT_EQ(R.GPR[4], Registers.rsp);
    EXPECT_EQ(Memory->controls().CR0, S.cr0);
    EXPECT_EQ(Memory->controls().CR4, S.cr4);
    EXPECT_EQ(Memory->controls().EFER, S.efer);
    EXPECT_EQ(V.Core.CS.Selector, S.cs.selector);
    EXPECT_EQ(V.Core.SS.Selector, S.ss.selector);
    EXPECT_EQ(V.Core.Core.CR2, S.cr2);
    const auto FinalRAM = Memory->physicalMemory().snapshotMemoryRegions();
    ASSERT_EQ(FinalRAM.size(), 1u);
    EXPECT_EQ(std::memcmp(FinalRAM.front().Bytes.data(), Native.RAM,
                          X64PagedNativeCPU::Size),
              0);
    ++Compared;
  }
  RecordProperty("continuous_native_event_comparisons", Compared);
  RecordProperty("original_initial_sregs_writes", Compared);
  RecordProperty("original_mode_rewrites", 0);
}

TEST_F(X86ModeExecutionTest, OneNativeCPUExecutesTheEntireRoundtrip) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU Native;
  ASSERT_NO_FATAL_FAILURE(Native.initialize());
  if (IsSkipped())
    return;
  ASSERT_NO_FATAL_FAILURE(Native.pinForReturnComparison());
  resetMode();
  roundtrip();
  ASSERT_NO_FATAL_FAILURE(compileLong(false, "-O2", NEVERD_TEST_CLANG));
  auto &V = Machine.Core.Core;
  auto &R = V.Core.Core.Core.Core;
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
  // Exactly one initial SREGS write. Every later mode, control and segment
  // result comes from the original instructions, without host stage rewrites.
  ASSERT_EQ(ioctl(Native.CPU, KVM_SET_SREGS, &S), 0) << errno;
  kvm_regs Registers{};
  Registers.rip = R.PC;
  Registers.rflags = R.RFlags;
  Registers.rbx = R.GPR[3];
  Registers.rsp = R.GPR[4];
  ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &Registers), 0) << errno;
  ASSERT_NO_FATAL_FAILURE(Native.run());
  ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
  ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Registers), 0) << errno;
  ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &S), 0) << errno;
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X86ModeExecution Execution(*Memory, Options);
  uint64_t Budget = Program.size();
  auto Got = Execution.run(Machine, Program, Budget);
  ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
  ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
  EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::Halted);
  EXPECT_EQ(Got->Attempts, Program.size());
  EXPECT_EQ(Got->Completions, Program.size());
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(R.PC, Registers.rip);
  EXPECT_EQ(R.RFlags, Registers.rflags);
  const std::array<uint64_t, 8> NativeGPR{
      Registers.rax, Registers.rcx, Registers.rdx, Registers.rbx,
      Registers.rsp, Registers.rbp, Registers.rsi, Registers.rdi};
  for (unsigned I = 0; I != 8; ++I)
    EXPECT_EQ(uint32_t(R.GPR[I]), uint32_t(NativeGPR[I]));
  EXPECT_EQ(Memory->controls().CR0, S.cr0);
  EXPECT_EQ(Memory->controls().CR3, S.cr3);
  EXPECT_EQ(Memory->controls().CR4, S.cr4);
  EXPECT_EQ(Memory->controls().EFER, S.efer);
  EXPECT_EQ(V.Core.CS.Selector, S.cs.selector);
  EXPECT_EQ(V.Core.CS.Base, S.cs.base);
  EXPECT_EQ(V.Core.CS.Limit, S.cs.limit);
  EXPECT_EQ(V.GDTR.Base, S.gdt.base);
  EXPECT_EQ(V.IDTR.Base, S.idt.base);
  const auto FinalRAM = Memory->physicalMemory().snapshotMemoryRegions();
  ASSERT_EQ(FinalRAM.size(), 1u);
  EXPECT_EQ(std::memcmp(FinalRAM.front().Bytes.data(), Native.RAM,
                        X64PagedNativeCPU::Size),
            0);
  RecordProperty("continuous_original_cpu_comparisons", 1);
  RecordProperty("original_initial_sregs_writes", 1);
  RecordProperty("original_mode_rewrites", 0);
}
#endif
