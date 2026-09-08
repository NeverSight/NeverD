#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Lifter.h"

class X86_32_Stack : public NeverDLiftTest {};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_stack32.o"; }

TEST_F(X86_32_Stack, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_stack32.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_32_Stack, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_32_Stack, PushPopLifts) {
  verifyLowIRContains(testObj(), "test_push_pop", "STORE");
}

TEST_F(X86_32_Stack, Xchg32Lifts) {
  verifyLowIRContains(testObj(), "test_xchg32", "COPY");
}

TEST_F(X86_32_Stack, Bswap32Lifts) {
  auto r = liftToLowIR(testObj());
  ASSERT_EQ(r.exitCode, 0);
  EXPECT_TRUE(r.out.find("Bswap") != std::string::npos ||
              r.out.find("INTRINSIC") != std::string::npos)
      << "Expected BSWAP intrinsic in LowIR";
}

TEST_F(X86_32_Stack, Cmov32Lifts) {
  verifyLowIRContains(testObj(), "test_cmov32", "INT_SUB");
}

TEST_F(X86_32_Stack, Setcc32Lifts) {
  verifyLowIRContains(testObj(), "test_setcc32", "INT_SUB");
}

TEST_F(X86_32_Stack, Adc32Lifts) {
  verifyLowIRContains(testObj(), "test_adc32", "INT_ADD");
}

TEST_F(X86_32_Stack, Sbb32Lifts) {
  verifyLowIRContains(testObj(), "test_sbb32", "INT_SUB");
}

TEST_F(X86_32_Stack, NoUnreachableInFunctions) {
  verifyLLVMIRNotContains(testObj(), "", "unreachable");
}

TEST(X86PushAll, SavesEntryStackPointer) {
  using namespace neverd;
  const uint64_t Registers[] = {x86reg::RAX, x86reg::RCX, x86reg::RDX,
                                x86reg::RBX, x86reg::RSP, x86reg::RBP,
                                x86reg::RSI, x86reg::RDI};
  constexpr uint32_t EntrySP = 0x2000;
  for (unsigned Width : {2u, 4u}) {
    SCOPED_TRACE(Width);
    const uint8_t Code[] = {0x66, 0x60};
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X86));
    DecodedInsn Insn{};
    const unsigned Start = Width == 2 ? 0 : 1;
    ASSERT_EQ(Dec.decodeOne(Code + Start, sizeof(Code) - Start, 0x1000, Insn),
              static_cast<int>(sizeof(Code) - Start));
    X86Lifter L(Arch::X86);
    std::vector<LowOp> Ops;
    L.lift(Insn.Raw, Ops);
    BinaryImage Img;
    Img.Arch = Arch::X86;
    NdOpEmulator Emu(Img);
    uint32_t Values[8];
    for (unsigned I = 0; I < 8; ++I) {
      Values[I] = I == 4 ? EntrySP : 0x12340000u + 0x111u * (I + 1);
      Emu.setRegister(Registers[I], Values[I]);
    }
    for (const auto &Op : Ops)
      ASSERT_TRUE(Emu.step(Op));
    EXPECT_EQ(Emu.getRegister(x86reg::RSP), EntrySP - 8 * Width);
    for (unsigned I = 0; I < 8; ++I) {
      SCOPED_TRACE(I);
      if (I != 4)
        EXPECT_EQ(Emu.getRegister(Registers[I]), Values[I]);
      LowOp Load;
      Load.Opcode = NdOp::LOAD;
      Load.Output = NdVar::reg(0x1000, Width);
      Load.addInput(NdVar::cst(EntrySP - (I + 1) * Width, 4));
      ASSERT_TRUE(Emu.step(Load));
      EXPECT_EQ(Emu.getRegister(0x1000),
                Values[I] & (Width == 2 ? 0xffffu : 0xffffffffu));
    }
  }
}
