#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/AArch64Lifter.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstdint>

class AArch64_MulDiv : public NeverDLiftTest {};

static fs::path obj(const char *name) { return fs::path(TEST_OBJ_DIR) / name; }

TEST_F(AArch64_MulDiv, AllStages) { verifyAllStages(obj("test_muldiv_a64.o")); }

TEST_F(AArch64_MulDiv, NoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(obj("test_muldiv_a64.o"));
}

TEST_F(AArch64_MulDiv, NoUnreachable) {
  verifyLLVMIRNoUnreachable(obj("test_muldiv_a64.o"));
}

TEST_F(AArch64_MulDiv, PreservesMulSemantic) {
  verifyLLVMIRContains(obj("test_muldiv_a64.o"), "test_mul_a64", "mul");
}

TEST_F(AArch64_MulDiv, PreservesDivSemantic) {
  verifyLLVMIRContains(obj("test_muldiv_a64.o"), "test_udiv_a64", "udiv");
}

TEST_F(AArch64_MulDiv, DecompileC) {
  verifyDecompileProducesOutput(obj("test_muldiv_a64.o"));
}

namespace {

using namespace neverd;

constexpr va_t MulHighAddress = 0x1000;

int decodeMulHigh(Decoder &Dec, uint32_t Word, bool ForLift,
                  DecodedInsn &Insn) {
  uint8_t Bytes[4];
  writeLE<uint32_t>(Bytes, Word);
  return ForLift
             ? Dec.decodeOneForLift(Bytes, sizeof(Bytes), MulHighAddress, Insn)
             : Dec.decodeOne(Bytes, sizeof(Bytes), MulHighAddress, Insn);
}

TEST(AArch64MulHigh, RejectsVectorFormsThroughBothRealDecodePaths) {
  struct Case {
    const char *Name;
    uint32_t Word;
    unsigned Id;
    unsigned Operands;
  };
  // Encoding facts from Capstone's MC/AArch64/{SVE,SVE2}/{s,u}mulh.s.yaml.
  // These tests require a real decode, including the Capstone fallback; a
  // native-decoder decline must not silently omit a case from the assertion.
  const Case Cases[] = {
      {"sve-smulh-b", 0x04121FE0u, AARCH64_INS_SMULH, 4},
      {"sve-smulh-h", 0x04521FE0u, AARCH64_INS_SMULH, 4},
      {"sve-smulh-s", 0x04921FE0u, AARCH64_INS_SMULH, 4},
      {"sve-smulh-d", 0x04D21FE0u, AARCH64_INS_SMULH, 4},
      {"sve-umulh-b", 0x04131FE0u, AARCH64_INS_UMULH, 4},
      {"sve-umulh-h", 0x04531FE0u, AARCH64_INS_UMULH, 4},
      {"sve-umulh-s", 0x04931FE0u, AARCH64_INS_UMULH, 4},
      {"sve-umulh-d", 0x04D31FE0u, AARCH64_INS_UMULH, 4},
      {"sve2-smulh-b", 0x04226820u, AARCH64_INS_SMULH, 3},
      {"sve2-smulh-h", 0x04626820u, AARCH64_INS_SMULH, 3},
      {"sve2-smulh-s", 0x04BF6BDDu, AARCH64_INS_SMULH, 3},
      {"sve2-smulh-d", 0x04FF6BFFu, AARCH64_INS_SMULH, 3},
      {"sve2-umulh-b", 0x04226C20u, AARCH64_INS_UMULH, 3},
      {"sve2-umulh-h", 0x04626C20u, AARCH64_INS_UMULH, 3},
      {"sve2-umulh-s", 0x04BF6FDDu, AARCH64_INS_UMULH, 3},
      {"sve2-umulh-d", 0x04FF6FFFu, AARCH64_INS_UMULH, 3},
      // The bytes also occurred at an invalid, unaligned application candidate.
      // Here they are deliberately decoded at an independent aligned address;
      // this tests opcode ownership, not that candidate or application ISA use.
      {"observed-byte-pattern", 0x04606DA9u, AARCH64_INS_UMULH, 3},
  };
  for (const auto &C : Cases)
    for (bool ForLift : {false, true}) {
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(ForLift ? "decodeOneForLift" : "decodeOne");
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::AArch64));
      Dec.setStrict(true);
      DecodedInsn Insn;
      ASSERT_EQ(decodeMulHigh(Dec, C.Word, ForLift, Insn), 4);
      ASSERT_NE(Insn.Raw, nullptr);
      ASSERT_NE(Insn.Raw->detail, nullptr);
      ASSERT_EQ(Insn.Id, C.Id);
      ASSERT_EQ(Insn.Raw->detail->aarch64.op_count, C.Operands);
      std::vector<LowOp> Ops;
      EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
}

TEST(AArch64MulHigh, ScalarSpecialRegistersKeepWidthAndHighHalfSemantics) {
  struct Case {
    bool Signed;
    unsigned Dst, Left, Right;
    uint64_t A, B, Expected;
  };
  const Case Cases[] = {
      {true, 29, 30, 28, UINT64_MAX, UINT64_MAX, 0},
      {false, 30, 29, 28, UINT64_MAX, UINT64_MAX, UINT64_MAX - 1},
      {true, 28, 29, 30, 0x8000000000000000ULL, 0x8000000000000000ULL,
       0x4000000000000000ULL},
      {false, 0, 29, 30, 0x8000000000000000ULL, 0x8000000000000000ULL,
       0x4000000000000000ULL},
      {true, 0, 1, 2, UINT64_MAX - 9, 0x100000000ULL, UINT64_MAX},
      {false, 0, 1, 2, UINT64_MAX - 9, 0x100000000ULL, 0xffffffffULL},
      {true, 29, 30, 31, UINT64_MAX, 0, 0},
      {false, 30, 31, 29, 0, UINT64_MAX, 0},
      {true, 31, 29, 30, UINT64_MAX, UINT64_MAX, 0},
      {false, 31, 29, 30, UINT64_MAX, UINT64_MAX, UINT64_MAX - 1},
  };
  for (const auto &C : Cases)
    for (bool ForLift : {false, true}) {
      SCOPED_TRACE(C.Signed ? "smulh" : "umulh");
      SCOPED_TRACE(C.Dst);
      SCOPED_TRACE(ForLift ? "decodeOneForLift" : "decodeOne");
      const uint32_t Word = (C.Signed ? 0x9B407C00u : 0x9BC07C00u) |
                            (C.Right << 16) | (C.Left << 5) | C.Dst;
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::AArch64));
      Dec.setStrict(true);
      DecodedInsn Insn;
      ASSERT_EQ(decodeMulHigh(Dec, Word, ForLift, Insn), 4);
      ASSERT_EQ(Insn.Id, C.Signed ? AARCH64_INS_SMULH : AARCH64_INS_UMULH);
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops));
      ASSERT_EQ(Ops.size(), 4u);
      for (unsigned I = 0; I < 2; ++I) {
        EXPECT_EQ(Ops[I].Opcode, C.Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT);
        ASSERT_EQ(Ops[I].NumInputs, 1);
        EXPECT_EQ(Ops[I].Inputs[0].Size, 8);
        EXPECT_EQ(Ops[I].Output.Size, 16);
        const unsigned Register = I == 0 ? C.Left : C.Right;
        if (Register == 31) {
          EXPECT_TRUE(Ops[I].Inputs[0].isConst());
          EXPECT_EQ(Ops[I].Inputs[0].Offset, 0u);
        } else {
          EXPECT_TRUE(Ops[I].Inputs[0].isReg());
          EXPECT_EQ(Ops[I].Inputs[0].Offset, Register * 8u);
        }
      }
      EXPECT_EQ(Ops[2].Opcode, NdOp::INT_MULT);
      EXPECT_EQ(Ops[2].Output.Size, 16);
      EXPECT_EQ(Ops[3].Opcode, NdOp::SUBBYTES);
      EXPECT_EQ(Ops[3].Output.Size, 8);
      ASSERT_EQ(Ops[3].NumInputs, 2);
      EXPECT_EQ(Ops[3].Inputs[0].Size, 16);
      EXPECT_TRUE(Ops[3].Inputs[1].isConst());
      EXPECT_EQ(Ops[3].Inputs[1].Offset, 8u);

      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emu(Image);
      Emu.setStrictMode(true);
      constexpr uint64_t ZeroRegisterSentinel = 0x123456789abcdef0ULL;
      Emu.setRegister(a64reg::XZR, ZeroRegisterSentinel);
      if (C.Left != 31)
        Emu.setRegister(C.Left * 8u, C.A);
      if (C.Right != 31)
        Emu.setRegister(C.Right * 8u, C.B);
      ASSERT_EQ(Emu.run(Ops), Ops.size());
      EXPECT_FALSE(Emu.skips().any());
      EXPECT_EQ(Emu.getRegister(a64reg::XZR).value_or(0), ZeroRegisterSentinel);
      if (C.Dst == 31) {
        EXPECT_TRUE(Ops.back().Output.isTemp());
        EXPECT_EQ(Emu.getRegister(C.Left * 8u).value_or(0), C.A);
        EXPECT_EQ(Emu.getRegister(C.Right * 8u).value_or(0), C.B);
      } else {
        EXPECT_TRUE(Ops.back().Output.isReg());
        EXPECT_EQ(Ops.back().Output.Offset, C.Dst * 8u);
        const auto Result = Emu.getRegister(C.Dst * 8u);
        ASSERT_TRUE(Result.has_value());
        EXPECT_EQ(*Result, C.Expected);
      }
    }
}

TEST(AArch64MulHigh, RejectsMalformedOperandShapesWithoutPartialIR) {
  struct Mutation {
    const char *Name;
    void (*Apply)(cs_aarch64_op &);
  };
  const Mutation Mutations[] = {
      {"w-register", [](cs_aarch64_op &O) { O.reg = AARCH64_REG_W0; }},
      {"same-width-fp-register",
       [](cs_aarch64_op &O) { O.reg = AARCH64_REG_D0; }},
      {"vector-register", [](cs_aarch64_op &O) { O.reg = AARCH64_REG_Q0; }},
      {"scalable-register", [](cs_aarch64_op &O) { O.reg = AARCH64_REG_Z0; }},
      {"stack-pointer", [](cs_aarch64_op &O) { O.reg = AARCH64_REG_SP; }},
      {"invalid-register",
       [](cs_aarch64_op &O) { O.reg = AARCH64_REG_INVALID; }},
      {"immediate",
       [](cs_aarch64_op &O) {
         O.type = AARCH64_OP_IMM;
         O.imm = 1;
       }},
      {"memory",
       [](cs_aarch64_op &O) {
         O.type = AARCH64_OP_MEM;
         O.mem = {};
         O.mem.base = AARCH64_REG_X1;
       }},
      {"predicate",
       [](cs_aarch64_op &O) {
         O.type = AARCH64_OP_PRED;
         O.pred = {};
         O.pred.reg = AARCH64_REG_P0;
       }},
      {"lane-index", [](cs_aarch64_op &O) { O.vector_index = 0; }},
      {"invalid-lane-index", [](cs_aarch64_op &O) { O.vector_index = -2; }},
      {"lane-layout", [](cs_aarch64_op &O) { O.vas = AARCH64LAYOUT_VL_D; }},
      {"shift-kind", [](cs_aarch64_op &O) { O.shift.type = AARCH64_SFT_LSL; }},
      {"shift-amount", [](cs_aarch64_op &O) { O.shift.value = 1; }},
      {"extension", [](cs_aarch64_op &O) { O.ext = AARCH64_EXT_SXTX; }},
      {"v-register-flag", [](cs_aarch64_op &O) { O.is_vreg = true; }},
      {"register-list", [](cs_aarch64_op &O) { O.is_list_member = true; }},
  };
  for (uint32_t Word : {0x9B427C20u, 0x9BC27C20u}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    DecodedInsn Original;
    ASSERT_EQ(decodeMulHigh(Dec, Word, false, Original), 4);
    ASSERT_NE(Original.Raw, nullptr);
    ASSERT_NE(Original.Raw->detail, nullptr);
    ASSERT_EQ(Original.Raw->detail->aarch64.op_count, 3);
    AArch64Lifter Lifter(Arch::AArch64);
    Lifter.setStrict(true);
    for (const auto &M : Mutations)
      for (unsigned Operand = 0; Operand < 3; ++Operand) {
        SCOPED_TRACE(Word);
        SCOPED_TRACE(M.Name);
        SCOPED_TRACE(Operand);
        cs_detail Detail = *Original.Raw->detail;
        cs_insn Insn = *Original.Raw;
        Insn.detail = &Detail;
        M.Apply(Detail.aarch64.operands[Operand]);
        std::vector<LowOp> Ops;
        EXPECT_THROW(Lifter.lift(&Insn, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
    for (unsigned Count : {0u, 2u, 4u, unsigned(NUM_AARCH64_OPS)}) {
      SCOPED_TRACE(Word);
      SCOPED_TRACE(Count);
      cs_detail Detail = *Original.Raw->detail;
      cs_insn Insn = *Original.Raw;
      Insn.detail = &Detail;
      Detail.aarch64.op_count = static_cast<uint8_t>(Count);
      std::vector<LowOp> Ops;
      EXPECT_THROW(Lifter.lift(&Insn, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  }
}

} // namespace
