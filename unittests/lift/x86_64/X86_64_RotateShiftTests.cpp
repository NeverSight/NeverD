#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"

class X86_64_RotateShift : public NeverDLiftTest {};

static fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_rotate_shift.o";
}

TEST_F(X86_64_RotateShift, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_rotate_shift.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_64_RotateShift, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_64_RotateShift, RolHasIntLeft) {
  verifyLowIRContains(testObj(), "test_rol", "INT_LEFT");
}

TEST_F(X86_64_RotateShift, RorHasIntRight) {
  verifyLowIRContains(testObj(), "test_ror", "INT_RIGHT");
}

TEST_F(X86_64_RotateShift, ShldPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_EQ(r.exitCode, 0);
  EXPECT_TRUE(r.out.find("INT_LEFT") != std::string::npos)
      << "SHLD should produce shift ops";
}

TEST_F(X86_64_RotateShift, ShrdPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_EQ(r.exitCode, 0);
  EXPECT_TRUE(r.out.find("INT_RIGHT") != std::string::npos)
      << "SHRD should produce shift ops";
}

TEST_F(X86_64_RotateShift, NoUnreachableInFunctions) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0);
  EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
      << "Found 'unreachable' in LLVM IR:\n"
      << r.out;
}

TEST(X86RotateCarry, WholeRotationsUpdateCarry) {
  using namespace neverd;
  for (unsigned Width : {1u, 2u}) {
    const unsigned Bits = Width * 8;
    const uint64_t Mask = (UINT64_C(1) << Bits) - 1;
    for (bool Left : {false, true}) {
      for (bool Immediate : {false, true}) {
        for (unsigned Count :
             {0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 24u, 31u, 32u, 33u, 255u}) {
          std::vector<uint8_t> Bytes;
          if (Width == 2)
            Bytes.push_back(0x66);
          Bytes.push_back(Immediate ? (Width == 1 ? 0xc0 : 0xc1)
                                    : (Width == 1 ? 0xd2 : 0xd3));
          Bytes.push_back(Left ? 0xc0 : 0xc8);
          if (Immediate)
            Bytes.push_back(static_cast<uint8_t>(Count));
          Decoder Dec;
          ASSERT_TRUE(Dec.init(Arch::X64));
          DecodedInsn Insn{};
          ASSERT_EQ(
              Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              static_cast<int>(Bytes.size()));
          std::vector<LowOp> Ops;
          Dec.liftToLow(Insn, Ops);
          ASSERT_FALSE(Ops.empty());
          for (uint64_t Value :
               {UINT64_C(0), UINT64_C(1), UINT64_C(1) << (Bits - 1), Mask}) {
            for (unsigned Carry : {0u, 1u}) {
              SCOPED_TRACE(::testing::Message()
                           << Width << '/' << Left << '/' << Immediate << '/'
                           << Count << '/' << Value << '/' << Carry);
              BinaryImage Image;
              Image.Arch = Arch::X64;
              Image.Bits = Bitness::Bits64;
              NdOpEmulator Emulator(Image);
              Emulator.setStrictMode(true);
              const uint64_t Original =
                  (UINT64_C(0x123456789abcdef0) & ~Mask) | Value;
              Emulator.setRegister(x86reg::RAX, Original);
              Emulator.setRegister(x86reg::RCX, Count);
              Emulator.setRegister(x86reg::CF, Carry);
              Emulator.setRegister(x86reg::OF, Carry);
              ASSERT_EQ(Emulator.run(Ops), Ops.size());
              const unsigned Masked = Count & 31;
              const unsigned Rotate = Masked % Bits;
              const uint64_t Result =
                  Rotate == 0
                      ? Value
                      : (Left ? ((Value << Rotate) | (Value >> (Bits - Rotate)))
                              : ((Value >> Rotate) |
                                 (Value << (Bits - Rotate)))) &
                            Mask;
              EXPECT_EQ(Emulator.getRegister(x86reg::RAX),
                        (Original & ~Mask) | Result);
              const uint64_t ExpectedCF = Masked == 0 ? Carry
                                          : Left ? Result & 1
                                                 : (Result >> (Bits - 1)) & 1;
              EXPECT_EQ(Emulator.getRegister(x86reg::CF), ExpectedCF);
              if (Masked == 0)
                EXPECT_EQ(Emulator.getRegister(x86reg::OF), Carry);
              else if (Masked == 1) {
                const uint64_t ExpectedOF =
                    (Result >> (Bits - 1)) ^
                    (Left ? Result & 1 : (Result >> (Bits - 2)) & 1);
                EXPECT_EQ(Emulator.getRegister(x86reg::OF), ExpectedOF);
              }
            }
          }
        }
      }
    }
  }
}
