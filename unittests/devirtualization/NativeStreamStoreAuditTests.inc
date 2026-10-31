//===- NativeStreamStoreAuditTests.inc - Streaming store contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Byte-authenticated stores and independent finite-memory checks.

TEST(NativeStateAudit, StreamingStoreRetainsAlignmentAndNativeMemoryEffect) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (const auto &Bytes :
         std::vector<std::vector<uint8_t>>{{0x66, 0x0f, 0xe7, 0x00},
                                           {0x66, 0x0f, 0xe7, 0x54, 0x24, 0x10},
                                           {0x67, 0x66, 0x0f, 0xe7, 0x00}}) {
      SCOPED_TRACE(::testing::PrintToString(Bytes));
      Decoder D;
      ASSERT_TRUE(D.init(A));
      D.setStrict(true);
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, I),
                int(Bytes.size()));
      ASSERT_TRUE(vectormoveaudit::form(I.Raw, A));
      std::vector<LowOp> Ops;
      LowInstructionUndefinedEffects Effects;
      LowInstructionPreservedState Preserved;
      D.liftToLow(I, Ops, {}, {}, &Effects, &Preserved);
      EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
      EXPECT_TRUE(Effects.Effects.empty());
      EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
      if (A == Arch::X64)
        EXPECT_EQ(Preserved.Audit, LowPreservedStateAudit::LegacyIntegerV1);
      unsigned Align = 0, Store = 0;
      for (const auto &Op : Ops) {
        EXPECT_NE(Op.Opcode, NdOp::STORE);
        EXPECT_NE(Op.Opcode, NdOp::LOAD);
        if (Op.Opcode != NdOp::INTRINSIC)
          continue;
        Align += Op.Inputs[0].Offset == uint64_t(Intrinsic::RequireAligned);
        if (Op.Inputs[0].Offset == uint64_t(Intrinsic::X86StreamStore128)) {
          ++Store;
          EXPECT_TRUE(x86SystemStateShapeIsValid(
              Intrinsic::X86StreamStore128, x86SystemStateLowShape(Op, A)));
          EXPECT_EQ(Op.Inputs[2].Size, 16);
          EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86StreamStore128));
        }
      }
      EXPECT_EQ(Align, 1u);
      EXPECT_EQ(Store, 1u);
      for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
        cs_insn Raw = *I.Raw;
        cs_detail Detail = *Raw.detail;
        Raw.detail = &Detail;
        if (!Mutation)
          Detail.x86.operands[0].access = CS_AC_READ;
        else if (Mutation == 1)
          Detail.x86.operands[1].access = CS_AC_WRITE;
        else if (Mutation == 2)
          Detail.x86.operands[0].size = 8;
        else if (Mutation == 3)
          Raw.bytes[Raw.size - 1] ^= 1;
        else
          Detail.x86.eflags = X86_EFLAGS_MODIFY_CF;
        EXPECT_FALSE(vectormoveaudit::form(&Raw, A));
      }
    }
}

LowOp streamStore(uint64_t Address) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(NdVar::cst(uint64_t(Intrinsic::X86StreamStore128), 2));
  Op.addInput(NdVar::cst(Address, 8));
  Op.addInput(NdVar::reg(x86reg::vectorReg(0), 16));
  return Op;
}

TEST(NativeStateAudit, StreamingStoreFiniteProjectionIsWholeAndAligned) {
  for (bool Strict : {false, true})
    for (unsigned Offset : {0u, 1u, 15u, 16u, 32u, 48u}) {
      SCOPED_TRACE(Offset);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Segment Data;
      Data.VA = MemoryBase;
      Data.Size = Data.FileSz = 40;
      Data.Data.assign(40, char(0xa5));
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Image.Segments.push_back(std::move(Data));
      NdOpEmulator E(Image);
      E.setStrictMode(Strict);
      std::vector<uint8_t> Value(16);
      for (unsigned I = 0; I != 16; ++I)
        Value[I] = uint8_t(I * 17 + Offset);
      E.setRegisterBytes(x86reg::vectorReg(0), Value);
      const bool Fits = Offset == 0 || Offset == 16;
      EXPECT_EQ(E.step(streamStore(MemoryBase + Offset)), Fits);
      // Observe every mapped byte through ordinary loads after the attempted
      // store. A rejected or partially mapped access must leave all bytes.
      for (unsigned I = 0; I != 40; ++I) {
        LowOp Read;
        Read.Opcode = NdOp::LOAD;
        Read.Output = NdVar::reg(x86reg::RAX, 1);
        Read.addInput(NdVar::cst(MemoryBase + I, 8));
        ASSERT_TRUE(E.step(Read));
        EXPECT_EQ(E.getRegister(x86reg::RAX),
                  Fits && I >= Offset && I < Offset + 16 ? Value[I - Offset]
                                                         : 0xa5u);
      }
      EXPECT_EQ(E.getRegisterBytes(x86reg::vectorReg(0)), Value);
    }
}

TEST(NativeStateAudit, StreamingStoreEvaluationRequiresExactShapeAndDomain) {
  SpecializationOptions Options;
  Options.ExplicitMachineState = Options.NormalNonfaultingExecution = true;
  const auto Good = streamStore(MemoryBase);
  const auto Scalar = retainedStoreEvaluation(Good, Options);
  ASSERT_TRUE(Scalar);
  EXPECT_EQ(Scalar->Opcode, NdOp::STORE);
  EXPECT_EQ(Scalar->NumInputs, Good.NumInputs);
  EXPECT_EQ(Scalar->Inputs[1], Good.Inputs[1]);
  EXPECT_EQ(Scalar->Inputs[2], Good.Inputs[2]);
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    auto Bad = Good;
    auto Domain = Options;
    switch (Mutation) {
    case 0:
      Bad.Inputs[0].Offset += UINT64_C(1) << 16;
      break;
    case 1:
      Bad.Inputs[0].Size = 1;
      break;
    case 2:
      Bad.Output = NdVar::reg(0, 8);
      break;
    case 3:
      Bad.Inputs[1].Size = 4;
      break;
    case 4:
      Bad.Inputs[2].Size = 8;
      break;
    case 5:
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 6:
      Bad.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 7:
      Domain.NormalNonfaultingExecution = false;
      break;
    case 8:
      Domain.ExplicitMachineState = false;
      break;
    case 9:
      Domain.Architecture = Arch::AArch64;
      break;
    case 10:
      Domain.ByteOrder = llvm::endianness::big;
      break;
    }
    EXPECT_FALSE(retainedStoreEvaluation(Bad, Domain)) << Mutation;
  }
}

TEST(NativeStateAudit, StreamingStoreFenceRequiresExactNativeAndScalarForms) {
  const std::vector<uint8_t> Bytes{0x0f, 0xae, 0xf8};
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setStrict(true);
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, I), 3);
  ASSERT_TRUE(nativestateaudit::memoryFenceForm(I.Raw, Arch::X64));
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  D.liftToLow(I, Ops, {}, {}, &Effects, &Preserved);
  ASSERT_EQ(Ops.size(), 1u);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Preserved.Audit, LowPreservedStateAudit::LegacyIntegerV1);
  SpecializationOptions Options;
  Options.ExplicitMachineState = Options.NormalNonfaultingExecution = true;
  ASSERT_TRUE(x64::streamstore::supportsFence(Ops.front(), Options));
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    auto Raw = *I.Raw;
    auto Detail = *Raw.detail;
    Raw.detail = &Detail;
    if (!Mutation)
      Raw.id = X86_INS_LFENCE;
    else if (Mutation == 1)
      Raw.bytes[2] = 0xe8;
    else if (Mutation == 2)
      Detail.x86.modrm = 0xe8;
    else if (Mutation == 3)
      Detail.x86.op_count = 1;
    else if (Mutation == 4)
      Detail.x86.eflags = X86_EFLAGS_MODIFY_CF;
    else
      Detail.x86.prefix[2] = 0x66;
    EXPECT_FALSE(nativestateaudit::memoryFenceForm(&Raw, Arch::X64));
    auto Bad = Ops.front();
    if (!Mutation)
      Bad.Inputs[0].Offset += UINT64_C(1) << 16;
    else if (Mutation == 1)
      Bad.Inputs[0].Size = 1;
    else if (Mutation == 2)
      Bad.Output = NdVar::reg(x86reg::RAX, 8);
    else if (Mutation == 3)
      Bad.addInput(NdVar::cst(0, 8));
    else if (Mutation == 4)
      Bad.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    else
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    EXPECT_FALSE(x64::streamstore::supportsFence(Bad, Options));
  }
  Options.NormalNonfaultingExecution = false;
  EXPECT_FALSE(x64::streamstore::supportsFence(Ops.front(), Options));
}
