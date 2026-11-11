//===- MIPSRelease6Tests.cpp - Release 6 scalar, CFG and decode tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/support/BinaryEncoding.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

namespace neverd {
namespace {

class MIPSRelease6 : public ::testing::TestWithParam<Arch> {
protected:
  BinaryImage Image;
  Decoder Decode;
  DecodedExecutionUnit Unit;
  std::vector<LowOp> Ops;
  void SetUp() override {
    Image.Arch = GetParam();
    Image.Bits = isMIPS64(GetParam()) ? Bitness::Bits64 : Bitness::Bits32;
    Image.MIPS.emplace();
    Image.MIPS->ABI = isMIPS64(GetParam()) ? MIPSABI::N64 : MIPSABI::O32;
    Image.MIPS->ISARevision = 6;
    ASSERT_TRUE(Decode.init(Image));
  }
  int decode(uint32_t Word, va_t PC = 0x10000) {
    uint8_t Bytes[8]{};
    llvm::support::endian::write32(Bytes, Word,
                                   archLittleEndian(GetParam())
                                       ? llvm::endianness::little
                                       : llvm::endianness::big);
    return Decode.decodeUnitForLift(Bytes, sizeof(Bytes), PC, Unit);
  }
  void run(uint32_t Word, NdOpEmulator &Emu) {
    ASSERT_EQ(decode(Word), 4);
    Ops.clear();
    Decode.liftUnitToLow(Unit, Ops);
    for (const auto &Op : Ops)
      ASSERT_TRUE(Emu.step(Op)) << ndOpName(Op.Opcode);
  }
};

TEST_P(MIPSRelease6, IntegerResultsAndArchitecturalState) {
  // GNU as -mips32r6: every destination is v0; sources are a0 and a1.
  struct Row {
    uint32_t Op;
    uint64_t X, Y;
    uint32_t Result;
  };
  constexpr Row Rows[] = {
      {0x00851098, 0xfffffffe, 7, 0xfffffff2}, // MUL
      {0x008510d8, 0xfffffffe, 7, 0xffffffff}, // MUH
      {0x008510d9, 0xfffffffe, 7, 6},          // MUHU
      {0x0085109a, 0xfffffff9, 3, 0xfffffffe}, // DIV
      {0x008510da, 0xfffffff9, 3, 0xffffffff}, // MOD
      {0x0085109b, 0xfffffff9, 3,
       0x55555553}, // DIVU accepts zero extension on n64
      {0x008510db, 0xfffffff9, 3, 0},
      {0x00851035, 0xabcdef, 0, 0xabcdef},
      {0x00851037, 0xabcdef, 0, 0},
      {0x00801050, 0x00100000, 0, 11},
      {0x00801051, 0xfffffff0, 0, 28},
      {0x7c041020, 0x12345678, 0, 0x482c6a1e},
      {0x7c8512a0, 0xaabbccdd, 0x11223344, 0x3344aabb},
      {0x00851045, 7, 9, 37}, // LSA shift two
  };
  const bool Wide = isMIPS64(GetParam());
  for (const auto &R : Rows) {
    SCOPED_TRACE(::testing::Message() << std::hex << R.Op);
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    const bool UnsignedDiv = (R.Op & 63) == 27;
    const auto Extend = [&](uint32_t V) -> uint64_t {
      return Wide && !UnsignedDiv ? uint64_t(int64_t(int32_t(V))) : V;
    };
    E.setRegister(mipsreg::A0, Extend(R.X));
    E.setRegister(mipsreg::A1, Extend(R.Y));
    E.setRegister(mipsreg::HI, 0x1234);
    E.setRegister(mipsreg::LO, 0x5678);
    run(R.Op, E);
    EXPECT_EQ(E.getRegister(mipsreg::V0),
              Wide ? uint64_t(int64_t(int32_t(R.Result))) : R.Result);
    EXPECT_EQ(E.getRegister(mipsreg::HI), 0x1234u);
    EXPECT_EQ(E.getRegister(mipsreg::LO), 0x5678u);
  }
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegister(mipsreg::A0, 1);
  E.setRegister(mipsreg::A1, 0);
  run(0x0085109a, E);
  EXPECT_FALSE(E.getRegister(mipsreg::V0));
}

TEST_P(MIPSRelease6, UnalignedWordLoadUsesTargetOrder) {
  Segment Data;
  Data.VA = 0x2000;
  Data.Data = {0, 0x81, 0x72, 0x63, 0xf4};
  Data.Size = Data.FileSz = Data.Data.size();
  Data.Flags = SegmentFlags::Readable;
  Image.Segments.push_back(std::move(Data));
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegister(mipsreg::A0, 0x2000);
  run(0x8c820001, E); // LW v0,1(a0)
  const uint32_t V = archLittleEndian(GetParam()) ? 0xf4637281 : 0x817263f4;
  EXPECT_EQ(E.getRegister(mipsreg::V0),
            isMIPS64(GetParam()) ? uint64_t(int64_t(int32_t(V))) : V);
}

TEST_P(MIPSRelease6, DoublewordOperationsAndPCRelativeBaseRetainHighBits) {
  if (!isMIPS64(GetParam())) {
    for (const auto Word : {0x008510ddu, 0x7c8512e4u, 0xec580001u}) {
      if (!decode(Word))
        continue;
      EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops), UnliftedInstruction);
    }
    return;
  }
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegister(mipsreg::A0, UINT64_MAX);
  E.setRegister(mipsreg::A1, 2);
  run(0x008510dd, E);
  EXPECT_EQ(E.getRegister(mipsreg::V0), 1u); // DMUHU
  E.setRegister(mipsreg::A0, 0x8000000000000000ull);
  E.setRegister(mipsreg::A1, UINT64_MAX);
  run(0x0085109e, E);
  EXPECT_EQ(E.getRegister(mipsreg::V0), 0x8000000000000000ull);
  run(0x008510de, E);
  EXPECT_EQ(E.getRegister(mipsreg::V0), 0u);
  // The low product ignores arbitrary high bits. MUH requires sign extension;
  // MUHU and unsigned division also accept a zero-extended word.
  const auto ProductInput = [&](uint32_t Word, uint64_t Input) {
    E.setRegister(mipsreg::A0, Input);
    E.setRegister(mipsreg::A1, 3);
    EXPECT_EQ(decode(Word), 4);
    Ops.clear();
    Decode.liftUnitToLow(Unit, Ops);
    return llvm::all_of(Ops, [&](const auto &Op) { return E.step(Op); });
  };
  EXPECT_TRUE(ProductInput(0x00851098, 0x87654321ffffffffull));
  EXPECT_FALSE(ProductInput(0x008510d8, 0x00000000ffffffffull));
  EXPECT_TRUE(ProductInput(0x008510d9, 0x00000000ffffffffull));
  EXPECT_FALSE(ProductInput(0x008510d9, 0x87654321ffffffffull));
  ASSERT_EQ(decode(0xec580001, 0x100001004ull), 4); // LDPC from (PC & ~7) + 8
  Ops.clear();
  Decode.liftUnitToLow(Unit, Ops);
  ASSERT_EQ(Ops.front().Opcode, NdOp::LOAD);
  EXPECT_EQ(Ops.front().Inputs[0].Offset, 0x100001008ull);
}

TEST_P(MIPSRelease6,
       CompactTransfersUseNextPCAndLeaveTheFollowingInstructionIndependent) {
  const va_t PC = isMIPS64(GetParam()) ? 0x100001004ull : 0x1004;
  ASSERT_EQ(decode(0xebffffff, PC), 4); // BALC -4 -> this instruction
  EXPECT_FALSE(Unit.HasDelaySlot);
  EXPECT_EQ(Decode.directCallTarget(Unit.Head), PC);
  Decode.liftUnitToLow(Unit, Ops);
  ASSERT_EQ(Ops.back().Opcode, NdOp::CALL);
  EXPECT_EQ(Ops.back().Inputs[0].Offset, PC);
  EXPECT_TRUE(llvm::any_of(Ops, [&](const auto &O) {
    return O.Output.isReg() && O.Output.Offset == mipsreg::RA &&
           O.Inputs[0].isConst() && O.Inputs[0].Offset == PC + 4;
  }));
  ASSERT_EQ(decode(0xd81f0000, PC), 4); // JRC ra
  EXPECT_TRUE(Decode.isFunctionTerminator(Unit.Head));
  Ops.clear();
  Decode.liftUnitToLow(Unit, Ops);
  EXPECT_EQ(Ops.back().Opcode, NdOp::RETURN);
  ASSERT_EQ(decode(0x03e00009, PC),
            8); // the JALR zero,ra spelling still owns a slot
  EXPECT_TRUE(Unit.HasDelaySlot);
  Ops.clear();
  Decode.liftUnitToLow(Unit, Ops);
  EXPECT_EQ(Ops.back().Opcode, NdOp::RETURN);
  uint8_t NAL[4];
  llvm::support::endian::write32(NAL, 0x0410ffff,
                                 archLittleEndian(GetParam())
                                     ? llvm::endianness::little
                                     : llvm::endianness::big);
  DecodedInsn Physical{};
  ASSERT_EQ(Decode.decodeOne(NAL, sizeof(NAL), PC, Physical), 4);
  EXPECT_STREQ(Physical.Raw->mnemonic, "nal");
  EXPECT_STREQ(Physical.Raw->op_str, "");
  ASSERT_NE(Physical.Raw->detail, nullptr);
  EXPECT_EQ(Physical.Raw->detail->mips.op_count, 0);
  EXPECT_EQ(Physical.Raw->detail->regs_write[0], MIPS_REG_RA);
  ASSERT_EQ(decode(0x0410ffff, PC), 8); // NAL ignores the physical offset.
  EXPECT_TRUE(Unit.HasDelaySlot);
  Ops.clear();
  Decode.liftUnitToLow(Unit, Ops);
  EXPECT_EQ(Ops.back().Opcode, NdOp::BRANCH);
  EXPECT_EQ(Ops.back().Inputs[0].Offset, PC + 8);
  EXPECT_TRUE(llvm::any_of(Ops, [&](const auto &O) {
    return O.Output.isReg() && O.Output.Offset == mipsreg::RA &&
           O.Inputs[0].isConst() && O.Inputs[0].Offset == PC + 8;
  }));
  ASSERT_EQ(decode(0x20850002, PC),
            4); // BEQC a0,a1,PC+12 shares the old ADDI major.
  Ops.clear();
  Decode.liftUnitToLow(Unit, Ops);
  ASSERT_EQ(Ops.back().Opcode, NdOp::COND_BR);
  EXPECT_EQ(Ops.back().Inputs[0].Offset, PC + 12);
}

TEST_P(MIPSRelease6, ForbiddenSlotFaultDependsOnFallthrough) {
  const bool Little = archLittleEndian(GetParam());
  uint8_t Code[8];
  const auto Order = Little ? llvm::endianness::little : llvm::endianness::big;
  llvm::support::endian::write32(Code, 0x20850002, Order);     // BEQC a0,a1
  llvm::support::endian::write32(Code + 4, 0xd81f0000, Order); // JRC ra
  ASSERT_EQ(Decode.decodeUnitForLift(Code, sizeof(Code), 0x10000, Unit), 4);
  ASSERT_EQ(Unit.Head.MIPSForbiddenFollower, true);
  Decode.liftUnitToLow(Unit, Ops);
  for (bool Taken : {false, true}) {
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0, 1);
    E.setRegister(mipsreg::A1, Taken ? 1 : 2);
    bool Checked = false;
    for (const auto &O : Ops) {
      if (O.Opcode == NdOp::COND_BR)
        break;
      if (O.Opcode == NdOp::INTRINSIC) {
        EXPECT_EQ(E.step(O), Taken);
        Checked = true;
        break;
      }
      ASSERT_TRUE(E.step(O));
    }
    EXPECT_TRUE(Checked);
  }
  // An independent entry to that same JRC has no forbidden predecessor.
  ASSERT_EQ(Decode.decodeUnitForLift(Code + 4, 4, 0x10004, Unit), 4);
  EXPECT_EQ(Unit.Head.MIPSForbiddenFollower, false);
  ASSERT_EQ(Decode.decodeUnitForLift(Code, 4, 0x10000, Unit), 4);
  EXPECT_FALSE(Unit.Head.MIPSForbiddenFollower.has_value());
  Ops.clear();
  EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST_P(MIPSRelease6, RemovedLegacyEncodingsNeverGetTheirOldSemantics) {
  for (const auto Word :
       {0x00850018u, 0x70001002u, 0x88820000u, 0x0085100au, 0x03e00008u}) {
    SCOPED_TRACE(::testing::Message() << std::hex << Word);
    if (!decode(Word))
      continue;
    Ops.clear();
    EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops), UnliftedInstruction);
  }
}

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
TEST_P(MIPSRelease6, IntegerKernelsMatchRelease6HardwareModel) {
  const bool Wide = isMIPS64(GetParam()), Little = archLittleEndian(GetParam());
  uc_engine *Raw = nullptr;
  const auto Mode = static_cast<uc_mode>(
      (Wide ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
      (Little ? UC_MODE_LITTLE_ENDIAN : UC_MODE_BIG_ENDIAN));
  ASSERT_EQ(uc_open(UC_ARCH_MIPS, Mode, &Raw), UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> Reference(Raw, uc_close);
  const int CPU =
      Wide ? int(UC_CPU_MIPS64_I6400) : int(UC_CPU_MIPS32_MIPS32R6_GENERIC);
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, CPU), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  const auto Set = [&](int Reg, uint64_t Full) {
    const uint32_t Low = Full;
    ASSERT_EQ(uc_reg_write(Raw, Reg,
                           Wide ? static_cast<const void *>(&Full)
                                : static_cast<const void *>(&Low)),
              UC_ERR_OK);
  };
  const auto Get = [&](int Reg) -> uint64_t {
    uint64_t Full = 0;
    uint32_t Low = 0;
    EXPECT_EQ(uc_reg_read(Raw, Reg,
                          Wide ? static_cast<void *>(&Full)
                               : static_cast<void *>(&Low)),
              UC_ERR_OK);
    return Wide ? Full : Low;
  };
  constexpr uint32_t Words[] = {0x00851098, 0x008510d8, 0x008510d9, 0x0085109a,
                                0x008510da, 0x0085109b, 0x008510db, 0x00851035,
                                0x00851037, 0x7c041020, 0x7c8512a0, 0x00851045};
  for (unsigned I = 0; I != std::size(Words); ++I) {
    uint8_t Bytes[4];
    llvm::support::endian::write32(Bytes, Words[I],
                                   Little ? llvm::endianness::little
                                          : llvm::endianness::big);
    const auto PC = 0x10000 + 4 * I;
    ASSERT_EQ(uc_mem_write(Raw, PC, Bytes, 4), UC_ERR_OK);
    for (uint32_t X : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xf3e4a961u}) {
      for (uint32_t Y : {1u, 3u, 0x80000000u, 0xffffffffu}) {
        const uint64_t A = uint64_t(int64_t(int32_t(X))),
                       B = uint64_t(int64_t(int32_t(Y)));
        Set(UC_MIPS_REG_A0, A);
        Set(UC_MIPS_REG_A1, B);
        ASSERT_EQ(uc_emu_start(Raw, PC, PC + 4, 0, 1), UC_ERR_OK) << I;
        const auto Expected = Get(UC_MIPS_REG_V0);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(mipsreg::A0, A);
        E.setRegister(mipsreg::A1, B);
        run(Words[I], E);
        EXPECT_EQ(E.getRegister(mipsreg::V0), Expected)
            << std::hex << Words[I] << " inputs " << A << ", " << B;
      }
    }
  }
  // Linked GNU branch encodings. Use distinct target/fallthrough addresses:
  // the reference can issue a non-taken branch's following NOP in one step.
  constexpr uint32_t Branches[] = {0xd8800007, 0xf8800007, 0x20040007,
                                   0x60040007, // EQZ/NEZ, with and without link
                                   0x5c040007, 0x58040007, 0x58840007,
                                   0x5c840007, // GTZ/LEZ/GEZ/LTZ
                                   0x18040007, 0x18840007, 0x1c040007,
                                   0x1c840007, // LEZ/GEZ/GTZ/LTZ with link
                                   0x20850007, 0x60850007, 0x5c850007,
                                   0x58850007, // EQ/NE/LT/GE
                                   0x1c850007, 0x18850007, 0x20a40007,
                                   0x60a40007}; // LTU/GEU/OV/NOV
  for (unsigned I = 0; I < std::size(Branches); ++I) {
    const uint64_t PC = 0x10400 + 32 * I;
    uint8_t Code[8]{};
    llvm::support::endian::write32(Code, Branches[I],
                                   Little ? llvm::endianness::little
                                          : llvm::endianness::big);
    ASSERT_EQ(uc_mem_write(Raw, PC, Code, sizeof(Code)), UC_ERR_OK);
    ASSERT_EQ(Decode.decodeUnitForLift(Code, sizeof(Code), PC, Unit), 4);
    Ops.clear();
    Decode.liftUnitToLow(Unit, Ops);
    for (int64_t X :
         {int64_t(INT32_MIN), int64_t(-1), int64_t(0), int64_t(INT32_MAX)}) {
      for (int64_t Y :
           {int64_t(-1), int64_t(0), int64_t(1), int64_t(INT32_MAX)}) {
        SCOPED_TRACE(::testing::Message() << std::hex << Branches[I]
                                          << " inputs " << X << ", " << Y);
        const uint64_t Sentinel = 0x5555;
        Set(UC_MIPS_REG_A0, X);
        Set(UC_MIPS_REG_A1, Y);
        Set(UC_MIPS_REG_RA, Sentinel);
        ASSERT_EQ(uc_emu_start(Raw, PC, PC + 128, 0, 1), UC_ERR_OK);
        const auto Next = Get(UC_MIPS_REG_PC);
        const auto ReturnAddress = Get(UC_MIPS_REG_RA);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(mipsreg::A0, X);
        E.setRegister(mipsreg::A1, Y);
        E.setRegister(mipsreg::RA, Sentinel);
        bool Taken = false;
        for (const auto &O : Ops) {
          if (O.Opcode == NdOp::COND_BR) {
            const auto Condition = E.getRegister(O.Inputs[1].Offset);
            ASSERT_TRUE(Condition.has_value());
            if (*Condition) {
              Taken = O.Inputs[0].Offset == PC + 32;
              break;
            }
          } else if (O.Opcode == NdOp::CALL) {
            EXPECT_EQ(O.Inputs[0].Offset, PC + 32);
            Taken = true;
            break;
          } else {
            ASSERT_TRUE(E.step(O));
          }
        }
        EXPECT_EQ(Taken, Next == PC + 32);
        EXPECT_EQ(E.getRegister(mipsreg::RA), ReturnAddress);
      }
    }
  }
}
#endif

INSTANTIATE_TEST_SUITE_P(BothWidthsAndByteOrders, MIPSRelease6,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));
} // namespace
} // namespace neverd
