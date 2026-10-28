#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/ARM/ARMLifter.h"

#include <array>

class ARM32_Control : public NeverDLiftTest {};

static fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_control_arm.o";
}

TEST_F(ARM32_Control, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_control_arm.o not built";
  verifyAllStages(testObj());
}

TEST_F(ARM32_Control, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(ARM32_Control, CmpBeqLifts) {
  verifyLowIRContains(testObj(), "test_cmp_beq_arm", "INT_SUB");
}

TEST_F(ARM32_Control, TstLifts) {
  verifyLowIRContains(testObj(), "test_tst_arm", "INT_AND");
}

TEST_F(ARM32_Control, CmnLifts) {
  verifyLowIRContains(testObj(), "test_cmn_arm", "INT_ADD");
}

TEST_F(ARM32_Control, ClzLifts) {
  verifyLowIRContains(testObj(), "test_clz_arm", "LZCOUNT");
}

TEST_F(ARM32_Control, RevLifts) {
  verifyLowIRContains(testObj(), "test_rev_arm", "INT_AND");
}

TEST_F(ARM32_Control, SxtbLifts) {
  verifyLowIRContains(testObj(), "test_sxtb_arm", "INT_SEXT");
}

TEST_F(ARM32_Control, SxthLifts) {
  verifyLowIRContains(testObj(), "test_sxth_arm", "INT_SEXT");
}

TEST_F(ARM32_Control, UxtbLifts) {
  verifyLowIRContains(testObj(), "test_uxtb_arm", "INT_AND");
}

TEST_F(ARM32_Control, AdcLifts) {
  verifyLowIRContains(testObj(), "test_adc_arm", "INT_ADD");
}

TEST_F(ARM32_Control, SbcLifts) {
  verifyLowIRContains(testObj(), "test_sbc_arm", "INT_SUB");
}

TEST_F(ARM32_Control, NoUnreachableInFunctions) {
  verifyLLVMIRNotContains(testObj(), "", "unreachable");
}

TEST(ARM32ThumbControl, CallWhoseHalfwordsLookLikeAUserRegisterLoad) {
  // `bl` 0xf0 bytes ahead, f000 f878: its two halfwords read as one A32 word
  // carry the multiple-transfer class and bit 22, the user-register form of
  // an A32 `ldm` that returns from an exception.  T32 has no such form; the
  // call lifts as a call.
  using namespace neverd;
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
  const uint8_t Call[] = {0x00, 0xf0, 0x78, 0xf8};
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOne(Call, sizeof(Call), 0x40119c, Insn), 4);
  ARMLifter L(Arch::ARM, InstructionMode::Thumb);
  ASSERT_TRUE(L.isStrict());
  std::vector<LowOp> Ops;
  ASSERT_NO_THROW(L.lift(Insn.Raw, Ops));
  bool Called = false;
  for (const LowOp &Op : Ops)
    Called |= Op.Opcode == NdOp::CALL && Op.NumInputs > 0 &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 0x401290;
  EXPECT_TRUE(Called);

  // A32 `ldm sp, {pc}^` is that exception return, which strict lifting
  // refuses until it has a typed contract.
  Decoder A32;
  ASSERT_TRUE(A32.init(Arch::ARM, InstructionMode::ARM));
  const uint8_t Return[] = {0x00, 0x80, 0xdd, 0xe8};
  ASSERT_EQ(A32.decodeOne(Return, sizeof(Return), 0x2000, Insn), 4);
  ARMLifter ARMMode(Arch::ARM, InstructionMode::ARM);
  Ops.clear();
  EXPECT_THROW(ARMMode.lift(Insn.Raw, Ops), UnliftedInstruction);
}

TEST(ARM32ThumbControl, LoadingPCOffTheStackReturns) {
  // MSVC returns from a function that homed r0-r3 with `ldr pc, [sp], #N`:
  // it pops the saved LR into PC and releases the home area with it.  A load
  // of PC that pops nothing still branches.
  using namespace neverd;
  struct Case {
    std::array<uint8_t, 4> Bytes;
    bool Returns;
  };
  const Case Cases[] = {
      {{0x5d, 0xf8, 0x0c, 0xfb}, true},  // ldr pc, [sp], #0xc
      {{0x5d, 0xf8, 0x14, 0xfb}, true},  // ldr pc, [sp], #0x14
      {{0x5d, 0xf8, 0x04, 0xfb}, true},  // ldr pc, [sp], #4: pop {pc}
      {{0xdd, 0xf8, 0x0c, 0xf0}, false}, // ldr.w pc, [sp, #0xc]
      {{0x5d, 0xf8, 0x04, 0xff}, false}, // ldr pc, [sp, #4]!
      {{0x5d, 0xf8, 0x0c, 0xf9}, false}, // ldr pc, [sp], #-0xc
      {{0x50, 0xf8, 0x0c, 0xfb}, false}, // ldr pc, [r0], #0xc
  };
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
  ARMLifter L(Arch::ARM, InstructionMode::Thumb);
  for (const Case &C : Cases) {
    SCOPED_TRACE(::testing::Message()
                 << std::hex << unsigned(C.Bytes[0]) << ' '
                 << unsigned(C.Bytes[1]) << ' ' << unsigned(C.Bytes[2]) << ' '
                 << unsigned(C.Bytes[3]));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOne(C.Bytes.data(), C.Bytes.size(), 0x1000, Insn), 4);
    EXPECT_EQ(L.isReturn(Insn.Raw), C.Returns);
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(L.lift(Insn.Raw, Ops));
    ASSERT_FALSE(Ops.empty());
    EXPECT_EQ(Ops.back().Opcode, C.Returns ? NdOp::RETURN : NdOp::INDIR_BR);
  }
}

TEST(ARM32ThumbControl, UndefinedInstructionWritesNoRegister) {
  // `udf #0xfe`, MSVC's __debugbreak: the debugger resumes after it with
  // every register as it was, so the trap must not define r0.
  using namespace neverd;
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
  const uint8_t Break[] = {0xfe, 0xde};
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOne(Break, sizeof(Break), 0x1000, Insn), 2);
  ARMLifter L(Arch::ARM, InstructionMode::Thumb);
  std::vector<LowOp> Ops;
  L.lift(Insn.Raw, Ops);
  bool Trap = false;
  for (const LowOp &Op : Ops) {
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
        Op.Inputs[0].isConst() &&
        Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::ArmUdf)) {
      Trap = true;
      EXPECT_FALSE(Op.Output.isReg());
    }
    EXPECT_FALSE(Op.Output.isReg() && Op.Output.Offset == armreg::R0);
  }
  EXPECT_TRUE(Trap);
}
