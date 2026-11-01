//===- NativeCompareExchangeAuditTests.inc - Scalar CMPXCHG receipts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact encoding admission, one ordered transaction, and refusal controls.
//===----------------------------------------------------------------------===//

TEST(NativeStateAudit, LockedCompareExchangeDefinesFlagsAndOneTransaction) {
  for (unsigned Width : {1u, 2u, 4u, 8u})
    for (unsigned Source = 0; Source != 16; ++Source)
      for (bool RexByte : {false, true}) {
        std::vector<uint8_t> Bytes{0xf0};
        if (Width == 2)
          Bytes.push_back(0x66);
        if (Width == 8 || Source >= 8 || RexByte)
          Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) | (Source >= 8 ? 4 : 0));
        Bytes.insert(Bytes.end(), {0x0f, uint8_t(Width == 1 ? 0xb0 : 0xb1),
                                   uint8_t(((Source & 7) << 3) | 7)});
        SCOPED_TRACE(::testing::PrintToString(Bytes));
        const auto Ops = lift(Bytes);
        unsigned Count = 0;
        for (const auto &Op : Ops) {
          EXPECT_NE(Op.Opcode, NdOp::LOAD);
          EXPECT_NE(Op.Opcode, NdOp::STORE);
          if (Op.Opcode != NdOp::ATOMIC_CMPXCHG)
            continue;
          ++Count;
          EXPECT_EQ(Op.MemoryOrdering,
                    NdMemoryOrdering::SequentiallyConsistent);
          EXPECT_EQ(Op.NumInputs, 3);
          EXPECT_EQ(Op.Output.Size, Width);
          EXPECT_EQ(Op.Inputs[1], NdVar::reg(x86reg::RAX, Width));
          EXPECT_EQ(Op.Inputs[2].Size, Width);
        }
        EXPECT_EQ(Count, 1u);
      }
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0x66, 0xf0, 0x44, 0x0f, 0xb1, 0x83, 0xec, 6, 0, 0},
           {0x65, 0xf0, 0x48, 0x0f, 0xb1, 0x54, 0x8b, 0xf8},
           {0x64, 0x67, 0xf0, 0x0f, 0xb1, 0x17},
           {0xf0, 0x0f, 0xb1, 0x15, 0x78, 0x56, 0x34, 0x12},
           {0xf0, 0x0f, 0xb0, 0x24, 0x25, 0, 0x20, 0, 0}})
    EXPECT_FALSE(lift(Bytes).empty());
}

TEST(NativeStateAudit, LockedCompareExchangeRejectsRawDetailDisagreement) {
  using Mutate = void (*)(cs_insn &);
  const Mutate Mutations[] = {
      [](cs_insn &I) { I.bytes[0] = 0x64; },
      [](cs_insn &I) { I.bytes[1] = 0xf2; },
      [](cs_insn &I) { I.bytes[2] = 0x4c; },
      [](cs_insn &I) { I.bytes[4] = 0xb0; },
      [](cs_insn &I) { I.bytes[5] ^= 8; },
      [](cs_insn &I) { I.bytes[6] ^= 8; },
      [](cs_insn &I) { I.bytes[7] ^= 1; },
      [](cs_insn &I) { --I.size; },
      [](cs_insn &I) { I.id = X86_INS_CMPXCHG8B; },
      [](cs_insn &I) { I.detail->x86.op_count = 1; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0; },
      [](cs_insn &I) { I.detail->x86.prefix[1] = 0; },
      [](cs_insn &I) { I.detail->x86.rex = 0; },
      [](cs_insn &I) { I.detail->x86.opcode[1] = 0xc1; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.modrm ^= 8; },
      [](cs_insn &I) { I.detail->x86.sib ^= 8; },
      [](cs_insn &I) { I.detail->x86.disp = 0; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset = 0; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_offset = 0; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_size = 0; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_size = 1; },
      [](cs_insn &I) { I.detail->x86.eflags ^= X86_EFLAGS_MODIFY_CF; },
      [](cs_insn &I) { I.detail->x86.avx_sae = true; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[0].access = CS_AC_READ; },
      [](cs_insn &I) { I.detail->x86.operands[0].access = CS_AC_WRITE; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.segment = X86_REG_FS; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.base = X86_REG_RDX; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.index = X86_REG_RDX; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.scale = 8; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.disp = 0; },
      [](cs_insn &I) { I.detail->x86.operands[1].size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[1].access = CS_AC_WRITE; },
      [](cs_insn &I) { I.detail->x86.operands[1].reg = X86_REG_RCX; },
      [](cs_insn &I) { I.detail->x86.operands[1].avx_zero_opmask = true; }};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (unsigned M = 0; M != std::size(Mutations); ++M) {
    SCOPED_TRACE(M);
    const uint8_t Bytes[] = {0x65, 0xf0, 0x48, 0x0f, 0xb1, 0x54, 0x8b, 0xf8};
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn),
              int(sizeof(Bytes)));
    EXPECT_EQ(Insn.Raw->detail->x86.operands[0].access, CS_AC_READ_WRITE);
    EXPECT_EQ(Insn.Raw->detail->x86.operands[1].access, CS_AC_READ);
    ASSERT_TRUE(lockedcompareexchangeaudit::form(Insn.Raw, Arch::X64))
        << "destination access: "
        << unsigned(Insn.Raw->detail->x86.operands[0].access)
        << ", source access: "
        << unsigned(Insn.Raw->detail->x86.operands[1].access);
    Mutations[M](*Insn.Raw);
    EXPECT_FALSE(lockedcompareexchangeaudit::form(Insn.Raw, Arch::X64));
    expectUnaudited(Dec, Insn);
  }
}

TEST(NativeStateAudit, LockedCompareExchangeKeepsUnauditedFormsClosed) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x0f, 0xb1, 0x17},
                                         {0x48, 0x0f, 0xb1, 0xd7},
                                         {0xf0, 0x0f, 0xc7, 0x0f},
                                         {0xf0, 0x48, 0x0f, 0xc7, 0x0f},
                                         {0xf0, 0xf0, 0x0f, 0xb1, 0x17},
                                         {0xf2, 0xf0, 0x0f, 0xb1, 0x17},
                                         {0xf3, 0xf0, 0x0f, 0xb1, 0x17},
                                         {0x66, 0x66, 0xf0, 0x0f, 0xb1, 0x17},
                                         {0x48, 0xf0, 0x0f, 0xb1, 0x17}}) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    DecodedInsn Insn{};
    if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn) !=
        int(Bytes.size()))
      continue; // Decoder rejection also keeps malformed forms closed.
    EXPECT_FALSE(lockedcompareexchangeaudit::form(Insn.Raw, Arch::X64));
    expectUnaudited(Dec, Insn);
  }
}
