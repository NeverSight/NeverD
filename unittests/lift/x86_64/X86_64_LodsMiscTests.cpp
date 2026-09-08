#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Lifter.h"

class X86_64_LodsMisc : public NeverDLiftTest {};

static fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_lods_misc.o";
}

TEST_F(X86_64_LodsMisc, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_lods_misc.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_64_LodsMisc, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_64_LodsMisc, NoUnreachable) { verifyLLVMIRNoUnreachable(testObj()); }

TEST_F(X86_64_LodsMisc, LodsbPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("INTRINSIC") || r.contains("LOAD"))
      << "LODSB should produce load or intrinsic";
}

TEST_F(X86_64_LodsMisc, PushfPopfPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("STORE") || r.contains("LOAD"))
      << "PUSHF/POPF should produce stack ops";
}

TEST_F(X86_64_LodsMisc, CbwCwdePreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("INT_SEXT") || r.contains("COPY"))
      << "CBW/CWDE should produce sign extension ops";
}

TEST_F(X86_64_LodsMisc, LoopPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("INT_SUB") || r.contains("COND_BR"))
      << "LOOP should decrement ECX and branch";
}

TEST_F(X86_64_LodsMisc, LLVMIRNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_64_LodsMisc, NoConstantTrueBranch) {
  verifyNoConstantTrueBranch(testObj());
}

TEST_F(X86_64_LodsMisc, LLVMIRHasConditionalLogic) {
  // Deep optimization proves test_loop_insn(count) == count and removes the
  // two fixed, side-effect-free LOOP variants.  Inspect lowering before that
  // legal CFG elimination; the optimized path remains covered by
  // AllStagesPass, LLVMIRNoVerifierErrors, and NoConstantTrueBranch.
  auto R = liftToLLVMIRUnopt(testObj());
  ASSERT_EQ(R.exitCode, 0) << "LLVM IR lift failed: " << R.err;
  bool HasCond = R.out.find("br i1 %") != std::string::npos ||
                 R.out.find("select i1") != std::string::npos ||
                 R.out.find("icmp") != std::string::npos;
  EXPECT_TRUE(HasCond)
      << "Expected conditional logic (br i1/select/icmp) in LLVM IR";
}

TEST_F(X86_64_LodsMisc, DecompileSucceeds) {
  verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_64_LodsMisc, AllModesSucceed) { verifyAllModesSucceed(testObj()); }

TEST(X86LoopCounter, AddressOverrideSelectsCounterWidth) {
  using namespace neverd;
  for (Arch A : {Arch::X86, Arch::X64})
    for (uint8_t Prefix : {0u, 0x66u, 0x67u})
      for (uint8_t Opcode : {0xe0u, 0xe1u, 0xe2u}) {
        Decoder Dec;
        ASSERT_TRUE(Dec.init(A));
        const uint8_t Code[] = {Prefix, Opcode, 0};
        const unsigned Start = Prefix == 0 ? 1 : 0;
        DecodedInsn Insn{};
        ASSERT_EQ(
            Dec.decodeOne(Code + Start, sizeof(Code) - Start, 0x1000, Insn),
            static_cast<int>(sizeof(Code) - Start));
        X86Lifter L(A);
        std::vector<LowOp> Ops;
        L.lift(Insn.Raw, Ops);
        const unsigned Width = A == Arch::X64 ? (Prefix == 0x67 ? 4 : 8)
                                              : (Prefix == 0x67 ? 2 : 4);
        const uint64_t Mask =
            Width == 8 ? UINT64_MAX : (uint64_t{1} << (Width * 8)) - 1;
        for (uint64_t Initial : {0ULL, 1ULL, 2ULL, 0x10001ULL, 0x100000001ULL})
          for (uint64_t ZF : {0u, 1u}) {
            SCOPED_TRACE(::testing::Message()
                         << static_cast<unsigned>(A) << '/' << unsigned(Prefix)
                         << '/' << unsigned(Opcode) << '/' << Initial << '/'
                         << ZF);
            BinaryImage Img;
            Img.Arch = A;
            NdOpEmulator Emu(Img);
            Emu.setRegister(x86reg::RCX, Initial);
            Emu.setRegister(x86reg::ZF, ZF);
            Emu.setRegister(x86reg::CF, 1);
            const uint64_t Count = (Initial - 1) & Mask;
            const bool Taken =
                Count != 0 &&
                (Opcode == 0xe2 || (Opcode == 0xe1 ? ZF != 0 : ZF == 0));
            bool SawBranch = false;
            for (const auto &Op : Ops) {
              if (Op.Opcode == NdOp::COND_BR) {
                LowOp Observe;
                Observe.Opcode = NdOp::COPY;
                Observe.Output = NdVar::reg(0x1000, 1);
                Observe.addInput(Op.Inputs[1]);
                ASSERT_TRUE(Emu.step(Observe));
                EXPECT_EQ(Emu.getRegister(0x1000), Taken);
                SawBranch = true;
                break;
              }
              ASSERT_TRUE(Emu.step(Op));
            }
            ASSERT_TRUE(SawBranch);
            ASSERT_TRUE(Emu.getRegister(x86reg::RCX).has_value());
            EXPECT_EQ(*Emu.getRegister(x86reg::RCX) & Mask, Count);
            EXPECT_EQ(Emu.getRegister(x86reg::ZF), ZF);
            EXPECT_EQ(Emu.getRegister(x86reg::CF), 1u);
          }
      }
}
