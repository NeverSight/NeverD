//===- RecoveryArchitectureTests.inc - Native contract ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Compatibility and architecture identity controls for raw receipts.
// Included after RawCodeEnvironmentTests.inc in the fixed-PE test namespace.

TEST_F(PEFixedImageTest, RawArchitectureReceiptCompatibility) {
  // Recorded with the original v2 option/exit encoding and v1 receipts.
  // A policy relocation must not silently introduce a new evidence schema.
  const char *Expected[2][4] = {
      {"8A628DDB2C445F9B90E3D418427FEF909E803C5FBCF8510332FAB5B78F987AB4",
       "7861421693CF948D345001BCD3AA1D124808C9BBBDC81E486C1E09ED5C4FE42E",
       "6C5C194454FC2CB4E228D17B8C78215E004221325FE1F0F8343EEC0F7A3D2F90",
       "6B46EE514A8722629C878D5D997267576CBEDC3F756808BB49B066D3769E9273"},
      {"49F6411DFB8D107606A87C7D5BA1D9CBC2A9899048150A9F076D2D3A45FC9CAB",
       "48AEFB5CEE6E967105756FBD787E27907B0E1D86841CE8E8772C42C9ECD68185",
       "1D1C92C1638AF650C8D35E694FA72B059D22D83AC4E7E12C616BBE0210C20924",
       "C95AB4D1A1663A1279ECB4E77EE8A3513F6BF3D5D06B6CA85D862DD9B1EDA770"},
  };
  for (unsigned Case = 0; Case != 2; ++Case) {
    SCOPED_TRACE(Case);
    auto Loaded = load(Case ? machineExitFixture({0xff, 0xd0})
                            : returningImportFixture());
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = Case ? machineExitOptions(Base + 0x1000, MachineExitKind::Call)
                  : returningRawOptions(0x400000);
    O.X64SegmentedReads = true;
    O.X64CR8State = true;
    O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
    const auto R = specializeBinaryInterpreter(
        *Loaded, Base + (Case ? 0x1000 : 0x1100), O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_TRUE(R.RawEnvironment);
    auto Error = validateSpecializationRawEnvironment(R, O);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    const auto &Receipt = *R.RawEnvironment;
    EXPECT_EQ(Receipt.Domain->DomainDigest, Expected[Case][0]);
    EXPECT_EQ(Receipt.GraphDigest, Expected[Case][1]);
    EXPECT_EQ(Receipt.EvidenceDigest, Expected[Case][2]);
    EXPECT_EQ(Receipt.IdentityDigest, Expected[Case][3]);
    if (Case) {
      ASSERT_EQ(R.ExternalExits.size(), 1U);
      EXPECT_EQ(
          R.ExternalExits.front().IdentityDigest,
          "EE472CFCD612E99A2757A1A64E49ECF7BA1FB08D3592FACC51B8A3341091B5E5");
    }
  }
}

TEST_F(PEFixedImageTest, RawArchitectureCannotRelabelEvidence) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto O = machineExitOptions(Base + 0x1000, MachineExitKind::Call);
  const auto Good = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  ASSERT_TRUE(Good.RawEnvironment);
  for (Arch Target : {Arch::AArch64, Arch::X86, Arch::ARM, Arch::EVM, Arch::SBF,
                      Arch::Unknown, static_cast<Arch>(255)}) {
    SCOPED_TRACE(static_cast<unsigned>(Target));
    auto Changed = O;
    Changed.Architecture = Target;
    for (bool Raw : {false, true}) {
      auto Error = Raw ? validateSpecializationRawEnvironment(Good, Changed)
                       : validateSpecializationExternalExits(Good, Changed);
      ASSERT_TRUE(bool(Error));
      EXPECT_NE(llvm::toString(std::move(Error)).find("architecture"),
                std::string::npos);
    }
    auto R = Good;
    auto Domain = std::make_shared<SpecializationRawEnvironment>(
        *Good.RawEnvironment->Domain);
    Domain->Declaration.Architecture = Target;
    // Stored authority cannot select the policy or enlarge its walk budget.
    Domain->Declaration.MaxImagePreparationBytes = UINT64_MAX;
    R.RawEnvironment->Domain = Domain;
    auto Error = validateSpecializationRawEnvironment(R, O);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("expected architecture"),
              std::string::npos);
    Error = validateSpecializationRawEnvironment(R, Changed);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("architecture"),
              std::string::npos);
    const auto Mismatch =
        specializeBinaryInterpreter(*Loaded, Base + 0x1000, Changed);
    EXPECT_EQ(Mismatch.Status, SpecializationStatus::InvalidInput);
    EXPECT_EQ(Mismatch.EvaluatedOperations, 0U);
    EXPECT_TRUE(Mismatch.Residual.Blocks.empty());
    EXPECT_FALSE(Mismatch.RawEnvironment);
    EXPECT_NE(Mismatch.Diagnostic.find("architecture"), std::string::npos);
  }
  auto Explicit = O;
  Explicit.Architecture = Arch::X64;
  auto Error = validateSpecializationRawEnvironment(Good, Explicit);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}

TEST_F(PEFixedImageTest, RawSegmentedWriteContractBindsUnusedCapability) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1000, MachineExitKind::Call);
  const auto Before = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Before.complete()) << Before.Diagnostic;
  ASSERT_TRUE(Before.RawEnvironment);
  O.X64SegmentedWrites = true;
  const auto After = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(After.complete()) << After.Diagnostic;
  ASSERT_TRUE(After.RawEnvironment);
  EXPECT_NE(Before.RawEnvironment->Domain->DomainDigest,
            After.RawEnvironment->Domain->DomainDigest);
  auto Error = validateSpecializationRawEnvironment(After, O);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = validateSpecializationRawEnvironment(Before, O);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  O.X64SegmentedWrites = false;
  Error = validateSpecializationRawEnvironment(After, O);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST_F(PEFixedImageTest, RawMXCSRContractBindsBothWritableMasks) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1000, MachineExitKind::Call);
  const auto Before = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Before.complete()) << Before.Diagnostic;
  std::set<std::string> Digests{Before.RawEnvironment->Domain->DomainDigest};
  for (uint32_t Mask : {0xffbfU, 0xffffU}) {
    O.X64MXCSRMask = Mask;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_TRUE(R.RawEnvironment);
    EXPECT_TRUE(Digests.insert(R.RawEnvironment->Domain->DomainDigest).second);
    auto Error = validateSpecializationRawEnvironment(R, O);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    Error = validateSpecializationRawEnvironment(Before, O);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    O.X64MXCSRMask = Mask ^ 0x40;
    Error = validateSpecializationRawEnvironment(R, O);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST_F(PEFixedImageTest,
       RawLegacyGSContractBindsUnusedStateAndExecutionProfile) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1000, MachineExitKind::Call);
  O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
  const auto Before = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Before.complete()) << Before.Diagnostic;
  ASSERT_TRUE(Before.RawEnvironment);
  O.X64LegacyGSState = true;
  const auto After = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(After.complete()) << After.Diagnostic;
  ASSERT_TRUE(After.RawEnvironment);
  EXPECT_NE(Before.RawEnvironment->Domain->DomainDigest,
            After.RawEnvironment->Domain->DomainDigest);
  auto Error = validateSpecializationRawEnvironment(After, O);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = validateSpecializationRawEnvironment(Before, O);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  O.X64LegacyGSState = false;
  Error = validateSpecializationRawEnvironment(After, O);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST_F(PEFixedImageTest, RawLegacyGSNativeExchangeRequiresTheExplicitDomain) {
  auto Loaded = load(machineExitFixture({0x0f, 0x01, 0xf8, 0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = machineExitOptions(Base + 0x1003, MachineExitKind::Return);
  O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
  EXPECT_FALSE(
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, O).complete());
  O.X64LegacyGSState = true;
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  unsigned Exchanges = 0;
  for (const auto &Block : R.Residual.Blocks)
    for (const auto &Op : Block.Ops)
      Exchanges += Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 1 &&
                   Op.Inputs[0] == NdVar::cst(uint64_t(Intrinsic::Swapgs), 2);
  EXPECT_EQ(Exchanges, 1U);
  ASSERT_TRUE(R.RawEnvironment);
  auto Error = validateSpecializationRawEnvironment(R, O);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}

TEST_F(PEFixedImageTest, RawMXCSRNativeLoadsRetainReservedBitProofs) {
  for (bool Vex : {false, true})
    for (bool Invalid : {false, true}) {
      // mov dword ptr [rbx], immediate; (v)ldmxcsr [rbx]; ret.
      std::vector<uint8_t> Code{
          0xc7, 0x03, 0x80, 0x1f, uint8_t(Invalid ? 1 : 0), 0};
      if (Vex)
        Code.insert(Code.end(), {0xc5, 0xf8, 0xae, 0x13});
      else
        Code.insert(Code.end(), {0x0f, 0xae, 0x13});
      const auto Exit = Base + 0x1000 + Code.size();
      Code.push_back(0xc3);
      auto Loaded = load(machineExitFixture(Code));
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      auto O = machineExitOptions(Exit, MachineExitKind::Return);
      EXPECT_FALSE(
          specializeBinaryInterpreter(*Loaded, Base + 0x1000, O).complete());
      O.X64MXCSRMask = 0xffbf;
      const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
      if (Invalid) {
        EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
        EXPECT_NE(R.Diagnostic.find("reserved bits"), std::string::npos);
        EXPECT_TRUE(R.Residual.Blocks.empty());
        continue;
      }
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      auto Error = validateSpecializationRawEnvironment(R, O);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      unsigned Loads = 0;
      for (const auto &Block : R.Residual.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
              Op.Inputs[0].Offset == uint64_t(Intrinsic::Ldmxcsr))
            ++Loads;
      EXPECT_EQ(Loads, 1U);
    }
}

TEST_F(PEFixedImageTest, RawSegmentedWriteCannotPreserveAliasedNativeReturn) {
  // CALL reaches the store at +11; the outer RET at +10 is a declared exit.
  // A fixed, arbitrary segment base can place the store on the internal return
  // word, even though RBX itself has external entry provenance.
  const std::vector<uint8_t> Code{0xe8, 6, 0,    0,    0,    0xb8, 7,    0,
                                  0,    0, 0xc3, 0x65, 0x48, 0x89, 0x0b, 0xc3};
  for (bool Store : {false, true}) {
    auto Bytes = Code;
    if (!Store)
      std::fill(Bytes.begin() + 11, Bytes.begin() + 15, 0x90);
    auto Loaded = load(machineExitFixture(Bytes));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Base + 0x100a, MachineExitKind::Return);
    O.X64SegmentedWrites = true;
    O.ProveStoreReturnSlotSeparation = true;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    if (!Store) {
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      auto Error = validateSpecializationRawEnvironment(R, O);
      EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    } else {
      EXPECT_FALSE(R.complete());
      EXPECT_NE(R.Diagnostic.find("return address"), std::string::npos)
          << R.Diagnostic;
      EXPECT_TRUE(R.Residual.Blocks.empty());
      EXPECT_FALSE(R.RawEnvironment);
    }
  }
}

TEST_F(PEFixedImageTest, RawSegmentedStoreRetainsEveryScalarWidth) {
  const std::vector<std::vector<uint8_t>> Stores{{0x65, 0x88, 0x0b},
                                                 {0x65, 0x66, 0x89, 0x0b},
                                                 {0x65, 0x89, 0x0b},
                                                 {0x65, 0x48, 0x89, 0x0b}};
  for (unsigned I = 0; I != Stores.size(); ++I) {
    auto Code = Stores[I];
    const auto Exit = Base + 0x1000 + Code.size();
    Code.push_back(0xc3);
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = machineExitOptions(Exit, MachineExitKind::Return);
    O.X64SegmentedReads = true;
    EXPECT_FALSE(
        specializeBinaryInterpreter(*Loaded, Base + 0x1000, O).complete());
    O.X64SegmentedWrites = true;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    auto Error = validateSpecializationRawEnvironment(R, O);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    unsigned Count = 0;
    for (const auto &B : R.Residual.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::STORE) {
          ++Count;
          EXPECT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::X86GS);
          EXPECT_EQ(lowMemoryOperands(Op).AccessSize, 1U << I);
        }
    EXPECT_EQ(Count, 1U);
  }
}

TEST_F(PEFixedImageTest, RecoveryArchitectureSelectionPrecedesProvider) {
  struct UnreachableProvider final : SpecializationProvider {
    mutable unsigned DomainReads = 0;
    unsigned Fetches = 0;
    std::shared_ptr<const SpecializationRawEnvironment>
    rawEnvironment() const override {
      ++DomainReads;
      return {};
    }
    llvm::Expected<SpecializationInstruction>
    instruction(SpecializationCursor) override {
      ++Fetches;
      return llvm::createStringError(llvm::errc::not_supported,
                                     "unexpected instruction fetch");
    }
  } P;
  for (Arch Target : {Arch::AArch64, Arch::X86, Arch::ARM, Arch::EVM, Arch::SBF,
                      Arch::Unknown, static_cast<Arch>(255)})
    for (bool Raw : {false, true}) {
      auto O = Raw ? rawOptions() : SpecializationOptions{};
      O.Architecture = Target;
      const auto R = specializeInterpreter(P, {Base + 0x1000}, O);
      EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
      EXPECT_NE(R.Diagnostic.find("architecture"), std::string::npos);
      EXPECT_EQ(R.EvaluatedOperations, 0U);
      EXPECT_TRUE(R.Residual.Blocks.empty());
      EXPECT_TRUE(R.ExternalExits.empty());
      EXPECT_FALSE(R.RawEnvironment);
      EXPECT_EQ(P.DomainReads, 0U);
      EXPECT_EQ(P.Fetches, 0U);
    }
}

TEST_F(PEFixedImageTest, RawAlignedMovesRequireAProofWithoutMXCSRState) {
  for (unsigned Mode = 0; Mode != 4; ++Mode) {
    SCOPED_TRACE(Mode);
    std::vector<uint8_t> Code;
    if (Mode != 1)
      Code = {0x48, 0x83, 0xe3, 0xf0}; // and rbx,-16
    if (Mode == 2)
      Code.insert(Code.end(), {0x48, 0x83, 0xcb, 0x01}); // or rbx,1
    if (Mode == 3)
      Code.push_back(0x64); // FS still has no admitted alignment base.
    const auto MoveAddress = Base + 0x1000 + Code.size();
    Code.insert(Code.end(), {0x66, 0x0f, 0x6f, 0x03}); // movdqa xmm0,[rbx]
    const auto Exit = Base + 0x1000 + Code.size();
    Code.push_back(0xc3);
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto Options = machineExitOptions(Exit, MachineExitKind::Return);
    Options.X64SegmentedReads = Mode == 3;
    ASSERT_FALSE(Options.X64MXCSRMask);
    const auto Result =
        specializeBinaryInterpreter(*Loaded, Base + 0x1000, Options);
    if (Mode != 0) {
      EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported)
          << Result.Diagnostic;
      EXPECT_TRUE(Result.Residual.Blocks.empty());
      EXPECT_FALSE(Result.RawEnvironment);
      continue;
    }
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    ASSERT_TRUE(Result.RawEnvironment);
    auto Valid = validateSpecializationRawEnvironment(Result, Options);
    ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    va_t ResidualMove = InvalidVA;
    uint32_t NativeOpCount = 0;
    for (const auto &Origin : Result.Origins)
      if (Origin.NativeInstruction.Address == MoveAddress) {
        ASSERT_EQ(ResidualMove, InvalidVA);
        ResidualMove = Origin.ResidualAddress;
        NativeOpCount = Origin.NativeInstruction.OpCount;
      }
    ASSERT_NE(ResidualMove, InvalidVA);
    unsigned Guards = 0, Loads = 0;
    for (const auto &Block : Result.Residual.Blocks) {
      for (const auto &Boundary : Block.InstructionBoundaries)
        if (Boundary.Address == ResidualMove)
          EXPECT_EQ(Boundary.OpCount, NativeOpCount);
      bool GuardBeforeLoad = false;
      for (const auto &Op : Block.Ops) {
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].Offset == uint64_t(Intrinsic::RequireAligned)) {
          ++Guards;
          EXPECT_EQ(Op.NumInputs, 3U);
          EXPECT_EQ(Op.Inputs[2], NdVar::scalar(16, 8));
          EXPECT_EQ(Op.Addr, ResidualMove);
          GuardBeforeLoad = true;
        }
        if (Op.Opcode == NdOp::LOAD && Op.Output.Size == 16) {
          ++Loads;
          EXPECT_TRUE(GuardBeforeLoad);
          EXPECT_EQ(Op.Addr, ResidualMove);
        }
      }
    }
    EXPECT_EQ(Guards, 1U);
    EXPECT_EQ(Loads, 1U);
    Options.MaxOperations = Result.EvaluatedOperations;
    EXPECT_TRUE(specializeBinaryInterpreter(*Loaded, Base + 0x1000, Options)
                    .complete());
    ASSERT_GT(Options.MaxOperations, 1U);
    --Options.MaxOperations;
    const auto Short =
        specializeBinaryInterpreter(*Loaded, Base + 0x1000, Options);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.Residual.Blocks.empty());
    EXPECT_FALSE(Short.RawEnvironment);
  }
}

TEST_F(PEFixedImageTest, RawAlignmentReceiptBindsRetainedGuard) {
  const std::vector<uint8_t> Code = {0x48, 0x83, 0xe3, 0xf0, 0x66,
                                     0x0f, 0x6f, 0x03, 0xc3};
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto Options =
      machineExitOptions(Base + 0x1008, MachineExitKind::Return);
  const auto Good =
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, Options);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
    auto Changed = Good;
    unsigned Guards = 0;
    for (auto &Block : Changed.Residual.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].Offset == uint64_t(Intrinsic::RequireAligned)) {
          ++Guards;
          if (Mutation == 0)
            Op.Inputs[2] = NdVar::scalar(32, 8);
          else
            Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
        }
    ASSERT_EQ(Guards, 1U);
    auto Error = validateSpecializationRawEnvironment(Changed, Options);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  auto Valid = validateSpecializationRawEnvironment(Good, Options);
  EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
}
