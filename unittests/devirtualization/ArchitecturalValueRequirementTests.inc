//===- ArchitecturalValueRequirementTests.inc - Retained choice guards ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Ordinary consumers must not approximate retained bit productions.
// Included inside RawEnvironmentRequirementTests.cpp's test namespace.

TEST(ArchitecturalValueRequirement, OrdinaryEnginesCannotInventAValue) {
  LowUndefinedEffect Effect;
  Effect.Output = NdVar::reg(0, 8);
  Effect.BitCount = 64;
  const auto Made = makeLowArchitecturalValue(Effect, 0x100);
  ASSERT_TRUE(Made);
  std::vector<LowOp> Ops{*Made};
  for (auto Bad : malformedMarkers()) {
    Bad.Inputs[0].Offset = uint64_t(Intrinsic::ArchitecturalValueV1) |
                           (Bad.Inputs[0].Offset & ~uint64_t{UINT16_MAX});
    Ops.push_back(Bad);
  }
  for (const auto &Op : Ops) {
    for (bool Strict : {false, true}) {
      BinaryImage Image;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setRegister(0, 17);
      EXPECT_FALSE(Emulator.step(Op));
      EXPECT_EQ(Emulator.getRegister(0), 17U);
      EXPECT_EQ(Emulator.skips().ApproximatedOps, 0U);
      EXPECT_EQ(Emulator.run(std::vector<LowOp>{Op, copy()}), 0U);
      EXPECT_EQ(Emulator.getRegister(0), 17U);
    }
    SymContext Ctx;
    SymState State(Ctx);
    State.write(SymSpace::Register, 0, Ctx.mkConst(64, 17));
    SymExec Exec(Ctx, State);
    EXPECT_THROW(Exec.step(Op), std::invalid_argument);
    EXPECT_THROW(Exec.run(std::vector<LowOp>{Op, copy()}),
                 std::invalid_argument);
    const auto Value = Ctx.asConst(State.read(SymSpace::Register, 0, 8));
    ASSERT_TRUE(Value);
    EXPECT_EQ(Value->getZExtValue(), 17U);
    EXPECT_EQ(Exec.unmodelledCount(), 0U);
    EXPECT_EQ(Exec.opaqueOperationCount(), 0U);
    EXPECT_THROW(LowToMedConverter().convert(function(Op), Arch::X64),
                 LowToMedConversionError);
    auto Source = wrapInterpreterMachineStateX64(
        function(Op), BinaryFormat::ELF,
        InterpreterMachineStateProfile::UserX64NoFaultV1);
    EXPECT_FALSE(bool(Source));
    if (!Source)
      EXPECT_NE(llvm::toString(Source.takeError()).find("choice-source"),
                std::string::npos);
  }
  for (auto A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    bool HasIntrinsics = false;
    EXPECT_THROW(renderIntrinsicCall(Intrinsic::ArchitecturalValueV1, A, {}, 8,
                                     HasIntrinsics),
                 std::invalid_argument);
    EXPECT_FALSE(HasIntrinsics);
    EXPECT_THROW(
        MultiOutputRender{}(A, Intrinsic::ArchitecturalValueV1, {}, {}, {}, {}),
        std::invalid_argument);
  }
}

TEST(ArchitecturalValueRequirement, StaticProofRefusesUnreachableProduction) {
  const auto Ordinary = function(copy());
  std::vector<LowIRUndefinedInstruction> Records;
  for (const auto &Block : Ordinary.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      LowIRUndefinedInstruction Record;
      Record.BlockId = Block.Id;
      Record.Boundary = Boundary;
      Record.Effects.Coverage = LowUndefinedCoverage::Complete;
      Record.Effects.OpCount = Boundary.OpCount;
      Record.Effects.OperationDigest = lowUndefinedOperationDigest(
          llvm::ArrayRef(Block.Ops).slice(Boundary.FirstOp, Boundary.OpCount));
      Records.push_back(Record);
    }
  LowIRIndependenceContract Contract;
  Contract.ReturnRegisters = {{0, 8}};
  const auto Control =
      checkLowIRUndefinedIndependence(Ordinary, Records, Contract);
  ASSERT_TRUE(Control.proved()) << Control.Diagnostic;
  auto Raw = Ordinary;
  addUnreachableRequirement(Raw);
  Raw.Blocks.back().Ops.front().Inputs[0] =
      NdVar::scalar(uint64_t(Intrinsic::ArchitecturalValueV1), 8);
  const auto Proof = checkLowIRUndefinedIndependence(Raw, Records, Contract);
  EXPECT_EQ(Proof.Status, LowIRIndependenceStatus::Unsupported);
  EXPECT_FALSE(Proof.Certificate);
  const auto RelationControl =
      checkLowIRRefinement(Ordinary, Records, Ordinary, Contract);
  ASSERT_TRUE(RelationControl.proved()) << RelationControl.Diagnostic;
  for (bool Original : {false, true}) {
    const auto Relation =
        checkLowIRRefinement(Original ? Raw : Ordinary, Records,
                             Original ? Ordinary : Raw, Contract);
    EXPECT_EQ(Relation.Status, LowIRRefinementStatus::Unsupported);
    EXPECT_FALSE(Relation.Certificate);
  }
}

TEST(ArchitecturalValueRequirement, DirectLLVMRefusesWideAndMalformedIds) {
  for (uint64_t Id :
       {uint64_t(Intrinsic::ArchitecturalValueV1),
        uint64_t(Intrinsic::ArchitecturalValueV1) + (uint64_t{1} << 32)})
    for (unsigned Shape = 0; Shape != 4; ++Shape) {
      MedFunc F;
      F.Entry = 0x100;
      F.Name = "architectural_value_guard";
      F.CC = CallingConv::SysV_AMD64;
      F.ReturnType = NdType::makeVoid();
      MedBlock B;
      B.Id = 0;
      B.StartAddr = F.Entry;
      B.EndAddr = F.Entry + 2;
      MedOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.Addr = F.Entry;
      Op.addInput(MedVar::makeConst(Id, 8));
      if (Shape == 1)
        Op.NumInputs = 0;
      else if (Shape == 2)
        Op.Inputs.front().Kind = MedVar::Reg;
      else if (Shape == 3)
        Op.NumInputs = 255;
      B.Ops.push_back(Op);
      Op = {};
      Op.Opcode = NdOp::RETURN;
      Op.Addr = F.Entry + 1;
      B.Ops.push_back(Op);
      F.Blocks.push_back(B);
      for (bool Masked : {false, true})
        EXPECT_DEATH(
            {
              llvm::LLVMContext Ctx;
              const std::vector<char> Mask{0};
              (void)MedLLVMEmitter().emit({F}, Ctx, F.Name, Arch::X64, {},
                                          nullptr, BinaryFormat::ELF, false,
                                          Masked ? &Mask : nullptr);
            },
            "raw environment requirement");
    }
}
