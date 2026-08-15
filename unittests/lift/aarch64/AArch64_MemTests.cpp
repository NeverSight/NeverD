#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/AArch64Regs.h"

#include <algorithm>
#include <array>

using namespace neverd;

class AArch64_Mem : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_mem_a64.o";
}

TEST_F(AArch64_Mem, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_mem_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Mem, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Mem, LdrStrHasLoadStore) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("LOAD") != std::string::npos)
        << "LDR should produce LOAD";
    EXPECT_TRUE(r.out.find("STORE") != std::string::npos)
        << "STR should produce STORE";
}

TEST_F(AArch64_Mem, UxtbLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_AND") != std::string::npos ||
                r.out.find("INT_ZEXT") != std::string::npos ||
                r.out.find("SUBBYTES") != std::string::npos)
        << "UXTB should produce masking/extension";
}

TEST_F(AArch64_Mem, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(AArch64_Mem, LdrLiteralKeepsTheFullVirtualAddress) {
    constexpr va_t InsnVA = 0x100000460ULL;
    constexpr va_t LiteralVA = InsnVA + 8;
    // ldr x0, #8
    constexpr std::array<uint8_t, 4> Bytes = {0x40, 0x00, 0x00, 0x58};

    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), InsnVA, Insn),
              4);

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);

    const auto Load = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD;
    });
    ASSERT_NE(Load, Ops.end());
    ASSERT_EQ(Load->NumInputs, 1);

    const auto Address = std::find_if(
        Ops.begin(), Load, [&](const LowOp &Op) {
            return Op.Opcode == NdOp::COPY && Op.Output == Load->Inputs[0] &&
                   Op.NumInputs == 1 && Op.Inputs[0].isConst();
        });
    ASSERT_NE(Address, Load);
    EXPECT_EQ(Address->Inputs[0].Offset, LiteralVA);
}

TEST_F(AArch64_Mem, Ld64bLoadsEightConsecutiveGPRs) {
  // ld64b x0, [x8]
  constexpr std::array<uint8_t, 4> Bytes = {0x00, 0xD1, 0x3F, 0xF8};

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn), 4);

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);

  std::vector<const LowOp *> Loads;
  for (const auto &Op : Ops)
    if (Op.Opcode == NdOp::LOAD)
      Loads.push_back(&Op);

  ASSERT_EQ(Loads.size(), 8u);
  const NdVar BaseEA = Loads.front()->Inputs[0];
  for (unsigned I = 0; I < Loads.size(); ++I) {
    const LowOp &Load = *Loads[I];
    EXPECT_EQ(Load.Output, NdVar::reg(a64reg::X0 + I * 8, 8));
    ASSERT_EQ(Load.NumInputs, 1);
    if (I == 0) {
      EXPECT_EQ(Load.Inputs[0], BaseEA);
      continue;
    }

    const auto Address =
        std::find_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
          return Op.Opcode == NdOp::INT_ADD && Op.Output == Load.Inputs[0] &&
                 Op.NumInputs == 2 && Op.Inputs[0] == BaseEA &&
                 Op.Inputs[1].isConst();
        });
    ASSERT_NE(Address, Ops.end());
    EXPECT_EQ(Address->Inputs[1].Offset, I * 8);
  }
}
