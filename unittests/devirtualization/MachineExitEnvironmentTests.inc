//===- MachineExitEnvironmentTests.inc - Explicit machine regions --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent byte-memory oracles for declared physical transfers.
// Included after RawCodeEnvironmentTests.inc in the fixed-PE test namespace.

using MachineExitKind = SpecializationX64MachineExitKind;

SpecializationOptions machineExitOptions(va_t Address, MachineExitKind Kind) {
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CetDisabled =
      true;
  O.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
  O.MaxChainedTransfers = 32;
  O.X64MachineExits = {{1, Address, Kind}};
  return O;
}

std::vector<uint8_t> machineExitFixture(llvm::ArrayRef<uint8_t> Code) {
  auto B = fixture();
  EXPECT_LE(Code.size(), SectionBytes);
  std::fill_n(B.begin() + Text, SectionBytes, 0xcc);
  std::copy(Code.begin(), Code.end(), B.begin() + Text);
  return B;
}

RawByteOracle machineExitState(uint64_t Stack) {
  RawByteOracle S;
  const uint64_t GPRs[] = {x86reg::RAX, x86reg::RBX, x86reg::RCX, x86reg::RDX,
                           x86reg::RSI, x86reg::RDI, x86reg::RBP, x86reg::RSP,
                           x86reg::R8,  x86reg::R9,  x86reg::R10, x86reg::R11,
                           x86reg::R12, x86reg::R13, x86reg::R14, x86reg::R15};
  for (uint64_t R : GPRs)
    S.write(NdVar::reg(R, 8), 0x9876543210000000 + R * 31);
  for (uint64_t F :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    S.write(NdVar::reg(F, 1), F & 1);
  S.write(NdVar::reg(x86reg::RSP, 8), Stack);
  for (unsigned I = 0; I != 256; ++I)
    S.Memory[Stack - 128 + I] = I ^ 0x69;
  return S;
}

uint64_t machineExitWord(const RawByteOracle &S, uint64_t Address) {
  uint64_t Value = 0;
  for (unsigned I = 0; I != 8; ++I)
    Value |= uint64_t{S.Memory.at(Address + I)} << (8 * I);
  return Value;
}

void expectMachineExitState(RawByteOracle Actual, const RawByteOracle &Expected,
                            uint64_t Target) {
  std::erase_if(Actual.Registers, [](const auto &V) {
    return V.first.first == VnodeSpace::TEMP;
  });
  EXPECT_EQ(Actual.Registers, Expected.Registers);
  EXPECT_EQ(Actual.Memory, Expected.Memory);
  EXPECT_EQ(Actual.NextPC, Target);
}

TEST_F(PEFixedImageTest, MachineExitRetReadsAliasedBytesBeforePhysicalPop) {
  for (uint16_t Cleanup : {uint16_t{0}, uint16_t{24}, uint16_t{65535}}) {
    // mov [rcx],rax; ret [imm16]. The store may partially or wholly replace
    // the return word; no entry-slot preservation premise is supplied.
    std::vector<uint8_t> Code{0x48, 0x89, 0x01};
    if (Cleanup)
      Code.insert(Code.end(), {0xc2, uint8_t(Cleanup), uint8_t(Cleanup >> 8)});
    else
      Code.push_back(0xc3);
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Base + 0x1003, MachineExitKind::Return);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_EQ(R.ExternalExits.size(), 1U);
    ASSERT_TRUE(R.ExternalExits.front().MachineExit);
    for (uint64_t Stack : {uint64_t{0x800000}, uint64_t{0xffff900020000000}})
      for (int Offset : {-7, 0, 1, 8, 32}) {
        auto Actual = machineExitState(Stack);
        // Keep the complete and partially overwritten targets canonical in
        // these controls. The oracle checks bytes, not executable mappings.
        Actual.memory(Stack, 0x12340, 8);
        Actual.write(NdVar::reg(x86reg::RAX, 8), 0x345678);
        const uint64_t Address = Stack + static_cast<uint64_t>(Offset);
        Actual.write(NdVar::reg(x86reg::RCX, 8), Address);
        const auto Value = Actual.read(NdVar::reg(x86reg::RAX, 8));
        auto Expected = Actual;
        Expected.memory(Address, Value, 8);
        const auto Target = machineExitWord(Expected, Stack);
        Expected.write(NdVar::reg(x86reg::RSP, 8), Stack + 8 + Cleanup);
        ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
        expectMachineExitState(Actual, Expected, Target);
      }
    auto Ordinary = O;
    Ordinary.X64MachineExits.clear();
    Ordinary.RequireRestoredFrameAtReturn = true;
    // Keep the explicit raw domain. The ordinary binary adapter supplies its
    // separate external-pointer ABI premise when no raw declaration is used.
    Ordinary.X64CodeRegions.push_back(
        {1, 0x400000, {0xc3}, "unused raw-domain control"});
    EXPECT_FALSE(specializeBinaryInterpreter(*Loaded, Base + 0x1000, Ordinary)
                     .complete());
  }
  // A declared RET also preserves a dynamically replaced stack pointer and
  // modular post-pop arithmetic; it need not be relative to the entry root.
  auto Loaded = load(machineExitFixture({0x48, 0x89, 0xcc, 0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1003, MachineExitKind::Return);
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t NewStack : {uint64_t{0x900000}, UINT64_MAX - 7}) {
    auto Actual = machineExitState(0x800000);
    Actual.write(NdVar::reg(x86reg::RCX, 8), NewStack);
    Actual.memory(NewStack, 0x12340, 8);
    auto Expected = Actual;
    Expected.write(NdVar::reg(x86reg::RSP, 8), NewStack + 8);
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    expectMachineExitState(Actual, Expected, 0x12340);
  }
}

TEST_F(PEFixedImageTest, MachineExitCallCapturesTargetBeforeAliasingPush) {
  const std::vector<std::vector<uint8_t>> Calls = {
      {0xff, 0x54, 0x24, 0xf8}, // call [rsp-8], exactly the push destination
      {0xff, 0x14, 0x24},       // call [rsp]
      {0xff, 0xd4},             // call rsp, target is the old RSP
      {0xff, 0xd0},             // call rax
      {0xe8, 0x40, 0, 0, 0}};   // call original fallthrough+64
  for (unsigned I = 0; I != Calls.size(); ++I) {
    SCOPED_TRACE(I);
    auto Loaded = load(machineExitFixture(Calls[I]));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Base + 0x1000, MachineExitKind::Call);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_EQ(R.Origins.size(), 1U);
    for (uint64_t TargetSeed :
         {uint64_t{0x12340}, uint64_t{0xffff800012345678}}) {
      constexpr uint64_t Stack = 0x800000;
      auto Actual = machineExitState(Stack);
      Actual.write(NdVar::reg(x86reg::RAX, 8), TargetSeed);
      Actual.memory(Stack - 8, TargetSeed, 8);
      Actual.memory(Stack, TargetSeed, 8);
      auto Expected = Actual;
      const uint64_t Return = Base + 0x1000 + Calls[I].size();
      const uint64_t Target = I == 2   ? Stack
                              : I == 4 ? Return + 64
                                       : TargetSeed;
      Expected.memory(Stack - 8, Return, 8);
      Expected.write(NdVar::reg(x86reg::RSP, 8), Stack - 8);
      ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
      expectMachineExitState(Actual, Expected, Target);
    }
  }
}

TEST_F(PEFixedImageTest,
       MachineExitJumpRetainsDynamicAndOriginalDirectTargets) {
  const std::vector<std::vector<uint8_t>> Jumps = {
      {0xff, 0xe0}, {0xff, 0x60, 0x18}, {0xe9, 0x20, 0, 0, 0}};
  for (unsigned I = 0; I != Jumps.size(); ++I) {
    auto Loaded = load(machineExitFixture(Jumps[I]));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Base + 0x1000, MachineExitKind::Jump);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    for (uint64_t Target : {uint64_t{0x12340}, uint64_t{0xffff800012345678}}) {
      auto Actual = machineExitState(0x800000);
      Actual.write(NdVar::reg(x86reg::RAX, 8), I == 1 ? 0x900000 : Target);
      Actual.memory(0x900018, Target, 8);
      const auto Expected = Actual;
      ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
      expectMachineExitState(Actual, Expected, I == 2 ? Base + 0x1025 : Target);
    }
  }
}

TEST_F(PEFixedImageTest, MachineExitKeepsInternalCallsAndReturnProofs) {
  const uint8_t Code[] = {0xe8, 3,    0,    0,    0, 0xc3, 0xcc,
                          0xcc, 0x48, 0x8d, 0x40, 7, 0xc3};
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1005, MachineExitKind::Return);
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.ExternalExits.size(), 1U);
  EXPECT_EQ(R.ExternalExits[0].Original.Origin.Address, Base + 0x1005);
  auto Actual = machineExitState(0x800000);
  auto Expected = Actual;
  Expected.memory(0x7ffff8, Base + 0x1005, 8);
  Expected.write(NdVar::reg(x86reg::RAX, 8),
                 Expected.read(NdVar::reg(x86reg::RAX, 8)) + 7);
  Expected.write(NdVar::reg(x86reg::RSP, 8), 0x800008);
  ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
  expectMachineExitState(Actual, Expected, machineExitWord(Expected, 0x800000));

  // A declaration elsewhere cannot authorize an unknown return or callback.
  for (const auto &BadCode :
       std::vector<std::vector<uint8_t>>{{0xc3}, {0xff, 0xd0}, {0xff, 0xe0}}) {
    auto Bad = load(machineExitFixture(BadCode));
    ASSERT_TRUE(bool(Bad)) << llvm::toString(Bad.takeError());
    O.X64MachineExits[0].Address = Base + 0x1100;
    const auto Refused = specializeBinaryInterpreter(*Bad, Base + 0x1000, O);
    EXPECT_FALSE(Refused.complete());
    EXPECT_TRUE(Refused.Residual.Blocks.empty());
    EXPECT_TRUE(Refused.ExternalExits.empty());
  }
}

TEST_F(PEFixedImageTest, MachineExitRequiresExactDomainBytesAndTransferKind) {
  auto Loaded = load(machineExitFixture({0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto Good = machineExitOptions(Base + 0x1000, MachineExitKind::Return);
  for (unsigned I = 0; I != 13; ++I) {
    auto O = Good;
    switch (I) {
    case 0:
      O.X64MachineExits[0].Version = 2;
      break;
    case 1:
      O.X64MachineExits[0].Kind = MachineExitKind::Call;
      break;
    case 2:
      O.X64MachineExits[0].Kind = MachineExitKind::Jump;
      break;
    case 3:
      O.X64MachineExits[0].Kind = MachineExitKind(255);
      break;
    case 4:
      O.X64MachineExits[0].Address = InvalidVA;
      break;
    case 5:
      O.X64MachineExits.push_back(O.X64MachineExits[0]);
      break;
    case 6:
      O.RequireRestoredFrameAtReturn = true;
      break;
    case 7:
      O.ExternalStoresPreserveEntryReturnSlot = true;
      break;
    case 8:
      O.ExplicitMachineState = false;
      break;
    case 9:
      O.X64CetDisabled = false;
      break;
    case 10:
      O.FrameBaseRegister.reset();
      break;
    case 11:
      O.ByteOrder = llvm::endianness::big;
      break;
    case 12:
      O.X64MachineExits[0].Address = 0x800000;
      break;
    }
    SCOPED_TRACE(I);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0x66, 0xc3}, {0xcb}, {0x74, 0x10}, {0xcc}, {0x90}}) {
    auto Bad = load(machineExitFixture(Bytes));
    ASSERT_TRUE(bool(Bad)) << llvm::toString(Bad.takeError());
    EXPECT_FALSE(
        specializeBinaryInterpreter(*Bad, Base + 0x1000, Good).complete());
  }
}

TEST_F(PEFixedImageTest, MachineExitProvesLiveInternalReturnCells) {
  for (bool PushAtRoot : {false, true}) {
    std::vector<uint8_t> Code;
    if (PushAtRoot)
      Code = {0x48, 0x8d, 0x64, 0x24, 8}; // lea rsp,[rsp+8]
    const size_t Call = Code.size();
    Code.insert(Code.end(), {0xe8, 0, 0, 0, 0});
    if (PushAtRoot)
      Code.insert(Code.end(), {0x48, 0x8d, 0x64, 0x24, 0xf8});
    const size_t Return = Code.size();
    Code.insert(Code.end(), {0xc3, 0xcc});
    const size_t Helper = Code.size();
    Code.insert(Code.end(), {0x48, 0x89, 0x01, 0xc3}); // mov [rcx],rax; ret
    put32(Code, Call + 1, Helper - Call - 5);
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O =
        machineExitOptions(Base + 0x1000 + Return, MachineExitKind::Return);
    O.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
    // The arbitrary high address bits remain free. The low-byte declaration
    // and entry alignment prove eight-byte separation from the active return
    // word, including when that internal word occupies the entry root itself.
    O.EntryConstants.push_back(
        {NdVar::reg(x86reg::RCX, 1), PushAtRoot ? 0U : 8U});
    O.ProveStoreReturnSlotSeparation = true;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete())
        << R.Diagnostic << "; push-at-root=" << PushAtRoot;
    for (uint64_t Separation : {uint64_t{0}, uint64_t{256}}) {
      constexpr uint64_t Stack = 0x800008;
      const uint64_t Address = Stack - (PushAtRoot ? 8 : 0) + Separation;
      auto Actual = machineExitState(Stack);
      Actual.memory(Stack, 0x12340, 8);
      Actual.write(NdVar::reg(x86reg::RAX, 8), 0x345678);
      Actual.write(NdVar::reg(x86reg::RCX, 8), Address);
      auto Expected = Actual;
      Expected.memory(Stack - (PushAtRoot ? 0 : 8), Base + 0x1000 + Call + 5,
                      8);
      Expected.memory(Address, 0x345678, 8);
      Expected.write(NdVar::reg(x86reg::RSP, 8), Stack + 8);
      ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
      expectMachineExitState(Actual, Expected,
                             machineExitWord(Expected, Stack));
    }
    for (bool DropAlignment : {false, true}) {
      auto Unproved = O;
      if (DropAlignment)
        Unproved.EntryFrameAlignment.reset();
      else
        Unproved.ProveStoreReturnSlotSeparation = false;
      const auto Refused =
          specializeBinaryInterpreter(*Loaded, Base + 0x1000, Unproved);
      EXPECT_FALSE(Refused.complete());
      EXPECT_TRUE(Refused.Residual.Blocks.empty());
    }
  }
}

TEST_F(PEFixedImageTest, MachineExitLookupUsesBoundedIndexedWork) {
  std::vector<uint8_t> Code(64, 0x90);
  Code.push_back(0xc3);
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  for (bool Reverse : {false, true}) {
    auto O = machineExitOptions(Base + 0x1040, MachineExitKind::Return);
    for (unsigned I = 0; I != 31; ++I)
      O.X64MachineExits.push_back(
          {1, Base + 0x1100 + I, MachineExitKind::Return});
    if (Reverse)
      std::reverse(O.X64MachineExits.begin(), O.X64MachineExits.end());
    // Enough for a bounded index and all original instructions, but not a
    // complete declaration scan at every NOP. No declaration may end a NOP.
    O.MaxOperations = 1024;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_TRUE(R.RawEnvironment);
    ASSERT_EQ(R.ExternalExits.size(), 1u);
    ASSERT_TRUE(R.ExternalExits.front().MachineExit);
    EXPECT_EQ(R.ExternalExits.front().MachineExit->Address, Base + 0x1040);
    auto Actual = machineExitState(0x800000);
    Actual.memory(0x800000, 0x12340, 8);
    auto Expected = Actual;
    Expected.write(NdVar::reg(x86reg::RSP, 8), 0x800008);
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    expectMachineExitState(Actual, Expected, 0x12340);
    O.MaxOperations = R.EvaluatedOperations;
    EXPECT_TRUE(
        specializeBinaryInterpreter(*Loaded, Base + 0x1000, O).complete());
    for (uint64_t Limit : {uint64_t{1}, R.EvaluatedOperations - 1}) {
      O.MaxOperations = Limit;
      const auto Short = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
      EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
      EXPECT_TRUE(Short.Residual.Blocks.empty());
      EXPECT_TRUE(Short.Origins.empty());
      EXPECT_TRUE(Short.Reads.empty());
      EXPECT_TRUE(Short.ExternalExits.empty());
      EXPECT_FALSE(Short.RawEnvironment);
    }
  }
}

TEST_F(PEFixedImageTest, MachineExitDiscoversBoundedCopyDestination) {
  // Reserve a local buffer and call a short copy loop with count ECX & 3.
  // The loop stops at either a zero word or the exhausted count. Its live
  // destination is an entry-relative pointer, not an absolute constant.
  const std::vector<uint8_t> Code{
      0x48, 0x8d, 0x64, 0x24, 0xb0, 0x48, 0x8d, 0x7c, 0x24, 0x28, 0x83, 0xe1,
      0x03, 0xe8, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x64, 0x24, 0x50, 0xc3,
      0x48, 0x85, 0xc9, 0x74, 0x1b, 0x66, 0x8b, 0x06, 0x66, 0x89, 0x07, 0x48,
      0x8d, 0x76, 0x02, 0x48, 0x8d, 0x7f, 0x02, 0x48, 0xff, 0xc9, 0x66, 0x85,
      0xc0, 0x74, 0x05, 0x48, 0x85, 0xc9, 0x75, 0xe5, 0xc3};
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1017, MachineExitKind::Return);
  O.EntryFrameBounds = SpecializationEntryFrameBounds{0, 8, {}};
  O.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
  O.ProveStoreFrameSeparation = O.ProveStoreReturnSlotSeparation = true;
  O.DiscoverControlState = true;
  ASSERT_TRUE(O.ControlRegisters.empty());
  ASSERT_TRUE(O.EntryConstants.empty());
  O.MaxSolverQueries = 16384;
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_FALSE(bool(validateSpecializationRawEnvironment(R, O)));
  for (uint64_t Stack : {UINT64_C(0x800008), UINT64_C(0xffff900020000008)})
    for (uint64_t Count : {UINT64_C(0), UINT64_C(1), UINT64_C(2), UINT64_MAX})
      for (int Gap : {-2, 0, 2, 72})
        for (unsigned Zero : {0, 1, 3}) {
          auto Actual = machineExitState(Stack);
          const uint64_t Source = Stack - 40 + static_cast<uint64_t>(Gap);
          Actual.memory(Stack, 0x12340, 8);
          Actual.write(NdVar::reg(x86reg::RCX, 8), Count);
          Actual.write(NdVar::reg(x86reg::RSI, 8), Source);
          Actual.write(NdVar::reg(x86reg::AF, 1), 1);
          for (unsigned I = 0; I != 4; ++I)
            Actual.memory(Source + 2 * I, I == Zero ? 0 : 0x8123 + I, 2);
          auto Expected = Actual;
          Expected.memory(Stack - 88, Base + 0x1012, 8);
          uint64_t Left = Count & 3, Read = Source, Write = Stack - 40;
          while (Left) {
            const uint16_t Value = Expected.Memory.at(Read) |
                                   uint16_t(Expected.Memory.at(Read + 1)) << 8;
            Expected.memory(Write, Value, 2);
            Expected.write(NdVar::reg(x86reg::RAX, 2), Value);
            Read += 2;
            Write += 2;
            --Left;
            if (!Value)
              break;
          }
          Expected.write(NdVar::reg(x86reg::RCX, 8), Left);
          Expected.write(NdVar::reg(x86reg::RSI, 8), Read);
          Expected.write(NdVar::reg(x86reg::RDI, 8), Write);
          Expected.write(NdVar::reg(x86reg::RSP, 8), Stack + 8);
          for (uint64_t Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                                x86reg::SF, x86reg::OF})
            Expected.write(NdVar::reg(Flag, 1),
                           Flag == x86reg::PF || Flag == x86reg::ZF ||
                               (Flag == x86reg::AF && !(Count & 3)));
          ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
          expectMachineExitState(Actual, Expected, 0x12340);
        }
  for (uint8_t Offset : {uint8_t{0xf8}, uint8_t{0xff}}) {
    // Possible complete and one-byte overlaps must still invalidate the
    // native return word; nominating a dependency supplies no alias proof.
    auto Aliased = Code;
    Aliased[9] = Offset;
    auto BadImage = load(machineExitFixture(Aliased));
    ASSERT_TRUE(bool(BadImage)) << llvm::toString(BadImage.takeError());
    const auto Bad = specializeBinaryInterpreter(*BadImage, Base + 0x1000, O);
    EXPECT_FALSE(Bad.complete());
    EXPECT_TRUE(Bad.Residual.Blocks.empty());
    EXPECT_FALSE(Bad.RawEnvironment);
  }
  auto NoDiscovery = O;
  NoDiscovery.DiscoverControlState = false;
  EXPECT_FALSE(specializeBinaryInterpreter(*Loaded, Base + 0x1000, NoDiscovery)
                   .complete());
  for (bool Queries : {false, true}) {
    auto Exact = O;
    auto &Limit = Queries ? Exact.MaxSolverQueries : Exact.MaxOperations;
    Limit = Queries ? R.SolverQueries : R.EvaluatedOperations;
    ASSERT_GT(Limit, 0u);
    const auto Good =
        specializeBinaryInterpreter(*Loaded, Base + 0x1000, Exact);
    ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
    --Limit;
    const auto Short =
        specializeBinaryInterpreter(*Loaded, Base + 0x1000, Exact);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded)
        << Short.Diagnostic;
    EXPECT_TRUE(Short.Residual.Blocks.empty());
    EXPECT_FALSE(Short.RawEnvironment);
  }
}

TEST_F(PEFixedImageTest, MachineTrapExitEndsRegionAtDeclaredTrap) {
  const std::vector<uint8_t> Encodings[] = {{0xcc}, {0xf1}, {0xcd, 0x03}};
  for (const auto &Trap : Encodings) {
    SCOPED_TRACE(unsigned(Trap.front()));
    // NOP; NOP; INT3/INT1; RET. The declared trap ends the region before RET.
    std::vector<uint8_t> Code{0x90, 0x90};
    Code.insert(Code.end(), Trap.begin(), Trap.end());
    Code.push_back(0xc3);
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Base + 0x1002, MachineExitKind::Trap);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_TRUE(R.RawEnvironment);
    ASSERT_EQ(R.ExternalExits.size(), 1u);
    ASSERT_TRUE(R.ExternalExits.front().MachineExit);
    EXPECT_EQ(R.ExternalExits.front().MachineExit->Address, Base + 0x1002);
    EXPECT_EQ(R.ExternalExits.front().MachineExit->Kind, MachineExitKind::Trap);
    EXPECT_EQ(R.ExternalExits.front().Original.Origin.Size, Trap.size());
    auto Error = validateSpecializationRawEnvironment(R, O);
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    ASSERT_FALSE(R.Residual.Blocks.empty());
    const auto &Terminal = R.Residual.Blocks.back();
    EXPECT_TRUE(Terminal.Succs.empty());
    ASSERT_FALSE(Terminal.Ops.empty());
    EXPECT_EQ(Terminal.Ops.back().Opcode, NdOp::INTRINSIC);
    ASSERT_EQ(Terminal.Ops.back().NumInputs, 1u);
    EXPECT_EQ(Terminal.Ops.back().Inputs[0].Offset,
              static_cast<uint64_t>(Trap.front() == 0xf1 ? Intrinsic::Int1
                                                         : Intrinsic::Int3));
    for (const auto &B : R.Residual.Blocks)
      for (const auto &Op : B.Ops)
        EXPECT_NE(Op.Opcode, NdOp::RETURN);
    O.MaxOperations = R.EvaluatedOperations;
    EXPECT_TRUE(
        specializeBinaryInterpreter(*Loaded, Base + 0x1000, O).complete());
    --O.MaxOperations;
    const auto Short = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.Residual.Blocks.empty());
    EXPECT_TRUE(Short.ExternalExits.empty());
  }
}

TEST_F(PEFixedImageTest, UndeclaredTrapRemainsUnsupported) {
  const std::vector<uint8_t> Encodings[] = {{0xcc}, {0xf1}, {0xcd, 0x03}};
  for (const auto &Trap : Encodings) {
    SCOPED_TRACE(unsigned(Trap.front()));
    // The same trap, left undeclared, still fails closed.
    auto Loaded = load(machineExitFixture(Trap));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    SpecializationOptions O;
    O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CetDisabled =
        true;
    O.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
    O.RequireRestoredFrameAtReturn = true;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
    EXPECT_NE(R.Diagnostic.find("opaque native terminator"), std::string::npos);
  }
}

TEST_F(PEFixedImageTest, MachineTrapExitRejectsNonTrapInstruction) {
  const std::vector<uint8_t> Encodings[] = {{0x90}, {0x0f, 0x0b}, {0xcd, 0x02}};
  for (const auto &Code : Encodings) {
    SCOPED_TRACE(unsigned(Code.front()));
    // NOP, UD2 and INT 2 do not have the declared resumable-trap contract.
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Base + 0x1000, MachineExitKind::Trap);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_FALSE(R.complete());
    EXPECT_EQ(R.Status, SpecializationStatus::InvalidInput);
  }
}

TEST_F(PEFixedImageTest, MachineExitIntegrityBudgetsAndOrdinaryConsumers) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  Image = std::move(*Loaded);
  Options = machineExitOptions(Base + 0x1000, MachineExitKind::Call);
  const auto R = specializeBinaryInterpreter(Image, Base + 0x1000, Options);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_TRUE(R.RawEnvironment);
  ASSERT_EQ(R.ExternalExits.size(), 1U);
  for (unsigned I = 0; I != 10; ++I) {
    auto Changed = R;
    switch (I) {
    case 0:
      Changed.ExternalExits.clear();
      break;
    case 1:
      Changed.ExternalExits[0].MachineExit.reset();
      break;
    case 2:
      Changed.ExternalExits[0].MachineExit->Address++;
      break;
    case 3:
      Changed.ExternalExits[0].MachineExit->Kind = MachineExitKind::Jump;
      break;
    case 4:
      Changed.ExternalExits[0].Original.NativeBytes[1] ^= 1;
      break;
    case 5:
      Changed.ExternalExits[0].ExpandedOperationDigest.clear();
      break;
    case 6:
      Changed.ExternalExits[0].ExpansionScratch.Offset++;
      break;
    case 7:
      Changed.Residual.Blocks.back().Ops.back().Inputs[0].Offset++;
      break;
    case 8:
      Changed.RawEnvironment.reset();
      break;
    case 9:
      Changed.Residual.Blocks.front().Ops.front().Inputs[1].Offset++;
      break;
    }
    auto Error = validateSpecializationRawEnvironment(Changed, Options);
    EXPECT_TRUE(bool(Error)) << I;
    llvm::consumeError(std::move(Error));
  }
  for (unsigned I = 0; I != 4; ++I) {
    auto O = Options;
    if (I == 0)
      O.MaxOperations = R.EvaluatedOperations - 1;
    if (I == 1)
      O.MaxImagePreparationRecords = 1;
    if (I == 2)
      O.MaxRawEnvironmentResultRecords = 1;
    if (I == 3)
      O.MaxRawEnvironmentResultBytes = 1;
    const auto Limited = specializeBinaryInterpreter(Image, Base + 0x1000, O);
    EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Limited.Residual.Blocks.empty());
    EXPECT_TRUE(Limited.ExternalExits.empty());
  }
  auto Exact = Options;
  Exact.MaxOperations = R.EvaluatedOperations;
  EXPECT_TRUE(
      specializeBinaryInterpreter(Image, Base + 0x1000, Exact).complete());
  auto Different = Options;
  Different.X64MachineExits[0].Kind = MachineExitKind::Jump;
  auto Error = validateSpecializationRawEnvironment(R, Different);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  for (bool Unused : {false, true}) {
    if (Unused)
      Options.X64MachineExits[0].Address = Base + 0x1100;
    EXPECT_EQ(proof().Proof.Status, LowIRIndependenceStatus::Unsupported);
    const auto Native = checkBinaryLowIRRefinement(
        Image, Base + 0x1000, Options, R.Residual, Contract);
    EXPECT_EQ(Native.Proof.Status, LowIRRefinementStatus::Unsupported);
    EXPECT_FALSE(Native.Certificate);
    const auto LLVM =
        checkBinaryLLVMRefinement(Image, Base + 0x1000, Options, R.Residual, "",
                                  "model", *Contract.Frame);
    EXPECT_EQ(LLVM.Native.Proof.Status, LowIRRefinementStatus::Unsupported);
  }
  for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                      InterpreterMachineStateLayout::SharedGPRExit}) {
    auto Wrapped = wrapInterpreterMachineStateX64(
        R.Residual, BinaryFormat::COFF,
        InterpreterMachineStateProfile::UserX64NoFaultV1, std::nullopt, Layout);
    EXPECT_FALSE(bool(Wrapped));
    if (!Wrapped)
      llvm::consumeError(Wrapped.takeError());
  }
}

#include "RecoveryArchitectureTests.inc"
#include "UnrootedStackEnvironmentTests.inc"
