//===- MIPSControlFlowTests.cpp - Delayed MIPS control regression --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check execution order, byte ownership, annulment and independently entered
/// slots using hand-authored machine words with known architectural outcomes.

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/Support/Error.h"

#include <vector>

using namespace neverd;

namespace {

std::vector<uint8_t> encode(llvm::ArrayRef<uint32_t> Words, Arch Target) {
  std::vector<uint8_t> Bytes;
  for (uint32_t Word : Words)
    for (unsigned I = 0; I < 4; ++I)
      Bytes.push_back(
          uint8_t(Word >> (8 * (archLittleEndian(Target) ? I : 3 - I))));
  return Bytes;
}

BinaryImage image(llvm::ArrayRef<uint32_t> Words, Arch Target) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = isMIPS64(Target) ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Base = Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Base;
  Text.Data = encode(Words, Target);
  Text.Size = Text.FileSz = Text.Data.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Text));
  return Image;
}

LowFunc build(const BinaryImage &Image) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(Image));
  CFGBuilder Builder;
  auto Func = Builder.build(Image, Decode, Image.Entry, "delayed");
  auto Error = validateLowInstructionBoundaries(
      Func, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  return Func;
}

class MIPSControl : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSControl, DelayedUnitOwnsBothInstructionsAndActiveOperandDetails) {
  const auto Code = encode({0x0c000408, 0x8ca2fff4, 0}, GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), Code.size(), 0x1000, Unit),
            8);
  ASSERT_TRUE(Unit.HasDelaySlot);
  DecodedInsn Other{};
  ASSERT_EQ(Decode.decodeOne(Code.data() + 8, 4, 0x1008, Other), 4);

  for (const auto *Saved : {&Unit.Head, &Unit.Slot}) {
    DecodedInsn Physical{};
    ASSERT_EQ(Decode.decodeOne(Code.data() + (Saved->Addr - 0x1000), 4,
                               Saved->Addr, Physical),
              4);
    EXPECT_NE(Saved->Raw, Physical.Raw);
    EXPECT_EQ(Saved->Id, Physical.Id);
    ASSERT_NE(Saved->Raw->detail, nullptr);
    ASSERT_NE(Physical.Raw->detail, nullptr);
    const auto &A = *Saved->Raw->detail, &B = *Physical.Raw->detail;
    EXPECT_NE(&A, &B);
    EXPECT_EQ(llvm::ArrayRef(A.regs_read, A.regs_read_count),
              llvm::ArrayRef(B.regs_read, B.regs_read_count));
    EXPECT_EQ(llvm::ArrayRef(A.regs_write, A.regs_write_count),
              llvm::ArrayRef(B.regs_write, B.regs_write_count));
    EXPECT_EQ(llvm::ArrayRef(A.groups, A.groups_count),
              llvm::ArrayRef(B.groups, B.groups_count));
    EXPECT_EQ(A.writeback, B.writeback);
    ASSERT_EQ(A.mips.op_count, B.mips.op_count);
    for (unsigned I = 0; I < B.mips.op_count; ++I) {
      const auto &X = A.mips.operands[I], &Y = B.mips.operands[I];
      ASSERT_EQ(X.type, Y.type);
      EXPECT_EQ(X.access, Y.access);
      switch (X.type) {
      case MIPS_OP_REG:
        EXPECT_EQ(X.reg, Y.reg);
        break;
      case MIPS_OP_IMM:
        EXPECT_EQ(X.imm, Y.imm);
        break;
      case MIPS_OP_MEM:
        EXPECT_EQ(X.mem.base, Y.mem.base);
        EXPECT_EQ(X.mem.disp, Y.mem.disp);
        break;
      default:
        FAIL() << "unexpected operand in JAL/LW fixture";
      }
    }
  }
}

TEST_P(MIPSControl, SourceReturnRequiresTheWholeIncomingLink) {
  const bool Wide = isMIPS64(GetParam());
  const auto Check = [&](llvm::ArrayRef<uint32_t> Words, bool Preserved) {
    auto Image = image(Words, GetParam());
    Image.MIPS.emplace();
    Image.MIPS->HasDSP = true;
    Image.MIPS->ABI = Wide ? MIPSABI::N64 : MIPSABI::O32;
    const auto Low = build(Image);
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    for (bool WithImage : {false, true}) {
      SCOPED_TRACE(WithImage);
      LowToMedConverter Convert;
      if (WithImage)
        Convert.setBinaryImage(&Image);
      if (Preserved)
        EXPECT_NO_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF));
      else
        EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
                     LowToMedConversionError);
    }
  };
  constexpr uint32_t ClearRA = 0x0000f825, Return = 0x03e00008;
  Check({Return, 0}, true);
  Check({ClearRA, Return, 0}, false);
  // sw/lw ra,-4(sp) preserves an o32 link and truncates an n64 link.
  Check({0xafbffffc, ClearRA, 0x8fbffffc, Return, 0}, !Wide);
  if (Wide)
    Check({0xffbffff8, ClearRA, 0xdfbffff8, Return, 0}, true);
}

TEST_P(MIPSControl, ReusedExecutionUnitClearsAnOldSlotAndFailedHead) {
  const auto Code = encode({0x0c000408, 0x8ca2fff4, 0}, GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), Code.size(), 0x1000, Unit),
            8);
  ASSERT_TRUE(Unit.HasDelaySlot);
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data() + 8, 4, 0x1008, Unit), 4);
  EXPECT_FALSE(Unit.HasDelaySlot);
  EXPECT_EQ(Unit.Slot.Raw, nullptr);
  EXPECT_EQ(Unit.Slot.Size, 0);
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), 1, 0x1010, Unit), 0);
  EXPECT_EQ(Unit.Head.Raw, nullptr);
  EXPECT_EQ(Unit.Head.Size, 0);
}

TEST_P(MIPSControl,
       AReinitializedDecoderUsesTheNewInstructionAddressWidthAndAlignment) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  const auto Nop = encode({0}, GetParam());
  DecodedInsn Insn{};
  EXPECT_EQ(Decode.decodeOne(Nop.data(), 4, 0x1002, Insn), 0);
  EXPECT_EQ(Decode.decodeOneLight(Nop.data(), 4, 0x100000000ull, Insn),
            isMIPS64(GetParam()) ? 4 : 0);
  if (!isMIPS64(GetParam())) {
    ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
    const uint8_t Compact[] = {
        uint8_t(archLittleEndian(GetParam()) ? 0 : 0x65),
        uint8_t(archLittleEndian(GetParam()) ? 0x65 : 0)};
    EXPECT_EQ(Decode.decodeOne(Compact, 2, 0xfffffffe, Insn), 2);
    EXPECT_EQ(Decode.decodeOneLight(Compact, 2, 0xffffffff, Insn), 0);
  }
  ASSERT_TRUE(Decode.init(Arch::X64));
  const uint8_t X86Nop = 0x90;
  EXPECT_EQ(Decode.decodeOne(&X86Nop, 1, 0x100000001ull, Insn), 1);
}

TEST_P(MIPSControl, WideCallResultKeepsTheO32RegisterWordOrder) {
  if (isMIPS64(GetParam()))
    return;
  MedFunc Function;
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.back();
  Block.Id = 0;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x1000;
  Call.addInput(MedVar::makeConst(0x2000, 4));
  Call.Output.Kind = MedVar::Reg;
  Call.Output.RegOff = mipsreg::V0;
  Call.Output.Size = 4;
  Call.Output.Id = 1;
  Call.Output.SSAVer = 1;
  Block.Ops.push_back(Call);
  const std::set<va_t> WideCallees = {0x2000};
  modelCallWideIntReturn(Function, GetParam(), &WideCallees);
  ASSERT_EQ(Block.Ops.size(), 3u);
  constexpr uint64_t Result = 0x1122334455667788ull;
  for (unsigned I = 1; I <= 2; ++I) {
    const auto &Extract = Block.Ops[I];
    ASSERT_EQ(Extract.Opcode, NdOp::SUBBYTES);
    ASSERT_EQ(Extract.NumInputs, 2);
    ASSERT_TRUE(Extract.Inputs[1].isConst());
    const uint32_t Word = Result >> (8 * Extract.Inputs[1].ConstVal);
    const bool First = Extract.Output.RegOff == mipsreg::V0;
    const bool High = First != archLittleEndian(GetParam());
    EXPECT_EQ(Word, High ? 0x11223344u : 0x55667788u);
  }
}

TEST_P(MIPSControl, PhysicalDecodeAndOwnedExecutionUnitsRemainDistinct) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  const auto Bytes = encode({0x03e00008, 0x27e20001, 0x24030002}, GetParam());
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), 0x1000, Unit),
            8);
  EXPECT_EQ(Unit.Head.Size, 4);
  EXPECT_EQ(Unit.Slot.Size, 4);
  DecodedInsn Other;
  ASSERT_EQ(Decode.decodeOneForLift(Bytes.data() + 8, 4, 0x1008, Other), 4);
  std::vector<LowOp> Ops;
  auto Member = Decode.liftUnitToLow(Unit, Ops);
  ASSERT_TRUE(Member);
  BinaryImage Image;
  Image.Arch = GetParam();
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(mipsreg::RA, 0x3000);
  for (const auto &Op : Ops) {
    if (Op.Opcode == NdOp::RETURN) {
      ASSERT_EQ(Op.NumInputs, 1);
      EXPECT_EQ(Emulator.getRegister(Op.Inputs[0].Offset), 0x3000u);
    } else {
      ASSERT_TRUE(Emulator.step(Op));
    }
  }
  EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 0x3001u);
  EXPECT_EQ(Decode.decodeUnitForLift(Bytes.data(), 4, 0x1000, Unit), 0);
  EXPECT_EQ(Decode.decodeUnitForLift(Bytes.data(), 7, 0x1000, Unit), 0);
}

TEST_P(MIPSControl, ConditionAndIndirectTargetAreCapturedBeforeSlotWrites) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  for (const auto Words : {std::vector<uint32_t>{0x10800003, 0x24040001},
                           std::vector<uint32_t>{0x00800008, 0x24040001}}) {
    const auto Bytes = encode(Words, GetParam());
    DecodedExecutionUnit Unit;
    ASSERT_EQ(
        Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), 0x1000, Unit), 8);
    std::vector<LowOp> Ops;
    ASSERT_TRUE(Decode.liftUnitToLow(Unit, Ops));
    BinaryImage Image;
    Image.Arch = GetParam();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::r(4), 0);
    for (const auto &Op : Ops) {
      if (Op.Opcode == NdOp::COND_BR)
        EXPECT_EQ(Emulator.getRegister(Op.Inputs[1].Offset), 1u);
      else if (Op.Opcode == NdOp::INDIR_BR)
        EXPECT_EQ(Emulator.getRegister(Op.Inputs[0].Offset), 0u);
      else
        ASSERT_TRUE(Emulator.step(Op));
    }
    EXPECT_EQ(Emulator.getRegister(mipsreg::r(4)), 1u);
  }
}

TEST_P(MIPSControl, LinkIsVisibleToSlotAndSlotCannotTransferControl) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  // jal 0x2000; addu a0,ra,zero
  auto Bytes = encode({0x0c000800, 0x03e02021}, GetParam());
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), 0x1000, Unit),
            8);
  std::vector<LowOp> Ops;
  ASSERT_TRUE(Decode.liftUnitToLow(Unit, Ops));
  BinaryImage Image;
  Image.Arch = GetParam();
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(mipsreg::RA, 0x3000);
  for (const auto &Op : Ops)
    if (Op.Opcode != NdOp::CALL)
      ASSERT_TRUE(Emulator.step(Op));
  EXPECT_EQ(Emulator.getRegister(mipsreg::r(4)), 0x1008u);
  Bytes = encode({0x0c000800, 0x03e00008}, GetParam());
  ASSERT_EQ(Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), 0x1000, Unit),
            8);
  Ops.clear();
  EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST_P(MIPSControl, LikelyBranchAnnulsSlotBeforeSSA) {
  // beql a0,zero,0x1010; addiu v0,zero,7
  // jr ra; addiu v0,zero,9
  // jr ra; nop
  const auto Image =
      image({0x50800003, 0x24020007, 0x03e00008, 0x24020009, 0x03e00008, 0},
            GetParam());
  const auto Func = build(Image);
  ASSERT_FALSE(Func.Blocks.empty());
  const auto &Entry = Func.Blocks.front();
  ASSERT_EQ(Entry.InstructionBoundaries.size(), 1u);
  EXPECT_TRUE(Entry.InstructionBoundaries.front().DelaySlot);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Entry.InstructionBoundaries.front().ControlFlags,
      LowInstructionControlFlag::InstructionGuard));
  for (unsigned Input : {0u, 1u}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::r(4), Input);
    Emulator.setRegister(mipsreg::V0, 3);
    Emulator.run(Entry);
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Input ? 3u : 7u);
  }
  LowToMedConverter Converter;
  const auto Med = Converter.convert(Func, GetParam());
  // The slot definition belongs to a distinct guarded path after conversion.
  bool FoundSeven = false;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        FoundSeven |= Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == 7;
  EXPECT_TRUE(FoundSeven);
}

TEST_P(MIPSControl, LikelySlotStoreIsGuardedInLowAndMedIR) {
  // beql a0,zero,0x1010; sw v0,0(a1); jr ra; nop; jr ra; nop
  auto Image =
      image({0x50800003, 0xaca20000, 0x03e00008, 0, 0x03e00008, 0}, GetParam());
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 16;
  Data.Data.resize(16);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));
  const auto Function = build(Image);
  ASSERT_FALSE(Function.Blocks.empty());
  for (unsigned Input : {0u, 1u}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::r(4), Input);
    Emulator.setRegister(mipsreg::r(5), 0x2000);
    Emulator.setRegister(mipsreg::V0, 0x12345678);
    Emulator.run(Function.Blocks.front());
    LowOp Read;
    Read.Opcode = NdOp::LOAD;
    Read.Output = NdVar::reg(mipsreg::V1, 4);
    Read.addInput(NdVar::cst(0x2000, isMIPS64(GetParam()) ? 8 : 4));
    ASSERT_TRUE(Emulator.step(Read));
    EXPECT_EQ(Emulator.getRegister(mipsreg::V1), Input ? 0u : 0x12345678u);
  }
  LowToMedConverter Converter;
  const auto Med = Converter.convert(Function, GetParam());
  ASSERT_GE(Med.Blocks.size(), 3u);
  int StoreBlock = -1;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::STORE) {
        EXPECT_EQ(StoreBlock, -1);
        StoreBlock = Block.Id;
        EXPECT_EQ(Op.Addr, 0x1004u);
      }
  ASSERT_GE(StoreBlock, 0);
  const auto &Guard = Med.Blocks.front();
  EXPECT_NE(StoreBlock, Guard.Id);
  ASSERT_EQ(Guard.Succs.size(), 2u);
  EXPECT_TRUE(Guard.Succs[0] == StoreBlock || Guard.Succs[1] == StoreBlock);
  ASSERT_FALSE(Guard.Ops.empty());
  EXPECT_EQ(Guard.Ops.back().Opcode, NdOp::COND_BR);
}

TEST_P(MIPSControl, BranchIntoSlotDecodesAnIndependentExecutionPath) {
  // beq a0,zero,0x100c; addiu v0,zero,7
  // jr ra; addiu v0,v0,1
  // jr ra; nop
  const auto Image =
      image({0x10800002, 0x24020007, 0x03e00008, 0x24420001, 0x03e00008, 0},
            GetParam());
  const auto Func = build(Image);
  bool Independent = false;
  bool Owned = false;
  for (const auto &Block : Func.Blocks) {
    if (Block.StartAddr == 0x100c) {
      Independent = true;
      ASSERT_FALSE(Block.InstructionBoundaries.empty());
      EXPECT_EQ(Block.InstructionBoundaries.front().Address, 0x100cu);
      EXPECT_EQ(Block.InstructionBoundaries.front().Size, 4);
    }
    for (const auto &Boundary : Block.InstructionBoundaries)
      Owned |= Boundary.Address == 0x1008 && Boundary.DelaySlot.has_value();
  }
  EXPECT_TRUE(Independent);
  EXPECT_TRUE(Owned);
}

TEST_P(MIPSControl, TailCallRetainsSlotArgumentSetup) {
  // j 0x2000; addiu a1,zero,7
  auto Image = image({0x08000800, 0x24050007}, GetParam());
  Segment Callee;
  Callee.VA = 0x2000;
  Callee.Data = encode({0x03e00008, 0}, GetParam());
  Callee.Size = Callee.FileSz = Callee.Data.size();
  Callee.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Callee));
  Image.Symbols.push_back(Symbol::makeFunc(0x2000));
  const auto Func = build(Image);
  ASSERT_FALSE(Func.Blocks.empty());
  const auto &Block = Func.Blocks.front();
  ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
  EXPECT_EQ(Block.InstructionBoundaries.front().Control,
            LowInstructionControl::TailCall);
  ASSERT_TRUE(Block.InstructionBoundaries.front().DelaySlot);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  for (const auto &Op : Block.Ops) {
    if (Op.Opcode == NdOp::CALL) {
      EXPECT_EQ(Emulator.getRegister(mipsreg::r(5)), 7u);
      break;
    }
    ASSERT_TRUE(Emulator.step(Op));
  }
}

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSControl,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace
