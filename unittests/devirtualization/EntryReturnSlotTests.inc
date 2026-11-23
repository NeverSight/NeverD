//===- EntryReturnSlotTests.inc - Contextual native returns ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent fixed-image checks for the explicit caller return word.
// Included after ReturningCallEnvironmentTests.inc in the fixed-PE namespace.

SpecializationOptions entryReturnOptions() {
  auto O = machineExitOptions(Base + 0x1010, MachineExitKind::Trap);
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-64, 8};
  O.EntryReturnSlot = SpecializationFrameSlot{0, 8};
  return O;
}

size_t entryReturnOperations(const SpecializationResult &R) {
  size_t Count = 0;
  for (const auto &B : R.Residual.Blocks)
    for (const auto &Op : B.Ops)
      Count += Op.Opcode == NdOp::RETURN;
  return Count;
}

TEST_F(PEFixedImageTest, EntryReturnSlotSeparatesSamePhysicalRetByContext) {
  // call ret; jmp ret; ret. The first RET consumes the pushed continuation at
  // entry RSP-8. The second executes the same byte at entry RSP.
  auto Loaded = load(machineExitFixture({0xe8, 2, 0, 0, 0, 0xeb, 0, 0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = entryReturnOptions();
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_TRUE(R.RawEnvironment);
  EXPECT_EQ(entryReturnOperations(R), 1U);
  size_t PhysicalReturns = 0;
  for (const auto &Origin : R.Origins)
    PhysicalReturns += Origin.NativeInstruction.Address == Base + 0x1007;
  EXPECT_EQ(PhysicalReturns, 2U);
  auto Valid = validateSpecializationRawEnvironment(R, O);
  EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  auto Stale = O;
  Stale.EntryReturnSlot.reset();
  auto Mismatch = validateSpecializationRawEnvironment(R, Stale);
  EXPECT_TRUE(bool(Mismatch));
  llvm::consumeError(std::move(Mismatch));
  const auto Undeclared =
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, Stale);
  EXPECT_FALSE(Undeclared.complete());
  EXPECT_TRUE(Undeclared.Residual.Blocks.empty());
}

TEST_F(PEFixedImageTest, EntryReturnSlotTracksWritesByPhysicalExtent) {
  const std::vector<std::pair<std::vector<uint8_t>, bool>> Cases = {
      {{0x48, 0x89, 0x44, 0x24, 0xf0, 0xc3}, true}, // [rsp-16] = rax
      {{0xc6, 0x04, 0x24, 0, 0xc3}, false},         // [rsp] = 0
      {{0x48, 0x89, 0x01, 0xc3}, false},            // [rcx] = rax
  };
  for (const auto &[Code, ExpectedComplete] : Cases) {
    SCOPED_TRACE(Code.size());
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = entryReturnOptions();
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_EQ(R.complete(), ExpectedComplete) << R.Diagnostic;
    if (ExpectedComplete)
      EXPECT_EQ(entryReturnOperations(R), 1U);
    else
      EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST_F(PEFixedImageTest, EntryReturnSlotSurvivesDeclaredReturningCall) {
  auto Loaded = load(machineExitFixture({0x48, 0x83, 0xec, 0x40, // sub rsp,64
                                         0xff, 0xd0,             // call rax
                                         0x48, 0x83, 0xc4, 0x40, // add rsp,64
                                         0xc3}));                // ret
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = entryReturnOptions();
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-128, 8};
  O.X64ReturningCalls = {
      {1, Base + 0x1004,
       SpecializationX64CallProfile::ConditionalNormalReturnV1, 32}};
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(R.ReturningCalls.size(), 1U);
  EXPECT_EQ(entryReturnOperations(R), 1U);
  auto Valid = validateSpecializationRawEnvironment(R, O);
  EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
}

TEST_F(PEFixedImageTest, EntryReturnSlotRequiresCompleteX64FrameWord) {
  auto Loaded = load(machineExitFixture({0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  for (unsigned Variant = 0; Variant < 5; ++Variant) {
    auto O = entryReturnOptions();
    switch (Variant) {
    case 0:
      O.EntryReturnSlot->Offset = 1;
      break;
    case 1:
      O.EntryReturnSlot->Bytes = 4;
      break;
    case 2:
      O.EntryFrameBounds->End = 7;
      break;
    case 3:
      O.X64MachineExits.clear();
      break;
    case 4:
      O.Architecture = Arch::AArch64;
      break;
    }
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_FALSE(R.complete()) << Variant;
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}
