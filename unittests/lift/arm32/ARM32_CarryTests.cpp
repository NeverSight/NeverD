#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/ARMLifter.h"

class ARM32_Carry : public NeverDLiftTest {};

static fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_carry_arm.o";
}

TEST_F(ARM32_Carry, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_carry_arm.o not built";
  verifyAllStages(testObj());
}

TEST_F(ARM32_Carry, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(ARM32_Carry, AdcPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_adc_arm", "INT_ADD");
}

TEST_F(ARM32_Carry, SbcPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_sbc_arm", "INT_SUB");
}

TEST_F(ARM32_Carry, RscPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_rsc_arm", "INT_SUB");
}

TEST_F(ARM32_Carry, NegUsesRsb) {
  verifyLowIRContains(testObj(), "test_neg_arm", "INT_SUB");
}

TEST_F(ARM32_Carry, LLVMIRNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(ARM32_Carry, AllModesSucceed) { verifyAllModesSucceed(testObj()); }

TEST(ARM32ThumbCarry, TwoOperandResultsAndNZCV) {
  using namespace neverd;
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
  for (bool Subtract : {false, true})
    for (bool SameRegister : {false, true}) {
      const uint8_t Code[] = {static_cast<uint8_t>((Subtract ? 0x80 : 0x40) |
                                                   (SameRegister ? 0 : 8)),
                              0x41};
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOne(Code, sizeof(Code), 0x1000, Insn), 2);
      ARMLifter L(Arch::ARM, InstructionMode::Thumb);
      std::vector<LowOp> Ops;
      L.lift(Insn.Raw, Ops);
      for (uint32_t A : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu})
        for (uint32_t InputB : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu})
          for (uint32_t Carry : {0u, 1u}) {
            SCOPED_TRACE(::testing::Message()
                         << Subtract << '/' << SameRegister << '/' << A << '/'
                         << InputB << '/' << Carry);
            const uint32_t B = SameRegister ? A : InputB;
            const uint64_t Wide =
                uint64_t(A) + (Subtract ? uint32_t(~B) : B) + Carry;
            const uint32_t Result = static_cast<uint32_t>(Wide);
            const auto Signed = [](uint32_t V) -> int64_t {
              return V < 0x80000000u ? int64_t(V) : int64_t(V) - 0x100000000LL;
            };
            const int64_t SignedResult =
                Subtract ? Signed(A) - Signed(B) - (1 - Carry)
                         : Signed(A) + Signed(B) + Carry;
            BinaryImage Img;
            Img.Arch = Arch::ARM;
            NdOpEmulator Emu(Img);
            Emu.setRegister(armreg::R0, A);
            Emu.setRegister(armreg::R1, InputB);
            Emu.setRegister(armreg::CFLAG, Carry);
            Emu.setRegister(armreg::NFLAG, 0);
            Emu.setRegister(armreg::ZFLAG, 0);
            Emu.setRegister(armreg::VFLAG, 0);
            for (const auto &Op : Ops)
              ASSERT_TRUE(Emu.step(Op));
            EXPECT_EQ(Emu.getRegister(armreg::R0), Result);
            EXPECT_EQ(Emu.getRegister(armreg::NFLAG), Result >> 31);
            EXPECT_EQ(Emu.getRegister(armreg::ZFLAG), Result == 0);
            EXPECT_EQ(Emu.getRegister(armreg::CFLAG), Wide >> 32);
            EXPECT_EQ(Emu.getRegister(armreg::VFLAG),
                      SignedResult < -0x80000000LL ||
                          SignedResult > 0x7FFFFFFFLL);
          }
    }
}

TEST(ARM32ThumbCarry, ModifiedImmediateCarryRespectsInstructionMode) {
  using namespace neverd;
  struct Case {
    InstructionMode Mode;
    uint8_t Code[4];
    uint32_t Immediate;
    bool PreservesCarry;
  };
  const Case Cases[] = {
      {InstructionMode::Thumb, {0x5f, 0xf4, 0x00, 0x00}, 0x800000u, false},
      {InstructionMode::Thumb, {0x5f, 0xf0, 0x00, 0x40}, 0x80000000u, false},
      {InstructionMode::Thumb, {0x5f, 0xf0, 0x80, 0x40}, 0x40000000u, false},
      {InstructionMode::Thumb, {0x5f, 0xf0, 0xab, 0x30}, 0xababababu, true},
      {InstructionMode::Thumb, {0x5f, 0xf0, 0xab, 0x00}, 0xabu, true},
      {InstructionMode::Thumb, {0x11, 0xf0, 0x00, 0x40}, 0x80000000u, false},
      {InstructionMode::ARM, {0x02, 0x01, 0xb0, 0xe3}, 0x80000000u, false},
      {InstructionMode::ARM, {0x01, 0x01, 0xb0, 0xe3}, 0x40000000u, false},
      {InstructionMode::ARM, {0xab, 0x00, 0xb0, 0xe3}, 0xabu, true},
  };
  for (const auto &C : Cases) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::ARM, C.Mode));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOne(C.Code, sizeof(C.Code), 0x1000, Insn), 4);
    SCOPED_TRACE(Insn.Raw->mnemonic);
    SCOPED_TRACE(Insn.Raw->op_str);
    ARMLifter L(Arch::ARM, C.Mode);
    std::vector<LowOp> Ops;
    L.lift(Insn.Raw, Ops);
    for (uint32_t Carry : {0u, 1u}) {
      SCOPED_TRACE(Carry);
      BinaryImage Img;
      Img.Arch = Arch::ARM;
      NdOpEmulator Emu(Img);
      Emu.setRegister(armreg::R0, 0);
      Emu.setRegister(armreg::R1, 0xffffffffu);
      Emu.setRegister(armreg::CFLAG, Carry);
      Emu.setRegister(armreg::VFLAG, 1);
      for (const auto &Op : Ops)
        ASSERT_TRUE(Emu.step(Op));
      EXPECT_EQ(Emu.getRegister(armreg::R0), C.Immediate);
      EXPECT_EQ(Emu.getRegister(armreg::NFLAG), C.Immediate >> 31);
      EXPECT_EQ(Emu.getRegister(armreg::ZFLAG), C.Immediate == 0);
      EXPECT_EQ(Emu.getRegister(armreg::CFLAG),
                C.PreservesCarry ? Carry : C.Immediate >> 31);
      EXPECT_EQ(Emu.getRegister(armreg::VFLAG), 1u);
    }
  }
}
