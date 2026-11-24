//===- ReturningCallEnvironmentTests.inc - Conditional call regions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independently authored native callers for the production relation.
// Included after MachineExitEnvironmentTests.inc in the fixed-PE namespace.

SpecializationOptions returningCallOptions(va_t Call, va_t Exit) {
  auto O = machineExitOptions(Exit, MachineExitKind::Return);
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-128, 128};
  O.X64ReturningCalls = {
      {1, Call, SpecializationX64CallProfile::ConditionalNormalReturnV1, 32}};
  return O;
}

TEST_F(PEFixedImageTest, ReturningCallCanRetainDeclaredEarlierFrameBytes) {
  // Save the real return stack pointer in a VM frame slot below the later
  // outbound call, then restore it after the imported call returns.
  const std::vector<uint8_t> Code = {
      0x48, 0x89, 0x64, 0x24, 0x90, // mov [rsp-112],rsp
      0x48, 0x83, 0xec, 0x40,       // sub rsp,64
      0xff, 0xd0,                   // call rax
      0x48, 0x83, 0xc4, 0x40,       // add rsp,64
      0x48, 0x8b, 0x64, 0x24, 0x90, // mov rsp,[rsp-112]
      0xff, 0xd0,                   // call rax through the restored stack
      0xc3};                        // ret
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x1009, Base + 0x1016);
  O.X64ReturningCalls.push_back(
      {1, Base + 0x1014,
       SpecializationX64CallProfile::ConditionalNormalReturnV1, 32});
  const auto Unpreserved =
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  EXPECT_FALSE(Unpreserved.complete());
  EXPECT_TRUE(Unpreserved.Residual.Blocks.empty());
  auto &Call = O.X64ReturningCalls.front();
  Call.Version = 2;
  Call.Profile =
      SpecializationX64CallProfile::ConditionalNormalReturnFrameSpansV2;
  Call.PreservedFrameRanges = {{-112, -104}};
  const auto Preserved = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Preserved.complete()) << Preserved.Diagnostic;
  ASSERT_EQ(Preserved.ReturningCalls.size(), 2U);
  ASSERT_TRUE(Preserved.RawEnvironment);
  auto Error = validateSpecializationRawEnvironment(Preserved, O);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto Stale = O;
  Stale.X64ReturningCalls.front().PreservedFrameRanges.front().End = -105;
  auto Mismatch = validateSpecializationRawEnvironment(Preserved, Stale);
  EXPECT_TRUE(bool(Mismatch));
  llvm::consumeError(std::move(Mismatch));
  auto Overlap = O;
  Overlap.X64ReturningCalls.front().PreservedFrameRanges = {{-72, -64}};
  const auto Refused =
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, Overlap);
  EXPECT_FALSE(Refused.complete());
  EXPECT_TRUE(Refused.Residual.Blocks.empty());
}

llvm::ArrayRef<LowOp> returningCallSpan(const SpecializationResult &R,
                                        va_t Address) {
  for (const auto &B : R.Residual.Blocks)
    for (const auto &I : B.InstructionBoundaries)
      if (I.Address == Address)
        return llvm::ArrayRef<LowOp>(B.Ops).slice(I.FirstOp, I.OpCount);
  return {};
}

TEST_F(PEFixedImageTest, ReturningCallRetainsTargetBeforePushAndGuestReturn) {
  const std::vector<std::vector<uint8_t>> Calls = {{0xff, 0x54, 0x24, 0xf8},
                                                   {0xff, 0x14, 0x24},
                                                   {0xff, 0xd4},
                                                   {0xff, 0xd0},
                                                   {0xe8, 0x40, 0, 0, 0}};
  for (size_t Kind = 0; Kind < Calls.size(); ++Kind) {
    auto Code = Calls[Kind];
    Code.push_back(0xc3);
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    const va_t Return = Base + 0x1000 + Calls[Kind].size();
    const auto O = returningCallOptions(Base + 0x1000, Return);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_EQ(R.ReturningCalls.size(), 1U);
    ASSERT_EQ(R.ExternalExits.size(), 1U);
    auto Valid = validateSpecializationRawEnvironment(R, O);
    ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    const auto Span = returningCallSpan(R, R.ReturningCalls[0].ResidualAddress);
    ASSERT_FALSE(Span.empty());
    ASSERT_TRUE(lowRawReturningCallShapeIsValid(Span.back()));
    // Independent byte oracle executes the retained physical prefix. It never
    // invokes symbolic forgetting, a production adapter, or a generic CALL.
    for (uint64_t Seed : {uint64_t{0x12340}, uint64_t{0xffff800012345678}}) {
      constexpr uint64_t Stack = 0x800000;
      auto S = machineExitState(Stack);
      S.write(NdVar::reg(x86reg::RAX, 8), Seed);
      S.memory(Stack - 8, Seed, 8);
      S.memory(Stack, Seed, 8);
      unsigned Loads = 0, Stores = 0;
      for (const auto &Op : Span.drop_back()) {
        switch (Op.Opcode) {
        case NdOp::COPY:
          S.write(Op.Output, S.read(Op.Inputs[0]));
          break;
        case NdOp::INT_ADD:
          S.write(Op.Output, S.read(Op.Inputs[0]) + S.read(Op.Inputs[1]));
          break;
        case NdOp::INT_SUB:
          S.write(Op.Output, S.read(Op.Inputs[0]) - S.read(Op.Inputs[1]));
          break;
        case NdOp::LOAD:
          ++Loads;
          S.write(Op.Output,
                  machineExitWord(S, S.read(*lowMemoryOperands(Op).Address)));
          break;
        case NdOp::STORE: {
          ++Stores;
          const auto M = lowMemoryOperands(Op);
          S.memory(S.read(*M.Address), S.read(*M.StoredValue), M.AccessSize);
          break;
        }
        default:
          FAIL() << "unexpected physical prefix operation "
                 << ndOpName(Op.Opcode);
        }
      }
      ASSERT_TRUE(S.Valid);
      EXPECT_EQ(Loads, Kind < 2 ? 1U : 0U);
      EXPECT_EQ(Stores, 1U);
      EXPECT_EQ(S.read(NdVar::reg(x86reg::RSP, 8)), Stack - 8);
      EXPECT_EQ(machineExitWord(S, Stack - 8), Return);
      EXPECT_EQ(S.read(Span.back().Inputs[5]), Kind == 2   ? Stack
                                               : Kind == 4 ? Return + 64
                                                           : Seed);
    }
  }
}

TEST_F(PEFixedImageTest, ReturningCallRequiresExplicitMachineRegionEvenUnused) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0, 0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto Good = returningCallOptions(Base + 0x1000, Base + 0x1002);
  ASSERT_TRUE(
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, Good).complete());
  for (bool Unused : {false, true})
    for (unsigned Change = 0; Change < 14; ++Change) {
      SCOPED_TRACE(Change);
      auto O = Good;
      if (Unused)
        O.X64ReturningCalls[0].Address += 0x100;
      switch (Change) {
      case 0:
        O.X64ReturningCalls[0].Version = 0;
        break;
      case 1:
        O.X64ReturningCalls[0].Profile = SpecializationX64CallProfile::None;
        break;
      case 2:
        O.X64ReturningCalls[0].OutgoingBytes = 0;
        break;
      case 3:
        O.X64ReturningCalls[0].OutgoingBytes = 24;
        break;
      case 4:
        O.X64ReturningCalls[0].OutgoingBytes = 33;
        break;
      case 5:
        O.X64ReturningCalls[0].OutgoingBytes = UINT64_MAX - 7;
        break;
      case 6:
        O.X64MachineExits.clear();
        O.RequireRestoredFrameAtReturn = true;
        break;
      case 7:
        O.RequireRestoredFrameAtReturn = true;
        break;
      case 8:
        O.ExternalStoresPreserveEntryReturnSlot = true;
        break;
      case 9:
        O.X64TerminalImports.push_back(
            {1, Base + 0x3080, "example.dll", "StopFixture"});
        break;
      case 10:
        O.EntryFrameBounds.reset();
        break;
      case 11:
        O.X64ReturningCalls.push_back(O.X64ReturningCalls[0]);
        break;
      case 12:
        O.X64MachineExits[0].Address = O.X64ReturningCalls[0].Address;
        break;
      case 13:
        O.Architecture = Arch::AArch64;
        break;
      }
      const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
      EXPECT_FALSE(R.complete());
      EXPECT_TRUE(R.Residual.Blocks.empty());
      EXPECT_TRUE(R.ReturningCalls.empty());
    }
}

TEST_F(PEFixedImageTest, ReturningCallKeepsOnlyCurrentCompleteSuffixFacts) {
  for (unsigned Variant = 0; Variant < 4; ++Variant) {
    // mov qword [rsp+offset],7; [overwrite byte]; call rax;
    // cmp qword [rsp+offset],7; je good; ud2; good: ret.
    const uint8_t Offset = Variant == 1 ? 16 : 40;
    std::vector<uint8_t> Code{0x48, 0xc7, 0x44, 0x24, Offset, 7, 0, 0, 0};
    if (Variant == 2)
      Code.insert(Code.end(), {0xc6, 0x44, 0x24, Offset, 9});
    if (Variant == 3)
      Code.erase(
          Code.begin()); // only four initialized bytes; upper half is a hole
    const va_t Call = Base + 0x1000 + Code.size();
    Code.insert(Code.end(), {0xff, 0xd0, 0x48, 0x83, 0x7c, 0x24, Offset, 7,
                             0x74, 2, 0x0f, 0x0b, 0xc3});
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    const auto O = returningCallOptions(Call, Base + 0x1000 + Code.size() - 1);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_EQ(R.complete(), Variant == 0) << R.Diagnostic;
    if (Variant != 0) {
      EXPECT_TRUE(R.Residual.Blocks.empty());
      EXPECT_TRUE(R.ReturningCalls.empty());
    }
  }
}

TEST_F(PEFixedImageTest, ReturningCallFreshResultKeepsBothFeasibleBranches) {
  const std::vector<uint8_t> Code{0xff, 0xd0, 0x85, 0xc0, 0x74, 1, 0xc3, 0xc3};
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x1000, Base + 0x1006);
  O.X64MachineExits.push_back({1, Base + 0x1007, MachineExitKind::Return});
  O.EntryConstants.push_back({NdVar::reg(x86reg::RAX, 8), 42});
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(R.ReturningCalls.size(), 1U);
  EXPECT_EQ(R.ExternalExits.size(), 2U);
  unsigned Branches = 0;
  for (const auto &B : R.Residual.Blocks)
    for (const auto &Op : B.Ops)
      Branches += Op.Opcode == NdOp::COND_BR;
  EXPECT_EQ(Branches, 1U);
}

TEST_F(PEFixedImageTest, ReturningCallResultObjectKeepsNullAndBoundedWrites) {
  // The explicit result relation admits NULL and a 64-byte object. Only the
  // nonnull branch writes within that object; the physical caller word must
  // remain intact across the call and the write.
  const std::vector<uint8_t> Code{0xba, 0x40, 0,    0,    0, // mov edx,64
                                  0x48, 0x83, 0xec, 0x40,    // sub rsp,64
                                  0xff, 0xd7,                // call rdi
                                  0x48, 0x83, 0xc4, 0x40,    // add rsp,64
                                  0x48, 0x85, 0xc0,          // test rax,rax
                                  0x74, 0x04,                // je ret
                                  0x48, 0x89, 0x48, 0x38,    // mov [rax+56],rcx
                                  0xc3};                     // ret
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x1009, Base + 0x1100);
  O.X64MachineExits = {{1, Base + 0x1100, MachineExitKind::Trap}};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-128, 8};
  O.EntryReturnSlot = SpecializationFrameSlot{0, 8};
  O.ProveStoreFrameSeparation = true;
  O.ProveStoreReturnSlotSeparation = true;
  auto &Call = O.X64ReturningCalls.front();
  Call.Version = 3;
  Call.Profile =
      SpecializationX64CallProfile::ConditionalNormalReturnResultObjectV3;
  Call.PreservedFrameRanges = {{-112, -104}};
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.ReturningCalls.size(), 1U);
  unsigned Branches = 0, Stores = 0, Returns = 0;
  for (const auto &Block : R.Residual.Blocks)
    for (const auto &Op : Block.Ops) {
      Branches += Op.Opcode == NdOp::COND_BR;
      Stores += Op.Opcode == NdOp::STORE;
      Returns += Op.Opcode == NdOp::RETURN;
    }
  EXPECT_EQ(Branches, 1U);
  EXPECT_GE(Stores, 2U); // physical CALL push and the bounded result write
  EXPECT_GE(Returns, 1U);
  auto Valid = validateSpecializationRawEnvironment(R, O);
  EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));

  auto WithoutObject = O;
  WithoutObject.X64ReturningCalls.front().Version = 2;
  WithoutObject.X64ReturningCalls.front().Profile =
      SpecializationX64CallProfile::ConditionalNormalReturnFrameSpansV2;
  const auto Missing =
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, WithoutObject);
  EXPECT_FALSE(Missing.complete());
  EXPECT_TRUE(Missing.Residual.Blocks.empty());

  auto ZeroSize = Code;
  ZeroSize[1] = 0;
  auto ZeroLoaded = load(machineExitFixture(ZeroSize));
  ASSERT_TRUE(bool(ZeroLoaded)) << llvm::toString(ZeroLoaded.takeError());
  const auto Zero = specializeBinaryInterpreter(*ZeroLoaded, Base + 0x1000, O);
  EXPECT_FALSE(Zero.complete());
  EXPECT_TRUE(Zero.Residual.Blocks.empty());

  auto UnknownSize = Code;
  std::fill_n(UnknownSize.begin(), 5, 0x90);
  auto UnknownLoaded = load(machineExitFixture(UnknownSize));
  ASSERT_TRUE(bool(UnknownLoaded)) << llvm::toString(UnknownLoaded.takeError());
  const auto Unknown =
      specializeBinaryInterpreter(*UnknownLoaded, Base + 0x1000, O);
  EXPECT_FALSE(Unknown.complete());
  EXPECT_TRUE(Unknown.Residual.Blocks.empty());

  auto OutOfRange = Code;
  OutOfRange[23] = 0x40;
  auto OutOfRangeLoaded = load(machineExitFixture(OutOfRange));
  ASSERT_TRUE(bool(OutOfRangeLoaded))
      << llvm::toString(OutOfRangeLoaded.takeError());
  const auto Outside =
      specializeBinaryInterpreter(*OutOfRangeLoaded, Base + 0x1000, O);
  EXPECT_FALSE(Outside.complete());
  EXPECT_TRUE(Outside.Residual.Blocks.empty());
}

TEST_F(PEFixedImageTest,
       ReturningCallRepeatedContainedWritesReuseNonnullProof) {
  std::vector<uint8_t> Code{0xba, 0x40, 0,    0,    0, // mov edx,64
                            0x48, 0x83, 0xec, 0x40,    // sub rsp,64
                            0xff, 0xd7,                // call rdi
                            0x48, 0x83, 0xc4, 0x40,    // add rsp,64
                            0x48, 0x85, 0xc0,          // test rax,rax
                            0x74, 0};                  // je ret
  for (unsigned I = 0; I < 8; ++I)
    Code.insert(Code.end(), {0x48, 0x89, 0x48, static_cast<uint8_t>(8 * I)});
  Code[19] = static_cast<uint8_t>(Code.size() - 20);
  Code.push_back(0xc3);
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x1009, Base + 0x1100);
  O.X64MachineExits = {{1, Base + 0x1100, MachineExitKind::Trap}};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-128, 8};
  O.EntryReturnSlot = SpecializationFrameSlot{0, 8};
  O.ProveStoreFrameSeparation = true;
  O.ProveStoreReturnSlotSeparation = true;
  O.MaxSolverQueries = 13;
  auto &Call = O.X64ReturningCalls.front();
  Call.Version = 3;
  Call.Profile =
      SpecializationX64CallProfile::ConditionalNormalReturnResultObjectV3;
  Call.PreservedFrameRanges = {{-112, -104}};
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  unsigned Stores = 0;
  for (const auto &Block : R.Residual.Blocks)
    for (const auto &Op : Block.Ops)
      Stores += Op.Opcode == NdOp::STORE;
  EXPECT_GE(Stores, 9U);
}

TEST_F(PEFixedImageTest, ReturningCallResultPointerSurvivesWritableGlobal) {
  // The old image word could name the entry frame. The program replaces it
  // with the returned object pointer before a later node reloads and writes.
  std::vector<uint8_t> Code{
      0x48, 0x89, 0x64, 0x24, 0x90,       // mov [rsp-112],rsp
      0xba, 0x40, 0,    0,    0,          // mov edx,64
      0x48, 0x83, 0xec, 0x40,             // sub rsp,64
      0xff, 0xd7,                         // call rdi
      0x48, 0x83, 0xc4, 0x40,             // add rsp,64
      0x48, 0x85, 0xc0,                   // test rax,rax
      0x74, 0x18,                         // je restore
      0x48, 0x89, 0x05, 0,    0,    0, 0, // mov [global],rax
      0x31, 0xc0,                         // xor eax,eax
      0xeb, 0x00,                         // jmp reload
      0x48, 0x8b, 0x1d, 0,    0,    0, 0, // mov rbx,[global]
      0x48, 0x89, 0x4b, 0x38,             // mov [rbx+56],rcx
      0xeb, 0x00,                         // jmp restore
      0x48, 0x8b, 0x5c, 0x24, 0x90,       // mov rbx,[rsp-112]
      0x48, 0x39, 0xe3,                   // cmp rbx,rsp
      0x75, 0x01,                         // jne trap
      0xc3,                               // ret
      0xcc};                              // trap
  constexpr va_t Global = Base + 0x30a0;
  put32(Code, 28, Global - (Base + 0x1000 + 32));
  put32(Code, 39, Global - (Base + 0x1000 + 43));
  auto Image = machineExitFixture(Code);
  put64(Image, IData + 0xa0, 0x800000);
  auto Loaded = load(Image);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x100e, Base + 0x103b);
  O.X64MachineExits = {{1, Base + 0x103b, MachineExitKind::Return},
                       {1, Base + 0x103c, MachineExitKind::Trap}};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-128, 8};
  O.EntryFrameBounds->ExcludedAddressRanges.push_back({Global, Global + 8});
  O.EntryReturnSlot = SpecializationFrameSlot{0, 8};
  O.ProveStoreFrameSeparation = true;
  O.ProveStoreReturnSlotSeparation = true;
  O.MaxChainedTransfers = 0;
  auto &Call = O.X64ReturningCalls.front();
  Call.Version = 3;
  Call.Profile =
      SpecializationX64CallProfile::ConditionalNormalReturnResultObjectV3;
  Call.PreservedFrameRanges = {{-112, -104}};
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.ReturningCalls.size(), 1U);
  const auto NativeAddress = [&](va_t ResidualAddress) {
    const auto It = std::find_if(
        R.Origins.begin(), R.Origins.end(), [&](const auto &Origin) {
          return Origin.ResidualAddress == ResidualAddress;
        });
    return It == R.Origins.end() ? InvalidVA : It->NativeInstruction.Address;
  };
  unsigned Branches = 0, Stores = 0;
  bool WroteGlobal = false, WroteObject = false;
  for (const auto &Block : R.Residual.Blocks)
    for (const auto &Op : Block.Ops) {
      Branches += Op.Opcode == NdOp::COND_BR;
      Stores += Op.Opcode == NdOp::STORE;
      if (Op.Opcode == NdOp::STORE) {
        WroteGlobal |= NativeAddress(Op.Addr) == Base + 0x1019;
        WroteObject |= NativeAddress(Op.Addr) == Base + 0x102b;
      }
    }
  EXPECT_EQ(Branches, 1U);
  EXPECT_GE(Stores, 3U);
  EXPECT_TRUE(WroteGlobal);
  EXPECT_TRUE(WroteObject);
  auto Valid = validateSpecializationRawEnvironment(R, O);
  EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
}

TEST_F(PEFixedImageTest, ReturningCallResultObjectBoundsSymbolicOffset) {
  // The index is symbolic but masked into the declared 64-byte extent. A null
  // result writes in the low address range; an explicit frame exclusion is
  // needed to preserve the entry frame on that branch.
  const std::vector<uint8_t> Code{0xba, 0x40, 0,    0,    0, // mov edx,64
                                  0x48, 0x83, 0xec, 0x40,    // sub rsp,64
                                  0xff, 0xd7,                // call rdi
                                  0x48, 0x83, 0xc4, 0x40,    // add rsp,64
                                  0x83, 0xe1, 0x1f,          // and ecx,31
                                  0x48, 0x89, 0x14, 0x08, // mov [rax+rcx],rdx
                                  0xc3};                  // ret
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x1009, Base + 0x1100);
  O.X64MachineExits = {{1, Base + 0x1100, MachineExitKind::Trap}};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-128, 8};
  O.EntryReturnSlot = SpecializationFrameSlot{0, 8};
  O.ProveStoreFrameSeparation = true;
  O.ProveStoreReturnSlotSeparation = true;
  auto &Call = O.X64ReturningCalls.front();
  Call.Version = 3;
  Call.Profile =
      SpecializationX64CallProfile::ConditionalNormalReturnResultObjectV3;
  Call.PreservedFrameRanges = {{-112, -104}};
  O.EntryFrameBounds->ExcludedAddressRanges.push_back({0, 64});
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  unsigned Stores = 0;
  for (const auto &Block : R.Residual.Blocks)
    for (const auto &Op : Block.Ops)
      Stores += Op.Opcode == NdOp::STORE;
  EXPECT_GE(Stores, 2U);

  auto WithoutLowExclusion = O;
  WithoutLowExclusion.EntryFrameBounds->ExcludedAddressRanges.clear();
  const auto Unproved =
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, WithoutLowExclusion);
  EXPECT_FALSE(Unproved.complete());
  EXPECT_TRUE(Unproved.Residual.Blocks.empty());

  auto Unbounded = Code;
  Unbounded[17] = 0x3f; // index may cross the 64-byte object boundary
  auto WideLoaded = load(machineExitFixture(Unbounded));
  ASSERT_TRUE(bool(WideLoaded)) << llvm::toString(WideLoaded.takeError());
  const auto Wide = specializeBinaryInterpreter(*WideLoaded, Base + 0x1000, O);
  EXPECT_FALSE(Wide.complete());
  EXPECT_TRUE(Wide.Residual.Blocks.empty());

  auto Checked = Code;
  Checked.insert(Checked.begin() + 15,
                 {0x48, 0x85, 0xc0, 0x74, 0x07}); // test rax,rax; je ret
  auto CheckedLoaded = load(machineExitFixture(Checked));
  ASSERT_TRUE(bool(CheckedLoaded)) << llvm::toString(CheckedLoaded.takeError());
  const auto Guarded = specializeBinaryInterpreter(
      *CheckedLoaded, Base + 0x1000, WithoutLowExclusion);
  EXPECT_TRUE(Guarded.complete()) << Guarded.Diagnostic;
}

TEST_F(PEFixedImageTest,
       ReturningCallRepeatedNativeSiteHasFiniteStaticReceipts) {
  // The saved nonvolatile counter is two. The same opaque site executes twice;
  // each return must be fresh even though target and native address repeat.
  const std::vector<uint8_t> Code{
      0xbb, 2,    0,    0,    0, // mov ebx,2
      0xff, 0xd7,                // site: call rdi
      0x48, 0xff, 0xcb,          // dec rbx
      0x75, 7,                   // first iteration: save result
      0x4c, 0x39, 0xe0,          // cmp rax,r12
      0x74, 1,    0xc3, 0xc3,    // equality and inequality both exit
      0x49, 0x89, 0xc4,          // mov r12,rax
      0xeb, 0xed};               // same site again
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x1005, Base + 0x1011);
  O.X64MachineExits.push_back({1, Base + 0x1012, MachineExitKind::Return});
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.ReturningCalls.size(), 2U);
  EXPECT_EQ(R.ExternalExits.size(), 2U);
  EXPECT_EQ(R.ReturningCalls[0].Original.Origin.Address,
            R.ReturningCalls[1].Original.Origin.Address);
  EXPECT_NE(R.ReturningCalls[0].ResidualAddress,
            R.ReturningCalls[1].ResidualAddress);
  auto Valid = validateSpecializationRawEnvironment(R, O);
  EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
}

TEST_F(PEFixedImageTest, ReturningCallReceiptsAndFullSpanAreBoundTogether) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0, 0xff, 0xd0, 0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x1000, Base + 0x1004);
  O.X64ReturningCalls.push_back(
      {1, Base + 0x1002,
       SpecializationX64CallProfile::ConditionalNormalReturnV1, 32});
  const auto Good = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  ASSERT_EQ(Good.ReturningCalls.size(), 2U);
  for (unsigned Change = 0; Change < 12; ++Change) {
    auto R = Good;
    switch (Change) {
    case 0:
      R.ReturningCalls.clear();
      break;
    case 1:
      R.ReturningCalls.push_back(R.ReturningCalls[0]);
      break;
    case 2:
      std::swap(R.ReturningCalls[0], R.ReturningCalls[1]);
      break;
    case 3:
      ++R.ReturningCalls[0].ResidualAddress;
      break;
    case 4:
      ++R.ReturningCalls[0].Original.Fallthrough.Address;
      break;
    case 5:
      R.ReturningCalls[0].Declaration.OutgoingBytes += 8;
      break;
    case 6:
      R.ReturningCalls[0].Original.NativeBytes[0] ^= 1;
      break;
    case 7:
      R.ReturningCalls[0].ExpansionScratch.Offset += 8;
      break;
    case 8:
      R.ReturningCalls[0].Original.Ops[0].ConsumesX87Argument = true;
      break;
    case 9:
      R.ReturningCalls[0].Original.UndefinedEffects.Diagnostic = "changed";
      break;
    case 10:
      R.ReturningCalls[0].Original.Ops[0].Inputs[5].Offset ^= 1;
      break;
    case 11:
      for (auto &B : R.Residual.Blocks)
        for (auto &Op : B.Ops)
          if (isLowRawReturningCall(Op))
            Op.Inputs[5].Offset ^= 8;
      break;
    }
    auto Error = validateSpecializationRawEnvironment(R, O);
    ASSERT_TRUE(bool(Error)) << Change;
    llvm::consumeError(std::move(Error));
  }
}

TEST_F(PEFixedImageTest, ReturningCallBudgetFailurePublishesNoPartialRelation) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0, 0xff, 0xd0, 0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x1000, Base + 0x1004);
  O.X64ReturningCalls.push_back(
      {1, Base + 0x1002,
       SpecializationX64CallProfile::ConditionalNormalReturnV1, 32});
  const auto Good = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  O.MaxOperations = Good.EvaluatedOperations;
  const auto Exact = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Exact.complete()) << Exact.Diagnostic;
  bool RegisterFailure = false, MemoryFailure = false,
       LaterMemoryFailure = false;
  for (uint64_t Work = 0; Work < Good.EvaluatedOperations; ++Work) {
    SCOPED_TRACE(Work);
    auto Short = O;
    Short.MaxOperations = Work;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, Short);
    // Zero is invalid at admission; every positive short allowance exhausts.
    EXPECT_EQ(R.Status, Work ? SpecializationStatus::BudgetExceeded
                             : SpecializationStatus::InvalidInput)
        << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_TRUE(R.Origins.empty());
    EXPECT_TRUE(R.Reads.empty());
    EXPECT_TRUE(R.ReturningCalls.empty());
    EXPECT_TRUE(R.ExternalExits.empty());
    EXPECT_FALSE(R.RawEnvironment);
    RegisterFailure |=
        R.Diagnostic.find("register preservation refused") != std::string::npos;
    const bool Memory =
        R.Diagnostic.find("memory preservation refused") != std::string::npos;
    MemoryFailure |= Memory;
    LaterMemoryFailure |=
        Memory && R.Diagnostic.find("140001002") != std::string::npos;
  }
  EXPECT_TRUE(RegisterFailure);
  EXPECT_TRUE(MemoryFailure);
  EXPECT_TRUE(LaterMemoryFailure);
  for (uint64_t Nodes : {uint64_t{1}, uint64_t{16}, uint64_t{64}}) {
    auto Short = O;
    Short.MaxSymbolicNodes = Nodes;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, Short);
    EXPECT_EQ(R.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_TRUE(R.ReturningCalls.empty());
  }
}

TEST_F(PEFixedImageTest,
       ReturningCallFreshAddressCannotGainEntrySeparationAtJoin) {
  for (bool CallOnTaken : {false, true})
    for (unsigned Write : {0U, 1U, 2U}) {
      // Entry R11 is an external input, but the call's unread R11 is arbitrary.
      // Join both arrival orders after an optional call, then store through
      // R11. A complete later write from preserved RDI may reestablish safe
      // origin; a partial byte write cannot do so for the whole address.
      std::vector<uint8_t> Code{0x48, 0xc7, 0x44, 0x24, 0x28, 7, 0,
                                0,    0,    0x85, 0xc9, 0x74, 4};
      va_t Call;
      if (!CallOnTaken) {
        Call = Base + 0x1000 + Code.size();
        Code.insert(Code.end(), {0xff, 0xd7, 0xeb, 3, 0x90, 0xeb, 0});
      } else {
        Code[12] = 3;
        Code.insert(Code.end(), {0x90, 0xeb, 4});
        Call = Base + 0x1000 + Code.size();
        Code.insert(Code.end(), {0xff, 0xd7, 0xeb, 0});
      }
      if (Write == 1)
        Code.insert(Code.end(), {0x49, 0x89, 0xfb}); // mov r11,rdi
      else if (Write == 2)
        Code.insert(Code.end(), {0x41, 0xb3, 0}); // mov r11b,0
      Code.insert(Code.end(),
                  {0x41, 0x89, 0x03, // mov [r11],eax
                   0x48, 0x83, 0x7c, 0x24, 0x28, 7, 0x74, 2, 0x0f, 0x0b, 0xc3});
      auto Loaded = load(machineExitFixture(Code));
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      auto O = returningCallOptions(Call, Base + 0x1000 + Code.size() - 1);
      O.MaxChainedTransfers = 0;
      O.ExternalStoresDisjointEntryFrame = true;
      const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
      EXPECT_EQ(R.complete(), Write == 1) << R.Diagnostic;
      if (Write != 1)
        EXPECT_TRUE(R.ReturningCalls.empty());
    }
}
