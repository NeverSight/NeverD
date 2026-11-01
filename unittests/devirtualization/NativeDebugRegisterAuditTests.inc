//===- NativeDebugRegisterAuditTests.inc - Debug register receipts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native debug moves have six arbitrary flags and exact register operands.
//===----------------------------------------------------------------------===//

TEST(X86NativeStateAudit, DebugTransfersKeepNativeEffectsAndSixArbitraryFlags) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (uint8_t Bank : {0, 1, 2, 3, 6, 7})
    for (uint8_t Register = 0; Register != 16; ++Register)
      for (uint8_t Prefix : {0, 0x40, 0x48})
        for (bool Write : {false, true}) {
          if (!Prefix && Register >= 8)
            continue;
          std::vector<uint8_t> Bytes;
          if (Prefix)
            Bytes.push_back(Prefix | (Register >= 8 ? 1 : 0));
          Bytes.insert(Bytes.end(),
                       {0x0f, uint8_t(Write ? 0x23 : 0x21),
                        uint8_t(0xc0 | (Bank << 3) | (Register & 7))});
          SCOPED_TRACE(::testing::PrintToString(Bytes));
          DecodedInsn Insn{};
          ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                         CodeAddress, Insn),
                    int(Bytes.size()));
          ASSERT_TRUE(registermoveundefined::form(Insn.Raw, Arch::X64));
          EXPECT_FALSE(registermoveundefined::form(Insn.Raw, Arch::X86));
          std::vector<LowOp> Ops, Plain;
          LowInstructionUndefinedEffects Effects;
          LowInstructionPreservedState Preserved;
          Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
          Dec.liftToLow(Insn, Plain);
          EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
          EXPECT_EQ(Effects.Effects.size(), 6u);
          EXPECT_TRUE(registermoveundefined::matches(Ops, Effects));
          EXPECT_EQ(Effects.OperationDigest,
                    lowUndefinedOperationDigest(Plain));
          EXPECT_EQ(Preserved, LowInstructionPreservedState{});
          ASSERT_EQ(Ops.size(), 1u);
          EXPECT_EQ(Ops[0].Opcode, NdOp::INTRINSIC);
          EXPECT_EQ(
              Ops[0].Inputs[0],
              NdVar::cst(
                  uint64_t(Write ? Intrinsic::WriteDr : Intrinsic::ReadDr), 2));
          EXPECT_EQ(Ops[0].Inputs[1], NdVar::scalar(Bank, 4));
          EXPECT_EQ(Write ? Ops[0].Inputs[2] : Ops[0].Output,
                    NdVar::reg(Register * 8, 8));
        }
}

TEST(X86NativeStateAudit, DebugAuditRejectsAliasesAndRawDetailDisagreement) {
  using Mutate = void (*)(cs_insn &);
  const Mutate Mutations[] = {
      [](cs_insn &I) { I.bytes[0] = 0x44; },
      [](cs_insn &I) { I.bytes[1] = 0x0e; },
      [](cs_insn &I) { I.bytes[2] = 0x20; },
      [](cs_insn &I) { I.bytes[3] = 0xc1; },
      [](cs_insn &I) { --I.size; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](cs_insn &I) { I.detail->x86.rex = 0x40; },
      [](cs_insn &I) { I.detail->x86.op_count = 1; },
      [](cs_insn &I) { I.detail->x86.modrm = 0xc1; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset = 0; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_size = 1; },
      [](cs_insn &I) { I.detail->x86.avx_sae = true; },
      [](cs_insn &I) { I.detail->x86.operands[0].reg = X86_REG_R9; },
      [](cs_insn &I) { I.detail->x86.operands[1].reg = X86_REG_DR1; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[1].size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[1].avx_zero_opmask = true; }};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (unsigned M = 0; M != std::size(Mutations); ++M) {
    SCOPED_TRACE(M);
    const uint8_t Bytes[] = {0x41, 0x0f, 0x21, 0xc0};
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn), 4);
    ASSERT_TRUE(registermoveundefined::form(Insn.Raw, Arch::X64));
    Mutations[M](*Insn.Raw);
    EXPECT_FALSE(registermoveundefined::form(Insn.Raw, Arch::X64));
    expectUnaudited(Dec, Insn);
  }
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x0f, 0x21, 0xe0},
                                         {0x0f, 0x23, 0xe8},
                                         {0x66, 0x0f, 0x21, 0xc0},
                                         {0x40, 0x41, 0x0f, 0x21, 0xc0}}) {
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
        int(Bytes.size()));
    EXPECT_FALSE(registermoveundefined::form(Insn.Raw, Arch::X64));
    expectUnaudited(Dec, Insn);
  }
}
