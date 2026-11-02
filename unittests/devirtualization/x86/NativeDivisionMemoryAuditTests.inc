//===- NativeDivisionMemoryAuditTests.inc - Division state audits --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Register and memory division admission, effect ordering and raw-byte
/// binding.
//===----------------------------------------------------------------------===//

TEST(X86NativeStateAudit, DivisionMemoryBindsEveryAddressEncoding) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Target));
    Dec.setStrict(true);
    for (unsigned Width : {1u, 2u, 4u, 8u}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (bool Signed : {false, true})
        for (bool AddressPrefix : {false, true})
          for (uint8_t Segment : {0, 0x64, 0x65})
            for (unsigned Mod = 0; Mod != 3; ++Mod)
              for (unsigned RM = 0; RM != 8; ++RM)
                for (unsigned Extension = 0; Extension != 4; ++Extension) {
                  if (Target == Arch::X86 && Extension)
                    continue;
                  const bool Address16 = Target == Arch::X86 && AddressPrefix;
                  const unsigned SibCount = !Address16 && RM == 4 ? 256 : 1;
                  for (unsigned Sib = 0; Sib != SibCount; ++Sib) {
                    std::vector<uint8_t> Bytes;
                    if (Segment)
                      Bytes.push_back(Segment);
                    if (Width == 2)
                      Bytes.push_back(0x66);
                    if (AddressPrefix)
                      Bytes.push_back(0x67);
                    if (Width == 8 || Extension)
                      Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) | Extension);
                    Bytes.push_back(Width == 1 ? 0xf6 : 0xf7);
                    Bytes.push_back((Mod << 6) | (Signed ? 0x38 : 0x30) | RM);
                    unsigned DispSize = Mod == 1   ? 1
                                        : Mod == 2 ? (Address16 ? 2 : 4)
                                                   : 0;
                    if (SibCount != 1) {
                      Bytes.push_back(Sib);
                      if (!Mod && (Sib & 7) == 5)
                        DispSize = 4;
                    } else if (!Mod && RM == (Address16 ? 6u : 5u)) {
                      DispSize = Address16 ? 2 : 4;
                    }
                    for (unsigned I = 0; I != DispSize; ++I)
                      Bytes.push_back(0x80); // Negative at every encoded width.
                    SCOPED_TRACE(unsigned(Target));
                    SCOPED_TRACE(Width);
                    SCOPED_TRACE(Mod);
                    SCOPED_TRACE(RM);
                    SCOPED_TRACE(Sib);
                    SCOPED_TRACE(Extension);
                    SCOPED_TRACE(AddressPrefix);
                    DecodedInsn Insn{};
                    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                                   CodeAddress, Insn),
                              int(Bytes.size()));
                    std::vector<LowOp> Ops;
                    LowInstructionUndefinedEffects Effects;
                    Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
                    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
                        << Effects.Diagnostic;
                    ASSERT_EQ(Effects.Effects.size(), 6u);
                    const auto Read = llvm::find_if(Ops, [](const LowOp &Op) {
                      return Op.Opcode == NdOp::LOAD;
                    });
                    ASSERT_NE(Read, Ops.end());
                    EXPECT_EQ(Read->Output.Size, Width);
                    EXPECT_EQ(Read->MemoryAddressSpace,
                              Segment == 0x64 ? NdMemoryAddressSpace::X86FS
                              : Segment == 0x65
                                  ? NdMemoryAddressSpace::X86GS
                                  : NdMemoryAddressSpace::Default);
                    // No architectural write may precede the operand read or
                    // the division precondition. A failed access cannot publish
                    // a quotient, remainder or any of the six arbitrary flags.
                    const auto Guard = llvm::find_if(Ops, [](const LowOp &Op) {
                      return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                             Op.Inputs[0].Offset ==
                                 unsigned(Intrinsic::X86RequireDivPrecondition);
                    });
                    ASSERT_NE(Guard, Ops.end());
                    EXPECT_LT(Read, Guard);
                    for (auto I = Ops.begin(); I != Guard; ++I)
                      EXPECT_FALSE(I->Output.isReg());
                    for (const auto &Effect : Effects.Effects)
                      EXPECT_GT(Effect.AfterOp, size_t(Guard - Ops.begin()));
                  }
                }
    }
  }
}

TEST(X86NativeStateAudit, DivisionMemoryRejectsForgedAddressDetails) {
  using Mutation = void (*)(cs_insn &);
  const Mutation Mutations[] = {
      [](cs_insn &I) { I.detail->x86.operands[0].mem.base = X86_REG_RAX; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.index = X86_REG_RDX; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.scale = 2; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.disp++; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.segment = X86_REG_FS; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 4; },
      [](cs_insn &I) { I.detail->x86.sib ^= 1; },
      [](cs_insn &I) { I.detail->x86.disp++; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_offset++; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_size = 4; },
      [](cs_insn &I) { I.bytes[4] ^= 8; },
      [](cs_insn &I) { I.bytes[5]++; }};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (unsigned M = 0; M != std::size(Mutations); ++M) {
    SCOPED_TRACE(M);
    const uint8_t Bytes[] = {0x65, 0x4b, 0xf7, 0x74, 0xa5, 0xf9};
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn),
              int(sizeof(Bytes)));
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
    Mutations[M](*Insn.Raw);
    expectUnaudited(Dec, Insn);
  }
}

TEST(X86NativeStateAudit, DivisionBindsFlagsAfterSuccessfulCommit) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Target));
    Dec.setStrict(true);
    for (bool Signed : {false, true})
      for (unsigned Width : {1u, 2u, 4u, 8u}) {
        if (Target == Arch::X86 && Width == 8)
          continue;
        const unsigned Count = Target == Arch::X64 ? 16 : 8;
        for (unsigned Register = 0; Register != Count; ++Register)
          for (bool ByteRex : {false, true}) {
            if (ByteRex && (Target != Arch::X64 || Width != 1 || Register >= 8))
              continue;
            SCOPED_TRACE(unsigned(Target));
            SCOPED_TRACE(Width);
            SCOPED_TRACE(Register);
            SCOPED_TRACE(Signed);
            SCOPED_TRACE(ByteRex);
            std::vector<uint8_t> Bytes;
            if (Width == 2)
              Bytes.push_back(0x66);
            if (Width == 8 || Register >= 8 || ByteRex)
              Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) | (Register >= 8));
            Bytes.push_back(Width == 1 ? 0xf6 : 0xf7);
            Bytes.push_back((Signed ? 0xf8 : 0xf0) | (Register & 7));
            DecodedInsn Insn{};
            ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                           CodeAddress, Insn),
                      int(Bytes.size()));
            std::vector<LowOp> Ops;
            LowInstructionUndefinedEffects Effects;
            LowInstructionPreservedState Preserved;
            Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
            ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
                << Effects.Diagnostic;
            EXPECT_EQ(Effects.OpCount, Ops.size());
            EXPECT_EQ(Effects.OperationDigest,
                      lowUndefinedOperationDigest(Ops));
            ASSERT_EQ(Effects.Effects.size(), 6u);
            const unsigned Undefined[] = {x86reg::OF, x86reg::SF, x86reg::ZF,
                                          x86reg::AF, x86reg::PF, x86reg::CF};
            for (unsigned I = 0; I != 6; ++I) {
              EXPECT_EQ(Effects.Effects[I].Output, NdVar::reg(Undefined[I], 1));
              EXPECT_EQ(Effects.Effects[I].BitOffset, 0u);
              EXPECT_EQ(Effects.Effects[I].BitCount, 1u);
              EXPECT_FALSE(Effects.Effects[I].When);
              const auto After = Effects.Effects[I].AfterOp;
              ASSERT_GT(After, 0u);
              ASSERT_LE(After, Ops.size());
              // The x64 zero-extension epilogue may follow this point, but
              // no division or other potentially faulting effect may remain.
              for (const auto &Later : llvm::ArrayRef(Ops).drop_front(After)) {
                EXPECT_EQ(Later.Opcode, NdOp::INT_ZEXT);
                EXPECT_TRUE(Later.Output == NdVar::reg(x86reg::RAX, 8) ||
                            Later.Output == NdVar::reg(x86reg::RDX, 8));
              }
            }
            EXPECT_EQ(Preserved, LowInstructionPreservedState{});
          }
      }
  }
}

TEST(X86NativeStateAudit, DivisionReadsActualHighHalfAfterPriorLifts) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {2u, 4u, 8u}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (bool Signed : {false, true})
        for (bool Memory : {false, true})
          for (unsigned Prior = 0; Prior != 3; ++Prior) {
            SCOPED_TRACE(unsigned(Target));
            SCOPED_TRACE(Width);
            SCOPED_TRACE(Signed);
            SCOPED_TRACE(Memory);
            SCOPED_TRACE(Prior);
            Decoder Dec;
            ASSERT_TRUE(Dec.init(Target));
            Dec.setStrict(true);
            std::vector<uint8_t> Clear, Divide;
            if (Width != 4) {
              Clear.push_back(Width == 2 ? 0x66 : 0x48);
              Divide.push_back(Clear.back());
            }
            if (Signed)
              Clear.push_back(0x99); // CWD, CDQ or CQO in a prior lift.
            else
              Clear.insert(Clear.end(), {0x31, 0xd2});
            Divide.insert(Divide.end(), {0xf7, uint8_t((Memory ? 0x31 : 0xf1) +
                                                       (Signed ? 8 : 0))});
            std::vector<LowOp> Ignored;
            DecodedInsn Insn{};
            if (Prior) {
              ASSERT_EQ(Dec.decodeOneForLift(Clear.data(), Clear.size(),
                                             CodeAddress, Insn),
                        int(Clear.size()));
              Dec.liftToLow(Insn, Ignored);
            }
            ASSERT_EQ(Dec.decodeOneForLift(Divide.data(), Divide.size(),
                                           CodeAddress + Clear.size(), Insn),
                      int(Divide.size()));
            if (Prior == 2)
              Dec.liftToLow(Insn, Ignored);
            std::vector<LowOp> Ops;
            LowInstructionUndefinedEffects Effects;
            Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
            ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
            auto Image = image();
            Image.Arch = Target;
            Image.Bits =
                Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
            for (uint64_t Divisor : {0, 1, 3}) {
              if (Memory)
                for (unsigned B = 0; B != Width; ++B)
                  Image.Segments[0].Data[B] = uint8_t(Divisor >> (B * 8));
              NdOpEmulator Emulator(Image);
              Emulator.setStrictMode(true);
              StateOracle Expected(Emulator);
              Expected.setRegister(Emulator, x86reg::RAX, 13);
              Expected.setRegister(Emulator, x86reg::RDX, 1);
              Expected.setRegister(Emulator, x86reg::RCX,
                                   Memory ? MemoryBase : Divisor);
              const auto Completed = Emulator.run(Ops);
              if (Divisor > 1) {
                EXPECT_EQ(Completed, Ops.size());
                // (2^(8*Width) + 13) / 3, with remainder 2.
                Expected.Registers[0] = Width == 8
                                            ? UINT64_C(6148914691236517209)
                                        : Width == 4 ? UINT64_C(1431655769)
                                                     : UINT64_C(21849);
                Expected.Registers[2] = 2;
              } else {
                EXPECT_LT(Completed, Ops.size());
              }
              // The sidecar owns arbitrary flags. The concrete representative
              // leaves every other bank unchanged, including on refusal.
              Expected.expect(Emulator);
              if (Memory)
                expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
            }
          }
    }
}

TEST(X86NativeStateAudit, DivisionRejectsUnauditedFormsAndForgedDetails) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  using Mutation = void (*)(cs_insn &);
  const Mutation Mutations[] = {
      [](cs_insn &I) { I.id = X86_INS_IDIV; },
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { I.bytes[0] = 0x40; },
      [](cs_insn &I) { I.bytes[1] = 0xf6; },
      [](cs_insn &I) { I.bytes[2] = 0xf9; },
      [](cs_insn &I) { I.detail->x86.op_count = 2; },
      [](cs_insn &I) { I.detail->x86.modrm ^= 1; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset = 1; },
      [](cs_insn &I) { I.detail->x86.opcode[0] ^= 1; },
      [](cs_insn &I) { I.detail->x86.opcode[1] = 1; },
      [](cs_insn &I) { I.detail->x86.rex ^= 1; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](cs_insn &I) { I.detail->x86.prefix[1] = 0x65; },
      [](cs_insn &I) { I.detail->x86.prefix[2] = 0x66; },
      [](cs_insn &I) { I.detail->x86.prefix[3] = 0x67; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.sib = 1; },
      [](cs_insn &I) { I.detail->x86.disp = 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_offset = 2; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_size = 1; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_offset = 2; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_size = 1; },
      [](cs_insn &I) { I.detail->x86.avx_sae = true; },
      [](cs_insn &I) { I.detail->x86.avx_rm = X86_AVX_RM_RN; },
      [](cs_insn &I) { I.detail->x86.avx_cc = X86_AVX_CC_EQ; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[0].reg = X86_REG_RDX; },
      [](cs_insn &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
      [](cs_insn &I) {
        I.detail->x86.operands[0].avx_bcast = X86_AVX_BCAST_2;
      }};
  for (unsigned I = 0; I != std::size(Mutations); ++I) {
    SCOPED_TRACE(I);
    const uint8_t Bytes[] = {0x48, 0xf7, 0xf1};
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn), 3);
    Mutations[I](*Insn.Raw);
    expectUnaudited(Dec, Insn);
  }
  const std::vector<std::vector<uint8_t>> Forms = {{0x66, 0x48, 0xf7, 0xf1},
                                                   {0x67, 0x48, 0xf7, 0xf1},
                                                   {0x48, 0x48, 0xf7, 0xf1}};
  for (const auto &Bytes : Forms) {
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
        int(Bytes.size()));
    expectUnaudited(Dec, Insn);
  }
}
