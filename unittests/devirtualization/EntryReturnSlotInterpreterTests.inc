//===- EntryReturnSlotInterpreterTests.inc - Caller word semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Exact native-byte provider controls for contextual return handling.
// Included after ReturningCallStateTests.inc.

SpecializationOptions entryReturnStateOptions() {
  auto O = returningStateOptions();
  O.X64ReturningCalls.clear();
  O.X64MachineExits = {{1, 0x200, SpecializationX64MachineExitKind::Trap}};
  O.EntryReturnSlot = SpecializationFrameSlot{0, 8};
  O.MaxChainedTransfers = 32;
  return O;
}

TEST(InterpreterSpecialization, EntryReturnSameRetHasInternalAndOuterContexts) {
  ReturningCallProvider P;
  P.native(0x100, {0xe8, 2, 0, 0, 0}); // call 0x107
  P.native(0x105, {0xeb, 0});          // jmp 0x107
  P.native(0x107, {0xc3});             // both internal and outer
  auto O = entryReturnStateOptions();
  auto Prepared = P.prepare(O);
  ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::RETURN), 1U);
  size_t Returns = 0;
  for (const auto &Origin : R.Origins)
    Returns += Origin.NativeInstruction.Address == 0x107;
  EXPECT_EQ(Returns, 2U);
  auto Valid = validateSpecializationRawEnvironment(R, O);
  EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  auto Mismatch = O;
  Mismatch.EntryReturnSlot.reset();
  auto Error = validateSpecializationRawEnvironment(R, Mismatch);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  O.EntryReturnSlot.reset();
  Prepared = P.prepare(O);
  ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
  const auto Without = specializeInterpreter(P, {0x100}, O);
  EXPECT_FALSE(Without.complete());
  EXPECT_TRUE(Without.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, EntryReturnWordCanSurviveADeclaredCall) {
  ReturningCallProvider P;
  P.native(0x100, {0x48, 0x83, 0xec, 0x40}); // sub rsp,64
  P.native(0x104, {0xff, 0xd0});             // call rax
  P.native(0x106, {0x48, 0x83, 0xc4, 0x40}); // add rsp,64
  P.native(0x10a, {0xc3});                   // ret
  auto O = entryReturnStateOptions();
  O.X64ReturningCalls = {
      {1, 0x104, SpecializationX64CallProfile::ConditionalNormalReturnV1, 32}};
  auto Prepared = P.prepare(O);
  ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(R.ReturningCalls.size(), 1U);
  EXPECT_EQ(count(R.Residual, NdOp::RETURN), 1U);
}

TEST(InterpreterSpecialization,
     EntryReturnWordFallsInsideUnallocatedOutgoingArea) {
  ReturningCallProvider P;
  P.native(0x100, {0xff, 0xd0}); // no shadow-space allocation
  P.native(0x102, {0xc3});
  auto O = entryReturnStateOptions();
  O.X64ReturningCalls = {
      {1, 0x100, SpecializationX64CallProfile::ConditionalNormalReturnV1, 32}};
  auto Prepared = P.prepare(O);
  ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
  const auto R = specializeInterpreter(P, {0x100}, O);
  EXPECT_FALSE(R.complete());
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, EntryReturnWordRejectsOverlappingWrite) {
  for (unsigned Variant = 0; Variant < 3; ++Variant) {
    ReturningCallProvider P;
    if (Variant == 1) {
      P.native(0x100, {0xc6, 0x04, 0x24, 0}); // mov byte [rsp],0
      P.native(0x104, {0xc3});                // ret
    } else if (Variant == 2) {
      P.native(0x100, {0x48, 0x89, 0x01}); // mov [rcx],rax
      P.native(0x103, {0xc3});             // ret
    } else {
      P.native(0x100, {0x48, 0x89, 0x44, 0x24, 0xf0}); // [rsp-16]=rax
      P.native(0x105, {0xc3});                         // ret
    }
    auto O = entryReturnStateOptions();
    auto Prepared = P.prepare(O);
    ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(R.complete(), Variant == 0) << R.Diagnostic;
    if (Variant)
      EXPECT_TRUE(R.Residual.Blocks.empty());
    else
      EXPECT_EQ(count(R.Residual, NdOp::RETURN), 1U);
  }
}

TEST(InterpreterSpecialization,
     EntryReturnWordSeparatesIntactAndReplacedPaths) {
  ReturningCallProvider P;
  P.native(0x100, {0x85, 0xc0}); // test eax,eax
  P.native(0x102, {0x74, 0x02}); // jz 0x106
  P.native(0x104, {0xeb, 0x0e}); // jmp 0x114
  P.native(0x106, {0x48, 0xba, 0x15, 0x01, 0, 0, 0, 0, 0, 0});
  P.native(0x110, {0x48, 0x89, 0x14, 0x24}); // [rsp] = 0x115
  P.native(0x114, {0xc3});                   // shared RET
  P.native(0x115, {0xcc});                   // replaced path ends in trap
  P.Code.at(0x115).Origin.Control = LowInstructionControl::Terminator;
  P.Code.at(0x115).Origin.ControlFlags = LowInstructionControlFlag::Terminator |
                                         LowInstructionControlFlag::Resumable;
  auto O = entryReturnStateOptions();
  O.X64MachineExits = {{1, 0x115, SpecializationX64MachineExitKind::Trap}};
  auto Prepared = P.prepare(O);
  ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::RETURN), 1U);
  ASSERT_EQ(R.ExternalExits.size(), 1U);
  EXPECT_EQ(R.ExternalExits.front().Original.Origin.Address, 0x115U);
  size_t Returns = 0;
  for (const auto &Origin : R.Origins)
    Returns += Origin.NativeInstruction.Address == 0x114;
  EXPECT_EQ(Returns, 2U);
}

TEST(InterpreterSpecialization, EntryReturnSlotRejectsMalformedContracts) {
  ReturningCallProvider P;
  for (unsigned Variant = 0; Variant < 4; ++Variant) {
    auto O = entryReturnStateOptions();
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
    }
    auto Error = P.prepare(O);
    EXPECT_TRUE(bool(Error)) << Variant;
    llvm::consumeError(std::move(Error));
  }
}
