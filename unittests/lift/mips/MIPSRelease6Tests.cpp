//===- MIPSRelease6Tests.cpp - Release 6 scalar, CFG and decode tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/MIPS/MIPSCachePlatform.h"
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
  bool runSystem(uint32_t Word, NdOpEmulator &Emu, MIPSSystemContext &Context) {
    if (decode(Word) != 4)
      return false;
    Ops.clear();
    Decode.liftUnitToLow(Unit, Ops);
    for (const auto &Op : Ops) {
      if (Op.Opcode == NdOp::INTRINSIC &&
          Op.Inputs[0].Offset != unsigned(Intrinsic::MIPSPause)) {
        if (!Emu.stepMIPSSystem(Op, Context))
          return false;
      } else if (!Emu.step(Op))
        return false;
    }
    return true;
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

TEST_P(MIPSRelease6, FollowerEvidenceBelongsToOnePhysicalDecode) {
  uint8_t Code[8]{};
  const auto Order = archLittleEndian(GetParam()) ? llvm::endianness::little
                                                  : llvm::endianness::big;
  llvm::support::endian::write32(Code, 0x20850002, Order);
  llvm::support::endian::write32(Code + 4, 0xd81f0000, Order);
  DecodedInsn Physical{};
  ASSERT_EQ(Decode.decodeOne(Code, 8, 0x1000, Physical), 4);
  ASSERT_EQ(Physical.MIPSForbiddenFollower, true);
  EXPECT_EQ(Decode.decodeOneLight(Code, 1, 0x1000, Physical), 0);
  EXPECT_FALSE(Physical.MIPSForbiddenFollower.has_value());

  ASSERT_EQ(Decode.decodeOne(Code, 8, 0x1000, Physical), 4);
  ASSERT_EQ(Physical.MIPSForbiddenFollower, true);
  ASSERT_TRUE(Decode.init(Arch::AArch64));
  const uint8_t Add[] = {0x00, 0x04, 0x00, 0x91}; // ADD x0,x0,#1
  ASSERT_EQ(Decode.decodeOneForLift(Add, sizeof(Add), 0x1000, Physical), 4);
  EXPECT_FALSE(Physical.MIPSForbiddenFollower.has_value());
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

TEST_P(MIPSRelease6, AtomicDisassemblyAndRegisterAccessMatchThePhysicalBytes) {
  // GNU binutils 2.47, identical words in o32 and n64 objects.
  const bool Wide = isMIPS64(GetParam());
  struct Row {
    uint32_t Word;
    const char *Name;
    bool Paired;
    bool Store;
    bool Double;
  };
  constexpr Row Rows[] = {
      {0x7c82fc36, "ll", false, false, false},
      {0x7c82fc26, "sc", false, true, false},
      {0x7c82fc37, "lld", false, false, true},
      {0x7c82fc27, "scd", false, true, true},
      {0x7c821876, "llwp", true, false, false},
      {0x7c821866, "scwp", true, true, false},
      {0x7c821877, "lldp", true, false, true},
      {0x7c821867, "scdp", true, true, true},
  };
  for (const auto &R : Rows) {
    SCOPED_TRACE(R.Name);
    if (!Wide && R.Double) {
      EXPECT_EQ(decode(R.Word), 0);
      continue;
    }
    ASSERT_EQ(decode(R.Word), 4);
    const auto *Raw = Unit.Head.Raw;
    ASSERT_STREQ(Raw->mnemonic, R.Name);
    EXPECT_STREQ(Raw->op_str, R.Paired ? "$v0, $v1, ($a0)" : "$v0, -8($a0)");
    ASSERT_NE(Raw->detail, nullptr);
    const auto &M = Raw->detail->mips;
    ASSERT_EQ(M.op_count, R.Paired ? 3 : 2);
    EXPECT_EQ(M.operands[0].reg, MIPS_REG_V0);
    EXPECT_EQ(M.operands[0].access, R.Store ? CS_AC_READ_WRITE : CS_AC_WRITE);
    if (R.Paired) {
      EXPECT_EQ(M.operands[1].reg, MIPS_REG_V1);
      EXPECT_EQ(M.operands[1].access, R.Store ? CS_AC_READ : CS_AC_WRITE);
    }
    const auto &Mem = M.operands[M.op_count - 1];
    EXPECT_EQ(Mem.type, MIPS_OP_MEM);
    EXPECT_EQ(Mem.mem.base, MIPS_REG_A0);
    EXPECT_EQ(Mem.mem.disp, R.Paired ? 0 : -8);
    EXPECT_EQ(Mem.access, R.Store ? CS_AC_WRITE : CS_AC_READ);
  }
  EXPECT_EQ(decode(0x7c8218f6), 0); // reserved bits 10:7 in paired LL
  ASSERT_EQ(decode(0x7c821076), 4); // LLWP v0,v0,(a0) is unpredictable
  Ops.clear();
  EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST_P(MIPSRelease6,
       LoadLinkedAndStoreConditionalRespectWidthByteOrderAndInterference) {
  const bool Wide = isMIPS64(GetParam()), Little = archLittleEndian(GetParam());
  const va_t VA = Wide ? 0x100002000ull : 0x2000;
  Segment Data;
  Data.VA = VA;
  Data.Size = Data.FileSz = 64;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  for (unsigned I = 0; I < Data.Size; ++I)
    Data.Data.push_back(0x81 + I);
  Image.Segments.push_back(Data);
  for (bool Double : {false, true}) {
    if (Double && !Wide)
      continue;
    const uint16_t Element = Double ? 8 : 4;
    for (bool Paired : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Element << " paired " << Paired);
      const uint32_t LL = (Paired ? 0x7c821876 : 0x7c82fc36) + Double;
      const uint32_t SC = LL ^ 0x10;
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      ASSERT_TRUE(E.setMIPSReservationModel(4, Paired));
      E.setRegister(mipsreg::A0, VA + (Paired ? 0 : 8));
      const auto Half = [&](unsigned Index) {
        const unsigned Position = Paired && !Little ? 1 - Index : Index;
        uint64_t V = 0;
        for (unsigned I = 0; I < Element; ++I)
          V |= uint64_t(Data.Data[Position * Element +
                                  (Little ? I : Element - I - 1)])
               << (8 * I);
        return Wide && !Double ? uint64_t(int64_t(int32_t(V))) : V;
      };
      run(LL, E);
      EXPECT_EQ(E.getRegister(mipsreg::V0), Half(0));
      if (Paired)
        EXPECT_EQ(E.getRegister(mipsreg::V1), Half(1));
      constexpr uint64_t NewLow = 0x0172635485abcdefull,
                         NewHigh = 0xf987654387654321ull;
      E.setRegister(mipsreg::V0, Wide ? NewLow : uint32_t(NewLow));
      E.setRegister(mipsreg::V1, Wide ? NewHigh : uint32_t(NewHigh));
      run(SC, E);
      EXPECT_EQ(E.getRegister(mipsreg::V0), 1u);
      EXPECT_EQ(E.getRegister(mipsreg::V1), Wide ? NewHigh : uint32_t(NewHigh));
      // A new linked read checks all bytes of the atomic write and its order.
      run(LL, E);
      const auto GPR = [&](uint64_t V) -> uint64_t {
        return Double ? V : Wide ? uint64_t(int64_t(int32_t(V))) : uint32_t(V);
      };
      EXPECT_EQ(E.getRegister(mipsreg::V0), GPR(NewLow));
      if (Paired)
        EXPECT_EQ(E.getRegister(mipsreg::V1), GPR(NewHigh));
      LowOp Write;
      Write.Opcode = NdOp::STORE;
      Write.addInput(
          NdVar::cst(VA + Element * (Paired ? 2 : 1) - 1, Wide ? 8 : 4));
      Write.addInput(NdVar::cst(0x12, 1));
      ASSERT_TRUE(E.step(Write));
      run(SC, E);
      EXPECT_EQ(E.getRegister(mipsreg::V0), 0u);
      run(SC, E);
      EXPECT_EQ(E.getRegister(mipsreg::V0), 0u);
    }
  }
}

TEST_P(MIPSRelease6, PairedReservationNeedsXNPAndValidMappingBeforeAnyEffect) {
  const bool Wide = isMIPS64(GetParam());
  const va_t VA = Wide ? 0x200002000ull : 0x2000;
  Segment Data;
  Data.VA = VA;
  Data.Size = Data.FileSz = 32;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(32, 0x34);
  Image.Segments.push_back(Data);
  NdOpEmulator E(Image);
  auto Execute = [&](uint32_t Word) {
    EXPECT_EQ(decode(Word), 4);
    Ops.clear();
    Decode.liftUnitToLow(Unit, Ops);
    return llvm::all_of(Ops, [&](const auto &Op) { return E.step(Op); });
  };
  ASSERT_TRUE(E.setMIPSReservationModel(16));
  E.setRegister(mipsreg::A0, VA);
  EXPECT_FALSE(Execute(0x7c821876)); // Config5.XNP unknown
  ASSERT_TRUE(E.setMIPSReservationModel(16, true));
  E.setRegister(mipsreg::V0, 91);
  E.setRegister(mipsreg::V1, 92);
  EXPECT_FALSE(Execute(0x7c821866)); // no incoming LLBit evidence
  EXPECT_EQ(E.getRegister(mipsreg::V0), 91u);
  ASSERT_TRUE(Execute(0x7c800076)); // zero,zero still establishes LLBit
  E.setRegister(mipsreg::A0, VA + 1);
  EXPECT_FALSE(Execute(0x7c821866)); // rejected SC leaves the old reservation
  E.setRegister(mipsreg::A0, VA + 32);
  EXPECT_FALSE(Execute(0x7c821866));
  E.setRegister(mipsreg::A0, VA);
  EXPECT_FALSE(Execute(Wide ? 0x7c820027 : 0x7c820026)); // mismatch, same VA
  ASSERT_TRUE(Execute(0x7c821866));
  EXPECT_EQ(E.getRegister(mipsreg::V0), 1u);
  ASSERT_TRUE(Execute(0x7c821876));
  E.setRegister(mipsreg::A0, VA + 16);
  ASSERT_TRUE(Execute(0x7c821866)); // a different mapped VA gives known failure
  EXPECT_EQ(E.getRegister(mipsreg::V0), 0u);
}

TEST_P(MIPSRelease6, SelectedHardwareRegisterDetailDoesNotRequireText) {
  for (bool Text : {false, true}) {
    Decode.setText(Text);
    ASSERT_EQ(decode(0x7c0220fb), 4); // RDHWR v0,$4,3
    auto *Raw = Unit.Head.Raw;
    ASSERT_NE(Raw->detail, nullptr);
    const auto &Operands = Raw->detail->mips;
    ASSERT_EQ(Operands.op_count, 3);
    EXPECT_EQ(Operands.operands[0].reg, MIPS_REG_V0);
    EXPECT_EQ(Operands.operands[0].access, CS_AC_WRITE);
    EXPECT_EQ(Operands.operands[1].reg, MIPS_REG_HWR4);
    EXPECT_EQ(Operands.operands[1].access, CS_AC_READ);
    EXPECT_EQ(Operands.operands[2].type, MIPS_OP_IMM);
    EXPECT_EQ(Operands.operands[2].imm, 3);
    if (Text)
      EXPECT_STREQ(Raw->op_str, "$v0, $4, 3");
  }
}

TEST_P(MIPSRelease6, CP0UsesTheRequestedViewAndHardwareSelector) {
  class Board final : public MIPSCP0Platform {
  public:
    uint64_t Entry = 0xfedcba9887654321ull;
    bool MVH = false;
    std::optional<uint32_t> readWord(uint8_t Key,
                                     MIPSSystemContext &) override {
      if (Key != 2)
        return std::nullopt;
      // MFC0 of EntryLo moves RI/XI from bits 63:62 to bits 31:30.
      return uint32_t(Entry & 0x3fffffff) | uint32_t(Entry >> 32 & 0xc0000000);
    }
    bool writeWord(uint8_t Key, uint32_t V, MIPSSystemContext &) override {
      if (Key != 2)
        return false;
      Entry = (Entry & 0x3fffffff00000000ull) |
              (uint64_t(V & 0xc0000000) << 32) | (V & 0x3fffffff);
      return true;
    }
    std::optional<uint64_t> readExtended(uint8_t Key, MIPSCP0ExtendedView View,
                                         MIPSSystemContext &) override {
      if (Key != 2 || (View == MIPSCP0ExtendedView::HighWord && !MVH))
        return std::nullopt;
      return View == MIPSCP0ExtendedView::Doubleword ? Entry
                                                     : uint32_t(Entry >> 32);
    }
    bool writeExtended(uint8_t Key, MIPSCP0ExtendedView View, uint64_t Value,
                       MIPSSystemContext &) override {
      if (Key != 2 || (View == MIPSCP0ExtendedView::HighWord && !MVH))
        return false;
      Entry = View == MIPSCP0ExtendedView::Doubleword
                  ? Value
                  : ((Entry & UINT32_MAX) | (Value << 32));
      return true;
    }
  } B;
  const bool Wide = isMIPS64(GetParam());
  auto SignWord = [&](uint32_t V) -> uint64_t {
    return Wide ? uint64_t(int64_t(int32_t(V))) : V;
  };
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  MIPSSystemContext C;
  C.CP0 = &B;
  C.Status = 0x80000001;
  EXPECT_FALSE(runSystem(0x40021000, E, C));
  C.CP0Usable = true;
  ASSERT_TRUE(runSystem(0x40021000, E, C)); // MFC0, full declared word view
  EXPECT_EQ(E.getRegister(mipsreg::V0), SignWord(0xc7654321));
  EXPECT_FALSE(runSystem(0x40421000, E, C)); // MFHC0 without MVH observation
  B.MVH = true;
  ASSERT_TRUE(runSystem(0x40421000, E, C));
  EXPECT_EQ(E.getRegister(mipsreg::V0), SignWord(0xfedcba98));
  E.setRegister(mipsreg::V0, 0x11223344);
  ASSERT_TRUE(runSystem(0x40c21000, E, C)); // MTHC0
  EXPECT_TRUE(C.ExecutionHazardPending);
  EXPECT_FALSE(runSystem(0x40021000, E, C));
  ASSERT_TRUE(runSystem(0xc0, E, C)); // EHB
  EXPECT_EQ(B.Entry, 0x1122334487654321ull);
  E.setRegister(mipsreg::V0, 0xc1122334);
  ASSERT_TRUE(runSystem(0x40821000, E, C));
  ASSERT_TRUE(runSystem(0xc0, E, C));
  EXPECT_EQ(B.Entry, 0xd122334401122334ull);
  if (Wide) {
    constexpr uint64_t Full = 0x8011223344556677;
    E.setRegister(mipsreg::V0, Full);
    ASSERT_TRUE(runSystem(0x40a21000, E, C)); // DMTC0
    ASSERT_TRUE(runSystem(0xc0, E, C));
    ASSERT_TRUE(runSystem(0x40221000, E, C)); // DMFC0
    EXPECT_EQ(E.getRegister(mipsreg::V0), Full);
  }
  ASSERT_TRUE(
      runSystem(0x41626000, E, C)); // DI, then EI after resolving hazard
  EXPECT_EQ(E.getRegister(mipsreg::V0), SignWord(0x80000001));
  EXPECT_EQ(C.Status, 0x80000000u);
  ASSERT_TRUE(runSystem(0xc0, E, C));
  ASSERT_TRUE(runSystem(0x41626020, E, C));
  EXPECT_EQ(C.Status, 0x80000001u);
  ASSERT_TRUE(runSystem(0xc0, E, C));
  C.CP0Usable = false;
  C.Hardware = MIPSHardwareObservation{4, 0xabcdef0187654321, 3};
  EXPECT_FALSE(runSystem(0x7c0220fb, E, C));
  C.HWREna = 1u << 4;
  EXPECT_FALSE(runSystem(0x7c0220bb, E, C)); // correct register, wrong selector
  ASSERT_TRUE(runSystem(0x7c0220fb, E, C));
  EXPECT_EQ(E.getRegister(mipsreg::V0),
            Wide ? 0xabcdef0187654321ull : 0x87654321u);
  EXPECT_FALSE(C.Hardware.has_value());
  C.HWREna = 1u << 5;
  C.Hardware = MIPSHardwareObservation{5, 1, 0};
  ASSERT_TRUE(runSystem(0x7c02283b, E, C));
  EXPECT_EQ(E.getRegister(mipsreg::V0), 1u);
  ASSERT_TRUE(E.setMIPSReservationModel(16, true));
  C.Hardware = MIPSHardwareObservation{5, 1, 0};
  EXPECT_FALSE(runSystem(0x7c02283b, E, C)); // contradicts the bound XNP=0
  C.Hardware = MIPSHardwareObservation{5, 0, 0};
  ASSERT_TRUE(runSystem(0x7c02283b, E, C));
}

TEST_P(MIPSRelease6, CacheAndPrefetchKeepRevisionAndTheEntireAddress) {
  class Board final : public MIPSCachePlatform {
  public:
    uint64_t Last = 0;
    bool Wide = false;
    MIPSCacheMapping Mapping = MIPSCacheMapping::Cached;
    std::optional<MIPSCacheRequest> Request;
    std::optional<MIPSCacheTranslation> translateDataRead(uint32_t V) override {
      Last = V;
      Wide = false;
      return MIPSCacheTranslation{Mapping, V};
    }
    std::optional<MIPSCacheTranslation>
    translateDataRead64(uint64_t V) override {
      Last = V;
      Wide = true;
      return MIPSCacheTranslation{Mapping, V};
    }
    bool apply(const MIPSCacheRequest &R) override {
      Request = R;
      return true;
    }
  } B;
  const bool Wide = isMIPS64(GetParam());
  const uint64_t VA = Wide ? 0x123400002008ull : 0x2008;
  NdOpEmulator E(Image);
  E.setRegister(mipsreg::A0, VA);
  MIPSSystemContext C;
  C.Cache = &B;
  EXPECT_FALSE(runSystem(0x7c83fc25, E, C));
  C.CP0Usable = true;
  ASSERT_TRUE(runSystem(0x7c83fc25, E, C)); // CACHE 3,-8(a0)
  EXPECT_EQ(B.Wide, Wide);
  EXPECT_EQ(B.Last, VA - 8);
  ASSERT_TRUE(B.Request);
  EXPECT_EQ(B.Request->VirtualAddress, VA - 8);
  EXPECT_EQ(B.Request->Action, MIPSCacheAction::IndexWritebackInvalidate);
  EXPECT_EQ(B.Request->ISARevision, 6);
  B.Request.reset();
  C.CP0Usable = false;
  ASSERT_TRUE(runSystem(0x7c91fc35, E, C)); // PREF L3 store, -8(a0)
  ASSERT_TRUE(B.Request);
  EXPECT_EQ(B.Request->Encoding, 17);
  EXPECT_EQ(B.Request->Action, MIPSCacheAction::Prefetch);
  B.Request.reset();
  B.Mapping = MIPSCacheMapping::AddressException;
  EXPECT_TRUE(runSystem(0x7c91fc35, E, C));
  EXPECT_FALSE(B.Request);
  ASSERT_EQ(decode(0x7c98fc35), 4);
  Ops.clear();
  EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops),
               UnliftedInstruction); // reserved R6 hint 24
  ASSERT_TRUE(E.setMIPSReservationModel(16));
  EXPECT_FALSE(runSystem(0x140, E, C)); // PAUSE needs a cleared LLBit
  E.clearMIPSReservation();
  EXPECT_TRUE(runSystem(0x140, E, C));
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
  // Run real LL/SC at one PC interval. Compare the success bit, the value
  // observed by LL, and a second linked read of the committed memory.
  Segment Data;
  Data.VA = 0x20000;
  Data.Size = Data.FileSz = 32;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(Data);
  ASSERT_EQ(uc_mem_map(Raw, Data.VA, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  const auto Order = Little ? llvm::endianness::little : llvm::endianness::big;
  for (bool Double : {false, true}) {
    if (Double && !Wide)
      continue;
    const uint32_t LL = 0x7c82fc36 + Double, SC = 0x7ca2fc26 + Double;
    const auto PC = 0x10800 + (Double ? 0x40 : 0);
    uint8_t Code[20];
    const uint32_t Insns[] = {LL, 0x00401825, 0x38420055, SC, LL};
    for (unsigned J = 0; J < std::size(Insns); ++J)
      llvm::support::endian::write32(Code + 4 * J, Insns[J], Order);
    ASSERT_EQ(uc_mem_write(Raw, PC, Code, sizeof(Code)), UC_ERR_OK);
    for (unsigned Seed : {0u, 0x70u, 0x80u, 0xffu}) {
      for (bool Same : {false, true}) {
        auto &Bytes = Image.Segments.back().Data;
        for (unsigned I = 0; I < Bytes.size(); ++I)
          Bytes[I] = Seed + 31 * I;
        ASSERT_EQ(uc_mem_write(Raw, Data.VA, Bytes.data(), Bytes.size()),
                  UC_ERR_OK);
        const auto Base = Data.VA + 8, Destination = Base + (Same ? 0 : 16);
        Set(UC_MIPS_REG_A0, Base);
        Set(UC_MIPS_REG_A1, Destination);
        ASSERT_EQ(uc_emu_start(Raw, PC, PC + 16, 0, 4), UC_ERR_OK);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        ASSERT_TRUE(E.setMIPSReservationModel(16));
        E.setRegister(mipsreg::A0, Base);
        E.setRegister(mipsreg::A1, Destination);
        for (unsigned I = 0; I < 4; ++I)
          run(Insns[I], E);
        EXPECT_EQ(E.getRegister(mipsreg::V1), Get(UC_MIPS_REG_V1));
        EXPECT_EQ(E.getRegister(mipsreg::V0), Get(UC_MIPS_REG_V0));
        ASSERT_EQ(uc_emu_start(Raw, PC + 16, PC + 20, 0, 1), UC_ERR_OK);
        run(LL, E);
        EXPECT_EQ(E.getRegister(mipsreg::V0), Get(UC_MIPS_REG_V0));
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

TEST_P(MIPSRelease6, FR1TransfersPreserveOnlyArchitecturallyDefinedBits) {
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegister(mipsreg::A0, 0x80000001);
  E.setRegister(mipsreg::A1, 0xfff12345);
  run(0x44841000, E); // mtc1 a0,f2
  EXPECT_FALSE(E.getRegister(mipsreg::w(2)));
  run(0x44021000, E); // mfc1 v0,f2
  EXPECT_EQ(E.getRegister(mipsreg::V0),
            isMIPS64(GetParam()) ? 0xffffffff80000001ull : 0x80000001ull);
  run(0x44e51000, E); // mthc1 a1,f2
  EXPECT_EQ(E.getRegister(mipsreg::w(2)), 0xfff1234580000001ull);
  run(0x44621000, E); // mfhc1 v0,f2
  EXPECT_EQ(E.getRegister(mipsreg::V0),
            isMIPS64(GetParam()) ? 0xfffffffffff12345ull : 0xfff12345ull);
  if (isMIPS64(GetParam())) {
    run(0x44221000, E); // dmfc1 v0,f2
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0xfff1234580000001ull);
  }
}
TEST_P(MIPSRelease6, ScalarClassAndSelectorsUseRawNaNsAndPredicateBitZero) {
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  for (unsigned Size : {4u, 8u}) {
    const uint64_t Sign = uint64_t(1) << (Size * 8 - 1),
                   Inf = Size == 4 ? 0x7f800000ull : 0x7ff0000000000000ull;
    const uint64_t Quiet = (Sign - Inf) / 2;
    const uint64_t Cases[] = {Inf | 1,    Inf | Quiet,
                              Sign | Inf, Sign | (Inf >> 1),
                              Sign | 1,   Sign,
                              Inf,        Inf >> 1,
                              1,          0};
    for (unsigned I = 0; I < std::size(Cases); ++I) {
      E.setRegister(mipsreg::w(2), Cases[I]);
      run((Size == 4 ? 0x4600111bu : 0x4620111bu), E); // class.fmt f4,f2
      run(0x44022000, E);
      EXPECT_EQ(E.getRegister(mipsreg::V0), uint64_t(1) << I);
    }
  }
  E.setRegister(mipsreg::w(2), 0x7ff0123456789abc);
  E.setRegister(mipsreg::w(3), 0x8000000000000000);
  E.setRegister(mipsreg::w(4), 2);
  run(0x46231110, E); // sel.d f4,f2,f3
  EXPECT_EQ(E.getRegister(mipsreg::w(4)), 0x7ff0123456789abcull);
  E.setRegister(mipsreg::w(4), 3);
  run(0x46231110, E);
  EXPECT_EQ(E.getRegister(mipsreg::w(4)), 0x8000000000000000ull);
  run(0x46201107, E); // neg.d preserves the NaN payload
  EXPECT_EQ(E.getRegister(mipsreg::w(4)), 0xfff0123456789abcull);
  EXPECT_EQ(decode(0x45220003), 8); // bc1eqz f2,PC+16
  ASSERT_TRUE(Unit.HasDelaySlot);
  Ops.clear();
  Decode.liftUnitToLow(Unit, Ops);
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &O) {
    return O.Opcode == NdOp::COND_BR && O.Inputs[0].Offset == 0x10010;
  }));
}

INSTANTIATE_TEST_SUITE_P(BothWidthsAndByteOrders, MIPSRelease6,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));
} // namespace
} // namespace neverd
