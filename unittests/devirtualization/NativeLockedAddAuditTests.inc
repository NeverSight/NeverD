//===- NativeLockedAddAuditTests.inc - Scalar LOCK ADD receipts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// ADD admission shares the raw locked binary memory encoding authority.
//===----------------------------------------------------------------------===//

TEST(NativeStateAudit, LockedAddDefinesFlagsAndOneTransaction) {
  for (unsigned Width : {1u, 2u, 4u, 8u})
    for (unsigned Source = 0; Source != 16; ++Source) {
      std::vector<uint8_t> Bytes{0xf0};
      if (Width == 2)
        Bytes.push_back(0x66);
      if (Width == 8 || Source >= 8)
        Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) | (Source >= 8 ? 4 : 0));
      Bytes.insert(Bytes.end(), {uint8_t(Width == 1 ? 0 : 1),
                                 uint8_t(((Source & 7) << 3) | 7)});
      SCOPED_TRACE(::testing::PrintToString(Bytes));
      const auto Ops = lift(Bytes);
      unsigned Count = 0;
      for (const auto &Op : Ops) {
        EXPECT_NE(Op.Opcode, NdOp::LOAD);
        EXPECT_NE(Op.Opcode, NdOp::STORE);
        if (Op.Opcode != NdOp::ATOMIC_ADD)
          continue;
        ++Count;
        EXPECT_EQ(Op.MemoryOrdering, NdMemoryOrdering::SequentiallyConsistent);
        EXPECT_EQ(Op.Output.Size, Width);
      }
      EXPECT_EQ(Count, 1u);
    }
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0xf0, 0x44, 0x01, 0x05, 0x7f, 0x15, 0x47, 0},
           {0x65, 0xf0, 0x48, 0x83, 0x44, 0x8b, 0xf8, 0xff},
           {0x64, 0x67, 0xf0, 0x01, 0x17},
           {0xf0, 0x66, 0x81, 0x07, 0x34, 0x80},
           {0xf0, 0x80, 0x07, 0xff}})
    EXPECT_FALSE(lift(Bytes).empty());
}

TEST(NativeStateAudit, LockedAddRejectsRawDetailDisagreement) {
  using Mutate = void (*)(cs_insn &);
  const Mutate Mutations[] = {
      [](cs_insn &I) { I.bytes[0] = 0x64; },
      [](cs_insn &I) { I.bytes[1] = 0xf3; },
      [](cs_insn &I) { I.bytes[2] = 0x4c; },
      [](cs_insn &I) { I.bytes[4] ^= 8; },
      [](cs_insn &I) { I.bytes[5] ^= 8; },
      [](cs_insn &I) { I.bytes[6] ^= 1; },
      [](cs_insn &I) { I.bytes[7] = 0; },
      [](cs_insn &I) { --I.size; },
      [](cs_insn &I) { I.detail->x86.op_count = 1; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[0].access = CS_AC_READ; },
      [](cs_insn &I) { I.detail->x86.operands[1].imm = 0; },
      [](cs_insn &I) { I.detail->x86.eflags ^= X86_EFLAGS_MODIFY_AF; }};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (unsigned M = 0; M != std::size(Mutations); ++M) {
    SCOPED_TRACE(M);
    const uint8_t Bytes[] = {0x65, 0xf0, 0x48, 0x83, 0x44, 0x8b, 0xf8, 0xff};
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn),
              int(sizeof(Bytes)));
    ASSERT_TRUE(lockedbinaryaudit::form(Insn.Raw, Arch::X64));
    Mutations[M](*Insn.Raw);
    EXPECT_FALSE(lockedbinaryaudit::form(Insn.Raw, Arch::X64));
    expectUnaudited(Dec, Insn);
  }
}
