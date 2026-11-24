//===- UnrootedStackEnvironmentTests.inc - Physical alternate stacks -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independently authored calls and returns on an arbitrary guest stack.

std::vector<uint8_t> unrootedStackFixture(bool Indirect, uint16_t Cleanup,
                                          bool Redirect = false,
                                          bool UnknownReturn = false) {
  // mov rsp,rcx. RCX is unrelated to the entry RSP; it may alias that stack.
  std::vector<uint8_t> Code{0x48, 0x89, 0xcc};
  if (Indirect) {
    // movabs rax,helper; mov [rsp-8],rax; call [rsp-8]. The physical CALL
    // overwrites its own target cell, after capturing the original target.
    Code.insert(Code.end(), {0x48, 0xb8});
    for (unsigned I = 0; I != 8; ++I)
      Code.push_back(uint8_t((Base + 0x1040) >> (8 * I)));
    Code.insert(Code.end(),
                {0x48, 0x89, 0x44, 0x24, 0xf8, 0xff, 0x54, 0x24, 0xf8});
  } else {
    Code.insert(Code.end(), {0xe8, 0x38, 0, 0, 0});
  }
  Code.push_back(0xc3);
  Code.resize(0x40, 0xcc);
  if (UnknownReturn) {
    // mov [rsp],rdx. No call-stack prediction may replace this unknown word.
    Code.insert(Code.end(), {0x48, 0x89, 0x14, 0x24});
  } else if (Redirect) {
    // Change the actual return address to a different, authenticated body.
    Code.insert(Code.end(), {0x48, 0xb8});
    for (unsigned I = 0; I != 8; ++I)
      Code.push_back(uint8_t((Base + 0x1060) >> (8 * I)));
    Code.insert(Code.end(), {0x48, 0x89, 0x04, 0x24});
  } else {
    // mov rax,[rsp]; lea rdx,[rdx+7]. Observe the real pushed continuation.
    Code.insert(Code.end(), {0x48, 0x8b, 0x04, 0x24, 0x48, 0x8d, 0x52, 7});
  }
  if (Cleanup)
    Code.insert(Code.end(), {0xc2, uint8_t(Cleanup), uint8_t(Cleanup >> 8)});
  else
    Code.push_back(0xc3);
  if (Redirect) {
    Code.resize(0x60, 0xcc);
    Code.insert(Code.end(), {0x48, 0x8d, 0x52, 9, 0xc3});
  }
  return machineExitFixture(Code);
}

TEST_F(PEFixedImageTest, MachineRegionUnrootedCallsPreservePhysicalState) {
  for (bool Indirect : {false, true})
    for (uint16_t Cleanup : {uint16_t{0}, uint16_t{24}}) {
      SCOPED_TRACE(Indirect);
      SCOPED_TRACE(Cleanup);
      auto Loaded = load(unrootedStackFixture(Indirect, Cleanup));
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      const auto Continuation = Base + (Indirect ? 0x1016 : 0x1008);
      auto O = machineExitOptions(Continuation, MachineExitKind::Return);
      const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      ASSERT_EQ(R.ExternalExits.size(), 1U);
      for (uint64_t Stack :
           {uint64_t{0}, uint64_t{0x800000}, uint64_t{0x800008},
            uint64_t{0x900000}, UINT64_MAX - 31}) {
        auto Actual = machineExitState(0x800000);
        Actual.write(NdVar::reg(x86reg::RCX, 8), Stack);
        Actual.memory(Stack + Cleanup, 0x12340, 8);
        auto Expected = Actual;
        Expected.memory(Stack - 8, Continuation, 8);
        Expected.write(NdVar::reg(x86reg::RAX, 8), Continuation);
        Expected.write(NdVar::reg(x86reg::RDX, 8),
                       Expected.read(NdVar::reg(x86reg::RDX, 8)) + 7);
        Expected.write(NdVar::reg(x86reg::RSP, 8), Stack + Cleanup + 8);
        ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
        expectMachineExitState(Actual, Expected, 0x12340);
      }
    }
}

TEST_F(PEFixedImageTest, MachineRegionUnrootedReturnFollowsActualStackWord) {
  for (uint16_t Cleanup : {uint16_t{0}, uint16_t{24}}) {
    auto Loaded = load(unrootedStackFixture(false, Cleanup, true));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Base + 0x1064, MachineExitKind::Return);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_EQ(R.ExternalExits.size(), 1U);
    EXPECT_EQ(R.ExternalExits.front().Original.Origin.Address, Base + 0x1064);
    auto Actual = machineExitState(0x800000);
    Actual.write(NdVar::reg(x86reg::RCX, 8), 0x900000);
    Actual.memory(0x900000 + Cleanup, 0x12340, 8);
    auto Expected = Actual;
    Expected.memory(0x8ffff8, Base + 0x1060, 8);
    Expected.write(NdVar::reg(x86reg::RAX, 8), Base + 0x1060);
    Expected.write(NdVar::reg(x86reg::RDX, 8),
                   Expected.read(NdVar::reg(x86reg::RDX, 8)) + 9);
    Expected.write(NdVar::reg(x86reg::RSP, 8), 0x900008 + Cleanup);
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    expectMachineExitState(Actual, Expected, 0x12340);
  }
}

TEST_F(PEFixedImageTest, MachineRegionUnrootedTransfersKeepUnknownTargets) {
  const std::vector<std::vector<uint8_t>> Images{
      unrootedStackFixture(false, 0, false, true),
      machineExitFixture({0x48, 0x89, 0xcc, 0xff, 0xd0}),
      machineExitFixture({0x48, 0x89, 0xcc, 0xc3})};
  for (const auto &Image : Images) {
    auto Loaded = load(Image);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Base + 0x1008, MachineExitKind::Return);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_FALSE(R.complete());
    EXPECT_EQ(R.Status, SpecializationStatus::UnresolvedControl)
        << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_TRUE(R.ExternalExits.empty());
  }
}

TEST_F(PEFixedImageTest, MachineRegionUnrootedNestedCallsReadBothActualSlots) {
  std::vector<uint8_t> Code{0x48, 0x89, 0xcc, 0xe8, 0x38, 0, 0, 0, 0xc3};
  Code.resize(0x40, 0xcc);
  Code.insert(Code.end(), {0xe8, 0x1b, 0, 0, 0, 0x48, 0x8b, 0x04, 0x24, 0xc3});
  Code.resize(0x60, 0xcc);
  Code.insert(Code.end(), {0x48, 0x8b, 0x14, 0x24, 0xc3});
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  for (uint32_t Chaining : {0u, 1024u}) {
    auto O = machineExitOptions(Base + 0x1008, MachineExitKind::Return);
    O.MaxChainedTransfers = Chaining;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    for (uint64_t Stack : {uint64_t{0x800000}, uint64_t{0x900000}}) {
      auto Actual = machineExitState(0x800000);
      Actual.write(NdVar::reg(x86reg::RCX, 8), Stack);
      Actual.memory(Stack, 0x12340, 8);
      auto Expected = Actual;
      Expected.memory(Stack - 8, Base + 0x1008, 8);
      Expected.memory(Stack - 16, Base + 0x1045, 8);
      Expected.write(NdVar::reg(x86reg::RAX, 8), Base + 0x1008);
      Expected.write(NdVar::reg(x86reg::RDX, 8), Base + 0x1045);
      Expected.write(NdVar::reg(x86reg::RSP, 8), Stack + 8);
      ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
      expectMachineExitState(Actual, Expected, 0x12340);
    }
  }
}

TEST_F(PEFixedImageTest, MachineRegionUnrootedSharedCalleeKeepsBothCallers) {
  // jrcxz second; mov rsp,rdx; call helper; ret. Both callers use the same
  // physical helper and different return words on a rebased stack.
  std::vector<uint8_t> Code{0xe3, 0x1e, 0x48, 0x89, 0xd4, 0xe8,
                            0x36, 0,    0,    0,    0xc3};
  Code.resize(0x20, 0xcc);
  Code.insert(Code.end(), {0x48, 0x89, 0xd4, 0xe8, 0x18, 0, 0, 0, 0xc3});
  Code.resize(0x40, 0xcc);
  Code.push_back(0xc3);
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x100a, MachineExitKind::Return);
  O.X64MachineExits.push_back({1, Base + 0x1028, MachineExitKind::Return});
  O.MaxChainedTransfers = 0;
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.ExternalExits.size(), 2U);
  for (uint64_t Choice : {uint64_t{0}, uint64_t{1}}) {
    const auto Continuation = Base + (Choice ? 0x100a : 0x1028);
    auto Actual = machineExitState(0x800000);
    Actual.write(NdVar::reg(x86reg::RCX, 8), Choice);
    Actual.write(NdVar::reg(x86reg::RDX, 8), 0x900000);
    Actual.memory(0x900000, 0x12340, 8);
    auto Expected = Actual;
    Expected.memory(0x8ffff8, Continuation, 8);
    Expected.write(NdVar::reg(x86reg::RSP, 8), 0x900008);
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    expectMachineExitState(Actual, Expected, 0x12340);
  }
}

TEST_F(PEFixedImageTest, MachineRegionUnrootedProjectionKeepsPushedReturnCell) {
  auto Loaded = load(unrootedStackFixture(false, 0));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1008, MachineExitKind::Return);
  O.MaxChainedTransfers = 0;
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.ExternalExits.size(), 1U);
  for (uint64_t Stack :
       {uint64_t{0}, uint64_t{0x800000}, uint64_t{0x900000}, UINT64_MAX - 31}) {
    auto Actual = machineExitState(0x800000);
    Actual.write(NdVar::reg(x86reg::RCX, 8), Stack);
    Actual.memory(Stack, 0x12340, 8);
    auto Expected = Actual;
    Expected.memory(Stack - 8, Base + 0x1008, 8);
    Expected.write(NdVar::reg(x86reg::RAX, 8), Base + 0x1008);
    Expected.write(NdVar::reg(x86reg::RDX, 8),
                   Expected.read(NdVar::reg(x86reg::RDX, 8)) + 7);
    Expected.write(NdVar::reg(x86reg::RSP, 8), Stack + 8);
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    expectMachineExitState(Actual, Expected, 0x12340);
  }
}

TEST_F(PEFixedImageTest, MachineRegionUnrootedDisjointWriteKeepsReturnCell) {
  std::vector<uint8_t> Code{0x48, 0x89, 0xcc, 0xe8, 0x38, 0, 0, 0, 0xc3};
  Code.resize(0x40, 0xcc);
  // mov byte ptr [rsp-9],dl; ret. The write misses the pushed word.
  Code.insert(Code.end(), {0x88, 0x54, 0x24, 0xf7, 0xc3});
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1008, MachineExitKind::Return);
  O.MaxChainedTransfers = 0;
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Stack : {uint64_t{0}, uint64_t{0x900000}}) {
    auto Actual = machineExitState(0x800000);
    Actual.write(NdVar::reg(x86reg::RCX, 8), Stack);
    Actual.memory(Stack, 0x12340, 8);
    auto Expected = Actual;
    Expected.memory(Stack - 8, Base + 0x1008, 8);
    Expected.memory(Stack - 17,
                    Expected.read(NdVar::reg(x86reg::RDX, 8)) & 0xff, 1);
    Expected.write(NdVar::reg(x86reg::RSP, 8), Stack + 8);
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    expectMachineExitState(Actual, Expected, 0x12340);
  }
}

TEST_F(PEFixedImageTest, MachineRegionUnrootedProjectionRejectsSlotOverwrite) {
  std::vector<uint8_t> Partial{0x48, 0x89, 0xcc, 0xe8, 0x38, 0, 0, 0, 0xc3};
  Partial.resize(0x40, 0xcc);
  // A single changed byte of the physical return word must revoke the fact.
  Partial.insert(Partial.end(), {0x88, 0x54, 0x24, 0x01, 0xc3});
  for (const auto &Image : {unrootedStackFixture(false, 0, false, true),
                            machineExitFixture(Partial)}) {
    auto Loaded = load(Image);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Base + 0x1008, MachineExitKind::Return);
    O.MaxChainedTransfers = 0;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_EQ(R.Status, SpecializationStatus::UnresolvedControl)
        << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_TRUE(R.ExternalExits.empty());
  }
}

TEST_F(PEFixedImageTest, UnrootedStackKeepsOrdinaryFunctionRequirements) {
  auto Loaded = load(unrootedStackFixture(false, 0));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1008, MachineExitKind::Return);
  O.X64MachineExits.clear();
  O.RequireRestoredFrameAtReturn = true;
  O.X64CodeRegions.push_back(
      {1, 0x400000, {0xc3}, "unused raw-domain control"});
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  EXPECT_FALSE(R.complete());
  EXPECT_NE(R.Diagnostic.find("no exact entry-relative stack pointer"),
            std::string::npos);
}

TEST_F(PEFixedImageTest, MachineRegionUnrootedCallsChargeExactWork) {
  auto Loaded = load(unrootedStackFixture(true, 24));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1016, MachineExitKind::Return);
  const auto Baseline = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Baseline.complete()) << Baseline.Diagnostic;
  O.MaxOperations = Baseline.EvaluatedOperations;
  EXPECT_TRUE(
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, O).complete());
  --O.MaxOperations;
  const auto Refused = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  EXPECT_EQ(Refused.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Refused.Residual.Blocks.empty());
  EXPECT_TRUE(Refused.ExternalExits.empty());
}
