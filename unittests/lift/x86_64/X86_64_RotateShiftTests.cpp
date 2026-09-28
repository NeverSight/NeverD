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

namespace {
using namespace neverd;

std::vector<LowOp> liftOneX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

struct ShiftOutcome {
  uint64_t Rax, Rdx;
  uint64_t CF, ZF, SF, PF, OF;
  bool operator==(const ShiftOutcome &O) const {
    return Rax == O.Rax && Rdx == O.Rdx && CF == O.CF && ZF == O.ZF &&
           SF == O.SF && PF == O.PF && OF == O.OF;
  }
};

ShiftOutcome runShift(const std::vector<LowOp> &Ops, uint64_t Rax, uint64_t Rdx,
                      uint64_t Rcx, uint64_t Flags) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RAX, Rax);
  Emulator.setRegister(x86reg::RDX, Rdx);
  Emulator.setRegister(x86reg::RCX, Rcx);
  const uint64_t FlagRegs[] = {x86reg::CF, x86reg::ZF, x86reg::SF, x86reg::PF,
                               x86reg::OF};
  for (unsigned I = 0; I != 5; ++I)
    Emulator.setRegister(FlagRegs[I], (Flags >> I) & 1);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  auto Get = [&](uint64_t Reg) {
    return Emulator.getRegister(Reg).value_or(~0ull);
  };
  return {Get(x86reg::RAX), Get(x86reg::RDX), Get(x86reg::CF), Get(x86reg::ZF),
          Get(x86reg::SF),  Get(x86reg::PF),  Get(x86reg::OF)};
}
} // namespace

TEST(X86ShiftCount, ImmediateCountMatchesTheSameCountInCL) {
  // A constant count is decided when lifted: no flag select remains, and the
  // outcome equals the generic CL form for every masked count.
  struct Form {
    std::vector<uint8_t> Imm, Cl; // opcode bytes before ModRM
    uint8_t ModRM;
    bool Double; // SHLD/SHRD take RDX as the source
  };
  for (unsigned Width : {1u, 2u, 4u, 8u}) {
    std::vector<Form> Forms;
    const uint8_t Imm = Width == 1 ? 0xc0 : 0xc1;
    const uint8_t Cl = Width == 1 ? 0xd2 : 0xd3;
    for (uint8_t Reg : {0, 1, 2, 3, 4, 5, 7}) // ROL ROR RCL RCR SHL SHR SAR
      Forms.push_back(
          {{Imm}, {Cl}, static_cast<uint8_t>(0xc0 | Reg << 3), false});
    if (Width != 1) {
      Forms.push_back({{0x0f, 0xa4}, {0x0f, 0xa5}, 0xd0, true}); // SHLD
      Forms.push_back({{0x0f, 0xac}, {0x0f, 0xad}, 0xd0, true}); // SHRD
    }
    for (const Form &F : Forms)
      for (unsigned Count :
           {0u, 1u, 3u, 8u, 9u, 16u, 17u, 31u, 32u, 33u, 63u, 64u, 255u}) {
        std::vector<uint8_t> Prefix;
        if (Width == 2)
          Prefix.push_back(0x66);
        if (Width == 8)
          Prefix.push_back(0x48);
        std::vector<uint8_t> ImmBytes = Prefix, ClBytes = Prefix;
        ImmBytes.insert(ImmBytes.end(), F.Imm.begin(), F.Imm.end());
        ImmBytes.push_back(F.ModRM);
        ImmBytes.push_back(static_cast<uint8_t>(Count));
        ClBytes.insert(ClBytes.end(), F.Cl.begin(), F.Cl.end());
        ClBytes.push_back(F.ModRM);
        const auto ImmOps = liftOneX64(ImmBytes);
        const auto ClOps = liftOneX64(ClBytes);
        ASSERT_FALSE(ImmOps.empty());
        ASSERT_FALSE(ClOps.empty());
        EXPECT_EQ(std::count_if(ImmOps.begin(), ImmOps.end(),
                                [&](const LowOp &Op) {
                                  return Op.Opcode == NdOp::SELECT ||
                                         (Op.Opcode == NdOp::INT_AND &&
                                          Op.NumInputs == 2 &&
                                          Op.Inputs[0].isConst() &&
                                          Op.Inputs[1].isConst());
                                }),
                  0)
            << "width " << Width << " modrm " << int(F.ModRM) << " count "
            << Count;
        for (uint64_t Value :
             {UINT64_C(0), UINT64_C(1), UINT64_C(0x8000000080008081),
              UINT64_C(0xfedcba9876543210), ~UINT64_C(0)})
          for (uint64_t Flags : {UINT64_C(0), UINT64_C(0x1f), UINT64_C(0x15)}) {
            SCOPED_TRACE(::testing::Message()
                         << "width " << Width << " modrm " << int(F.ModRM)
                         << " double " << F.Double << " count " << Count
                         << " value " << Value << " flags " << Flags);
            const uint64_t Source = UINT64_C(0x0123456789abcdef);
            EXPECT_EQ(runShift(ImmOps, Value, Source, 0, Flags),
                      runShift(ClOps, Value, Source, Count, Flags));
          }
      }
  }
}
