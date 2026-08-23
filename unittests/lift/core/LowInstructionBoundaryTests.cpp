//===- LowInstructionBoundaryTests.cpp - LowIR instruction provenance ----===//

#include "../../../lib/ir/low/jumptable/JumpTableResolverDetail.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/ARMRegs.h"
#include "neverd/lift/LiftCommon.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kEntry = 0x1000;

TEST(LowInstructionBoundary, JumpTableCaseLabelsUseModularArithmetic) {
  EXPECT_EQ(recoverCaseLabelBitPattern(/*EntryIndex=*/0,
                                       /*Stride=*/1, /*NormShift=*/0,
                                       std::numeric_limits<int64_t>::min()),
            UINT64_C(0x8000000000000000));
  EXPECT_EQ(recoverCaseLabelBitPattern(UINT64_MAX, /*Stride=*/2,
                                       /*NormShift=*/0, /*NormBase=*/0),
            UINT64_C(0xfffffffffffffffe));
  EXPECT_EQ(recoverCaseLabelBitPattern(/*EntryIndex=*/1,
                                       /*Stride=*/1, /*NormShift=*/63,
                                       std::numeric_limits<int64_t>::min()),
            UINT64_C(0));
  EXPECT_FALSE(recoverCaseLabelBitPattern(/*EntryIndex=*/1,
                                          /*Stride=*/1, /*NormShift=*/64,
                                          /*NormBase=*/0));
}

TEST(LowInstructionBoundary, GuardEvaluatorDistinguishesNotFromTwosComplement) {
  const std::array<uint64_t, 1> Zero{0};
  const std::array<uint16_t, 1> W32{4};
  EXPECT_EQ(evaluateJumpTableGuardPrimitive(NdOp::INT_NEGATE, 4, Zero, W32),
            UINT64_C(0xffffffff));
  EXPECT_EQ(evaluateJumpTableGuardPrimitive(NdOp::INT_NEG2, 4, Zero, W32),
            UINT64_C(0));
}

TEST(LowInstructionBoundary, GuardEvaluatorCoercesMixedWidthComparisons) {
  const std::array<uint64_t, 2> Values{UINT64_C(0xffffffff),
                                       UINT64_C(0x100000004)};
  const std::array<uint16_t, 2> Widths{4, 8};
  EXPECT_EQ(evaluateJumpTableGuardPrimitive(NdOp::INT_LESS, 1, Values, Widths),
            UINT64_C(1));
}

TEST(LowInstructionBoundary, GuardEvaluatorRejectsValuesWiderThanU64) {
  const std::array<uint64_t, 1> Value{0};
  const std::array<uint16_t, 1> W128{16};
  EXPECT_FALSE(
      evaluateJumpTableGuardPrimitive(NdOp::INT_NEGATE, 16, Value, W128));
}

TEST(LowInstructionBoundary,
     RelativeTableTargetsRequireCompletePointerWidthTransforms) {
  EXPECT_TRUE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ZEXT, /*DynamicInputSize=*/4, /*OtherInputSize=*/0,
      /*OutputSize=*/8, /*PointerSize=*/8));
  EXPECT_FALSE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ZEXT, /*DynamicInputSize=*/1, /*OtherInputSize=*/0,
      /*OutputSize=*/4, /*PointerSize=*/8));

  EXPECT_TRUE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_LEFT, /*DynamicInputSize=*/8, /*OtherInputSize=*/1,
      /*OutputSize=*/8, /*PointerSize=*/8));
  EXPECT_FALSE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_LEFT, /*DynamicInputSize=*/4, /*OtherInputSize=*/1,
      /*OutputSize=*/4, /*PointerSize=*/8));

  EXPECT_TRUE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ADD, /*DynamicInputSize=*/8, /*OtherInputSize=*/8,
      /*OutputSize=*/8, /*PointerSize=*/8));
  EXPECT_FALSE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ADD, /*DynamicInputSize=*/4, /*OtherInputSize=*/4,
      /*OutputSize=*/4, /*PointerSize=*/8));
  EXPECT_FALSE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ADD, /*DynamicInputSize=*/8, /*OtherInputSize=*/4,
      /*OutputSize=*/8, /*PointerSize=*/8));
}

TEST(LowInstructionBoundary, AbsoluteARMTableTargetsUseUniformImageMode) {
  BinaryImage Thumb;
  Thumb.Arch = Arch::ARM;
  Thumb.Mode = InstructionMode::Thumb;
  EXPECT_EQ(canonicalizeAbsoluteTableCodeTarget(Thumb, 0x1001), 0x1000u);
  EXPECT_FALSE(canonicalizeAbsoluteTableCodeTarget(Thumb, 0x1000));

  BinaryImage ARM;
  ARM.Arch = Arch::ARM;
  ARM.Mode = InstructionMode::ARM;
  EXPECT_EQ(canonicalizeAbsoluteTableCodeTarget(ARM, 0x2000), 0x2000u);
  EXPECT_FALSE(canonicalizeAbsoluteTableCodeTarget(ARM, 0x2001));

  BinaryImage A64;
  A64.Arch = Arch::AArch64;
  A64.Mode = InstructionMode::Default;
  EXPECT_EQ(canonicalizeAbsoluteTableCodeTarget(A64, 0x3001), 0x3001u);
}

TEST(LowInstructionBoundary, JumpTableTargetBasePresenceAllowsZeroAnchor) {
  const uint8_t Entry = 3;
  EXPECT_EQ(decodeTableEntry(&Entry, 1, /*IsRelative=*/true,
                             /*IsSigned=*/false, /*BaseAddr=*/0x1000,
                             /*HasTargetBase=*/true, /*TargetBase=*/0,
                             /*Scale=*/4, /*AddressBytes=*/8),
            12u);
  EXPECT_EQ(decodeTableEntry(&Entry, 1, /*IsRelative=*/true,
                             /*IsSigned=*/false, /*BaseAddr=*/0x1000,
                             /*HasTargetBase=*/false, /*TargetBase=*/0,
                             /*Scale=*/4, /*AddressBytes=*/8),
            0x1003u);
}

TEST(LowInstructionBoundary,
     RelocatedOwnerUsesMachOSectionCodeAuthorityForOnePast) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Mode = InstructionMode::Default;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = 0x1000;
  Text.Size = 0x200;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));

  Section Code;
  Code.Name = "__text";
  Code.SegmentName = "__TEXT";
  Code.VA = 0x1000;
  Code.Size = 0x40;
  Code.FileSz = Code.Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Type = llvm::MachO::S_REGULAR | llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
              llvm::MachO::S_ATTR_SOME_INSTRUCTIONS;
  Image.Sections.push_back(Code);

  Section CString;
  CString.Name = "__cstring";
  CString.SegmentName = "__TEXT";
  CString.VA = 0x1100;
  CString.Size = 0x10;
  CString.FileSz = CString.Size;
  CString.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  CString.Type = llvm::MachO::S_CSTRING_LITERALS;
  Image.Sections.push_back(CString);

  ASSERT_TRUE(Image.hasExecutableCodeOwnerAt(Code.VA));
  ASSERT_FALSE(Image.hasExecutableCodeOwnerAt(CString.VA));
  EXPECT_FALSE(
      Image.relocatedTargetBelongsToOwner(Code.VA + Code.Size, Code.VA));
  EXPECT_TRUE(Image.relocatedTargetBelongsToOwner(CString.VA + CString.Size,
                                                  CString.VA));
}

TEST(LowInstructionBoundary, JumpTableTargetsUseGuestWidthModularArithmetic) {
  const uint32_t PositiveOffset = 0x30;
  EXPECT_EQ(decodeTableEntry(reinterpret_cast<const uint8_t *>(&PositiveOffset),
                             4,
                             /*IsRelative=*/true, /*IsSigned=*/true,
                             /*BaseAddr=*/0xfffffffffffffff0ULL,
                             /*HasTargetBase=*/false, /*TargetBase=*/0,
                             /*Scale=*/1, /*AddressBytes=*/8),
            0x20u);

  const uint8_t WrappedOffset = 0x10;
  EXPECT_EQ(decodeTableEntry(&WrappedOffset, 1, /*IsRelative=*/true,
                             /*IsSigned=*/false, /*BaseAddr=*/0xfffffff8u,
                             /*HasTargetBase=*/false, /*TargetBase=*/0,
                             /*Scale=*/1, /*AddressBytes=*/4),
            8u);

  const uint8_t CompactEntry = 2;
  EXPECT_EQ(decodeTableEntry(&CompactEntry, 1, /*IsRelative=*/true,
                             /*IsSigned=*/false, /*BaseAddr=*/0x200,
                             /*HasTargetBase=*/true,
                             /*TargetBase=*/0xfffffffcu,
                             /*Scale=*/4, /*AddressBytes=*/4),
            4u);
}

TEST(LowInstructionBoundary, JumpTableKeepsPhysicalSlotsSeparateFromLabels) {
  JumpTable Table;
  Table.BaseAddr = 0x2000;
  Table.HasBaseAddr = true;
  Table.EntrySize = 8;
  Table.EntryStride = 16;
  Table.StorageRanges = {JumpTableStorageRange{0x2000, 8, 16, 3},
                         JumpTableStorageRange{0x3000, 4, 4, 2}};
  Table.Targets = {0x3000, 0x3020};
  Table.SlotIndices = {0, 2};
  Table.HasDispatchSlotMap = true;
  Table.CaseLabels = {-7, 41};

  EXPECT_EQ(Table.targetPositionForPhysicalSlot(0), 0u);
  EXPECT_FALSE(Table.targetPositionForPhysicalSlot(1).has_value());
  EXPECT_EQ(Table.targetPositionForPhysicalSlot(2), 1u);
  EXPECT_TRUE(Table.ownsStorageAddress(0x2000));
  EXPECT_FALSE(
      Table.ownsStorageAddress(0x2008)); // strided padding is not owned
  EXPECT_TRUE(Table.ownsStorageAddress(0x2010));
  EXPECT_FALSE(Table.ownsStorageAddress(0x2018));
  EXPECT_TRUE(Table.ownsStorageAddress(0x2027)); // end byte of trailing slot
  EXPECT_FALSE(Table.ownsStorageAddress(0x2028));
  EXPECT_FALSE(Table.ownsStorageAddress(0x2800)); // disjoint gap remains free
  EXPECT_TRUE(Table.ownsStorageAddress(0x3007));
  EXPECT_FALSE(Table.ownsStorageAddress(0x3008));
  EXPECT_FALSE(Table.storageEnd().has_value());
}

TEST(LowInstructionBoundary, JumpTableRejectsUnsupportedEntryWidths) {
  const uint8_t Bytes[16] = {};
  EXPECT_FALSE(decodeTableEntry(Bytes, 3, /*IsRelative=*/true,
                                /*IsSigned=*/false, /*BaseAddr=*/0,
                                /*HasTargetBase=*/true,
                                /*TargetBase=*/0x1000, /*Scale=*/4,
                                /*AddressBytes=*/8));
  EXPECT_FALSE(decodeTableEntry(Bytes, 16, /*IsRelative=*/false,
                                /*IsSigned=*/false, /*BaseAddr=*/0));
}

TEST(LowInstructionBoundary, FrameSlotDeltasFollowArithmeticWidthCoercion) {
  auto resolveOffset = [&](Arch Architecture, NdOp Opcode, NdVar Constant,
                           uint16_t OutputSize) -> std::optional<int64_t> {
    const TargetRegInfo &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(TRI.PointerSize, OutputSize);
    LowOp Arithmetic;
    Arithmetic.Opcode = Opcode;
    Arithmetic.Output = NdVar::tmp(0x100, OutputSize);
    Arithmetic.addInput(NdVar::reg(TRI.StackPointer, OutputSize));
    Arithmetic.addInput(Constant);
    std::vector<LowOp> Ops{Arithmetic};
    uint64_t BaseReg = InvalidVA;
    int64_t Offset = 0;
    if (!frameSlotKey(Ops, 0, Arithmetic.Output, TRI, BaseReg, Offset))
      return std::nullopt;
    EXPECT_EQ(BaseReg, TRI.StackPointer);
    return Offset;
  };

  // Integer operands are zero-extended to the operation width.  Treating the
  // high bit of the i8 literal as a sign bit would analyze a different stack
  // address than the emitted 32-bit ADD/SUB executes.
  EXPECT_EQ(resolveOffset(Arch::X86, NdOp::INT_ADD, NdVar::scalar(0xf0, 1), 4),
            240);
  EXPECT_EQ(resolveOffset(Arch::X86, NdOp::INT_SUB, NdVar::scalar(0xf0, 1), 4),
            -240);

  // A genuine pointer-width two's-complement displacement remains negative.
  EXPECT_EQ(resolveOffset(Arch::X86, NdOp::INT_ADD,
                          NdVar::scalar(uint32_t{0xfffffff0}, 4), 4),
            -16);
  EXPECT_EQ(resolveOffset(Arch::X86, NdOp::INT_SUB,
                          NdVar::scalar(uint32_t{0xfffffff0}, 4), 4),
            16);

  // Exercise the full-width mask/sign path as well; it must make the same
  // distinction without shifting by 64 or relying on narrower truncation.
  EXPECT_EQ(resolveOffset(Arch::X64, NdOp::INT_ADD, NdVar::scalar(0xf0, 1), 8),
            240);
  EXPECT_EQ(resolveOffset(Arch::X64, NdOp::INT_SUB, NdVar::scalar(0xf0, 1), 8),
            -240);
  EXPECT_EQ(resolveOffset(Arch::X64, NdOp::INT_ADD,
                          NdVar::scalar(uint64_t{0xfffffffffffffff0}, 8), 8),
            -16);
  EXPECT_EQ(resolveOffset(Arch::X64, NdOp::INT_SUB,
                          NdVar::scalar(uint64_t{0xfffffffffffffff0}, 8), 8),
            16);
}

TEST(LowInstructionBoundary, IntegerAndMaskUsesOutputAndDynamicWidths) {
  // A wider encoded immediate is truncated to the operation result.  Using
  // the immediate's own width would invent a 512-value domain for an i8 AND.
  EXPECT_EQ(effectiveIntegerAndMask(/*EncodedMask=*/0x1ff,
                                    /*MaskSize=*/2,
                                    /*DynamicSize=*/1,
                                    /*OutputSize=*/1),
            0xffu);

  // A narrow dynamic input is zero-extended before a wider AND, so its newly
  // introduced high output bits remain unreachable even when the mask sets
  // them.
  EXPECT_EQ(effectiveIntegerAndMask(/*EncodedMask=*/0x1ff,
                                    /*MaskSize=*/2,
                                    /*DynamicSize=*/1,
                                    /*OutputSize=*/2),
            0xffu);

  // With a genuinely wide dynamic operand the ninth bit is reachable.
  EXPECT_EQ(effectiveIntegerAndMask(/*EncodedMask=*/0x1ff,
                                    /*MaskSize=*/2,
                                    /*DynamicSize=*/2,
                                    /*OutputSize=*/2),
            0x1ffu);
  EXPECT_FALSE(effectiveIntegerAndMask(/*EncodedMask=*/~uint64_t{0},
                                       /*MaskSize=*/16,
                                       /*DynamicSize=*/8,
                                       /*OutputSize=*/8));
}

LowFunc buildFunction(Arch Architecture, InstructionMode Mode,
                      std::vector<uint8_t> Bytes,
                      std::vector<Symbol> Symbols = {}) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Mode = Mode;
  Image.Bits = Architecture == Arch::X64 || Architecture == Arch::AArch64
                   ? Bitness::Bits64
                   : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Base = kEntry;
  Image.Symbols = std::move(Symbols);

  Segment Text;
  Text.VA = kEntry;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(Text));

  Decoder Dec;
  if (!Dec.init(Architecture, Mode)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  return Builder.build(Image, Dec, kEntry, "instruction_boundaries");
}

const LowBlock *findBlock(const LowFunc &Function, va_t Address) {
  for (const LowBlock &Block : Function.Blocks)
    if (Block.StartAddr == Address)
      return &Block;
  return nullptr;
}

std::string errorText(llvm::Error Error) {
  return llvm::toString(std::move(Error));
}

std::vector<LowOp> liftARMInstruction(InstructionMode Mode,
                                      const std::vector<uint8_t> &Bytes,
                                      va_t Address = kEntry) {
  Decoder Dec;
  if (!Dec.init(Arch::ARM, Mode)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  int Size = Dec.decodeOneForLift(Bytes.data(), Bytes.size(), Address, Insn);
  if (Size <= 0) {
    ADD_FAILURE() << "instruction decode failed";
    return {};
  }
  EXPECT_EQ(static_cast<size_t>(Size), Bytes.size());

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

std::vector<LowOp> liftAArch64Instruction(const std::vector<uint8_t> &Bytes,
                                          va_t Address = kEntry) {
  Decoder Dec;
  if (!Dec.init(Arch::AArch64, InstructionMode::Default)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  int Size = Dec.decodeOneForLift(Bytes.data(), Bytes.size(), Address, Insn);
  if (Size <= 0) {
    ADD_FAILURE() << "instruction decode failed";
    return {};
  }
  EXPECT_EQ(static_cast<size_t>(Size), Bytes.size());

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

std::vector<LowOp> liftX86Instruction(Arch Architecture,
                                      const std::vector<uint8_t> &Bytes,
                                      va_t Address = kEntry) {
  Decoder Dec;
  if (!Dec.init(Architecture, InstructionMode::Default)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  int Size = Dec.decodeOneForLift(Bytes.data(), Bytes.size(), Address, Insn);
  if (Size <= 0) {
    ADD_FAILURE() << "instruction decode failed";
    return {};
  }
  EXPECT_EQ(static_cast<size_t>(Size), Bytes.size());

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

LowFunc buildAArch64PageBaseUse(std::vector<uint8_t> Bytes) {
  constexpr va_t DataPage = 0x9000;

  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Mode = InstructionMode::Default;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Image.Base = kEntry;
  Image.Entry = kEntry;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = kEntry;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = "__DATA";
  Data.VA = DataPage;
  Data.Size = 0x100;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));

  // Deliberately give the page start exact loader ownership in both tests.
  // The negative case therefore cannot pass by merely noticing that the ADRP
  // result numerically equals a known pointer slot.
  Image.CodePtrRelocSlots.insert(DataPage);

  Decoder Dec;
  if (!Dec.init(Arch::AArch64, InstructionMode::Default)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  return Builder.build(Image, Dec, kEntry, "aarch64_page_base_use");
}

const LowOp *findAddressMaterialization(const LowFunc &Function,
                                        va_t InstructionAddress) {
  for (const LowBlock &Block : Function.Blocks)
    for (const LowOp &Op : Block.Ops)
      if (Op.Addr == InstructionAddress && Op.Opcode == NdOp::COPY &&
          Op.NumInputs == 1 && Op.Output.isReg() && Op.Inputs[0].isConst())
        return &Op;
  return nullptr;
}

bool containsOp(const std::vector<LowOp> &Ops, NdOp Opcode) {
  return std::any_of(Ops.begin(), Ops.end(),
                     [Opcode](const LowOp &Op) { return Op.Opcode == Opcode; });
}

TEST(LowInstructionBoundary, PreservesInstructionSlicesAcrossBlockSplits) {
  // xor eax, eax; je target; nop; target: ret
  LowFunc Function = buildFunction(Arch::X64, InstructionMode::Default,
                                   {0x31, 0xc0, 0x74, 0x01, 0x90, 0xc3});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 2u);

  const LowInstructionBoundary &Xor = Entry->InstructionBoundaries[0];
  EXPECT_EQ(Xor.Address, kEntry);
  EXPECT_EQ(Xor.Size, 2u);
  EXPECT_EQ(Xor.FirstOp, 0u);
  EXPECT_GT(Xor.OpCount, 0u);
  EXPECT_EQ(Xor.Mode, InstructionMode::Default);

  const LowInstructionBoundary &Branch = Entry->InstructionBoundaries[1];
  EXPECT_EQ(Branch.Address, kEntry + 2);
  EXPECT_EQ(Branch.Size, 2u);
  EXPECT_EQ(Branch.FirstOp, Xor.FirstOp + Xor.OpCount);
  EXPECT_EQ(Branch.FirstOp + Branch.OpCount, Entry->Ops.size());
  EXPECT_EQ(Branch.Control, LowInstructionControl::Branch);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Branch.ControlFlags, LowInstructionControlFlag::Conditional));
  ASSERT_TRUE(Branch.Immediate.has_value());
  EXPECT_EQ(*Branch.Immediate, kEntry + 5);

  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary, PreservesDirectCallAndEncodedReturnImmediates) {
  // call 0x1008; ret 0.  The return's explicit zero must not collapse into the
  // absence used by the one-byte `ret` encoding.
  LowFunc Function =
      buildFunction(Arch::X64, InstructionMode::Default,
                    {0xe8, 0x03, 0x00, 0x00, 0x00, 0xc2, 0x00, 0x00});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 2u);

  const LowInstructionBoundary &Call = Entry->InstructionBoundaries[0];
  EXPECT_EQ(Call.Control, LowInstructionControl::Call);
  EXPECT_TRUE(hasLowInstructionControlFlag(Call.ControlFlags,
                                           LowInstructionControlFlag::Call));
  ASSERT_TRUE(Call.Immediate.has_value());
  EXPECT_EQ(*Call.Immediate, kEntry + 8);

  const LowInstructionBoundary &Return = Entry->InstructionBoundaries[1];
  EXPECT_EQ(Return.Control, LowInstructionControl::Return);
  EXPECT_TRUE(hasLowInstructionControlFlag(Return.ControlFlags,
                                           LowInstructionControlFlag::Return));
  ASSERT_TRUE(Return.Immediate.has_value());
  EXPECT_EQ(*Return.Immediate, 0u);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));

  LowFunc Ordinary = buildFunction(Arch::X64, InstructionMode::Default, {0xc3});
  const LowBlock *OrdinaryEntry = findBlock(Ordinary, kEntry);
  ASSERT_NE(OrdinaryEntry, nullptr);
  ASSERT_EQ(OrdinaryEntry->InstructionBoundaries.size(), 1u);
  EXPECT_FALSE(OrdinaryEntry->InstructionBoundaries[0].Immediate.has_value());
}

TEST(LowInstructionBoundary, RetainsTheDecoderModeAcrossSupportedISAs) {
  struct Case {
    Arch Architecture;
    InstructionMode Mode;
    InstructionMode ExpectedMode;
    std::vector<uint8_t> Bytes;
    std::vector<uint16_t> Sizes;
  };
  const std::vector<Case> Cases = {
      {Arch::X64,
       InstructionMode::Default,
       InstructionMode::Default,
       {0x90, 0xc3},
       {1, 1}},
      {Arch::ARM,
       InstructionMode::ARM,
       InstructionMode::ARM,
       {0x00, 0x00, 0xa0, 0xe1, 0x1e, 0xff, 0x2f, 0xe1},
       {4, 4}},
      {Arch::ARM,
       InstructionMode::Default,
       InstructionMode::ARM,
       {0x00, 0x00, 0xa0, 0xe1, 0x1e, 0xff, 0x2f, 0xe1},
       {4, 4}},
      {Arch::ARM,
       InstructionMode::Thumb,
       InstructionMode::Thumb,
       {0x00, 0xbf, 0x70, 0x47},
       {2, 2}},
      {Arch::AArch64,
       InstructionMode::Default,
       InstructionMode::Default,
       {0x1f, 0x20, 0x03, 0xd5, 0xc0, 0x03, 0x5f, 0xd6},
       {4, 4}},
  };

  for (const Case &Test : Cases) {
    LowFunc Function = buildFunction(Test.Architecture, Test.Mode, Test.Bytes);
    const LowBlock *Entry = findBlock(Function, kEntry);
    ASSERT_NE(Entry, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), Test.Sizes.size());
    for (size_t I = 0; I < Test.Sizes.size(); ++I) {
      EXPECT_EQ(Entry->InstructionBoundaries[I].Mode, Test.ExpectedMode);
      EXPECT_EQ(Entry->InstructionBoundaries[I].Size, Test.Sizes[I]);
    }
    EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
        Function, LowInstructionBoundaryRequirement::Required)));
  }
}

TEST(LowInstructionBoundary, RejectsMixedInstructionModesWithinABlock) {
  LowFunc Function = buildFunction(Arch::X64, InstructionMode::Default,
                                   {0x90, 0xc3}); // nop; ret
  LowBlock *Entry = Function.blockFor(kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 2u);

  Entry->InstructionBoundaries.back().Mode = InstructionMode::Thumb;
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Function, LowInstructionBoundaryRequirement::Required))
          .find("mix instruction modes"),
      std::string::npos);
}

TEST(LowInstructionBoundary, PreservesARMInterworkingTargetMode) {
  struct Case {
    InstructionMode Mode;
    std::vector<uint8_t> Bytes;
    LowInstructionTargetMode TargetMode;
  };
  const std::vector<Case> Cases = {
      {InstructionMode::Default,
       {0x00, 0x00, 0x00, 0xea},
       LowInstructionTargetMode::Preserve},
      {InstructionMode::ARM,
       {0x00, 0x00, 0x00, 0xeb},
       LowInstructionTargetMode::Preserve},
      {InstructionMode::ARM,
       {0x00, 0x00, 0x00, 0xfa},
       LowInstructionTargetMode::Thumb},
      {InstructionMode::ARM,
       {0x10, 0xff, 0x2f, 0xe1},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x1e, 0xff, 0x2f, 0xe1},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x30, 0xff, 0x2f, 0xe1},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x00, 0xe0},
       LowInstructionTargetMode::Preserve},
      {InstructionMode::Thumb,
       {0x00, 0xf0, 0x00, 0xf8},
       LowInstructionTargetMode::Preserve},
      {InstructionMode::Thumb,
       {0x00, 0xf0, 0x00, 0xe8},
       LowInstructionTargetMode::ARM},
      {InstructionMode::Thumb,
       {0x00, 0x47},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x70, 0x47},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x80, 0x47},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x00, 0xf0, 0x90, 0xe5},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0xd0, 0xf8, 0x00, 0xf0},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x04, 0xf0, 0x9d, 0xe4},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x00, 0xbd},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x10, 0x80, 0xb0, 0xe8},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x00, 0xf0, 0xa0, 0xe1},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x87, 0x46},
       LowInstructionTargetMode::Preserve},
  };

  for (const Case &Test : Cases) {
    LowFunc Function = buildFunction(Arch::ARM, Test.Mode, Test.Bytes);
    const LowBlock *Entry = findBlock(Function, kEntry);
    ASSERT_NE(Entry, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
    const LowInstructionBoundary &Boundary =
        Entry->InstructionBoundaries.front();
    EXPECT_EQ(Boundary.Mode, Test.Mode == InstructionMode::Default
                                 ? InstructionMode::ARM
                                 : Test.Mode);
    EXPECT_EQ(Boundary.TargetMode, Test.TargetMode);
    EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
        Function, LowInstructionBoundaryRequirement::Required)));
  }
}

TEST(LowInstructionBoundary, ARMPCWritesEmitExplicitControlOps) {
  struct Case {
    InstructionMode Mode;
    std::vector<uint8_t> Bytes;
    NdOp Terminator;
  };
  const std::vector<Case> Cases = {
      {InstructionMode::ARM, {0x00, 0xf0, 0x90, 0xe5}, NdOp::INDIR_BR},
      {InstructionMode::Thumb, {0xd0, 0xf8, 0x00, 0xf0}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x10, 0x80, 0xb0, 0xe8}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x00, 0xf0, 0xa0, 0xe1}, NdOp::INDIR_BR},
      {InstructionMode::Thumb, {0x87, 0x46}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x00, 0xf0, 0xe0, 0xe1}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x01, 0xf0, 0x80, 0xe0}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x0e, 0xf0, 0xa0, 0xe1}, NdOp::RETURN},
      {InstructionMode::ARM, {0x04, 0xf0, 0x9d, 0xe4}, NdOp::RETURN},
      {InstructionMode::Thumb, {0x00, 0xbd}, NdOp::RETURN},
      {InstructionMode::ARM, {0x00, 0x80, 0xbd, 0xe8}, NdOp::RETURN},
  };

  for (const Case &Test : Cases) {
    const std::vector<LowOp> Ops = liftARMInstruction(Test.Mode, Test.Bytes);
    EXPECT_TRUE(containsOp(Ops, Test.Terminator));
    EXPECT_FALSE(containsOp(
        Ops, Test.Terminator == NdOp::RETURN ? NdOp::INDIR_BR : NdOp::RETURN));
  }
}

TEST(LowInstructionBoundary, ARMPCReadUsesModeSpecificRelocatableAddress) {
  struct Case {
    InstructionMode Mode;
    va_t Address;
    std::vector<uint8_t> Bytes;
    uint64_t ExpectedPC;
  };
  const std::vector<Case> Cases = {
      {InstructionMode::Thumb, 0x1000, {0x00, 0x48}, 0x1004},
      {InstructionMode::Thumb, 0x1002, {0x00, 0x48}, 0x1004},
      {InstructionMode::ARM, 0x1000, {0x00, 0x00, 0x9f, 0xe5}, 0x1008},
  };

  for (const Case &Test : Cases) {
    SCOPED_TRACE(::testing::Message()
                 << (Test.Mode == InstructionMode::Thumb ? "Thumb" : "ARM")
                 << " at 0x" << std::hex << Test.Address);
    const std::vector<LowOp> Ops =
        liftARMInstruction(Test.Mode, Test.Bytes, Test.Address);
    auto PCWrite = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::COPY &&
             Op.Output == NdVar::reg(armreg::PC, 4) && Op.NumInputs == 1 &&
             Op.Inputs[0].isConst();
    });
    ASSERT_NE(PCWrite, Ops.end());
    EXPECT_EQ(PCWrite->Inputs[0].Offset, Test.ExpectedPC);
    EXPECT_EQ(PCWrite->Inputs[0].Provenance,
              ConstantAddressProvenance::Address);
  }
}

TEST(LowInstructionBoundary, ARMTableBranchUsesRawPCAndScalarScale) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    va_t Address;
  };
  const std::vector<Case> Cases = {
      {"tbb", {0xdf, 0xe8, 0x00, 0xf0}, 0x1002},
      {"tbh", {0xdf, 0xe8, 0x10, 0xf0}, 0x1002},
  };

  for (const Case &Test : Cases) {
    SCOPED_TRACE(Test.Name);
    const std::vector<LowOp> Ops =
        liftARMInstruction(InstructionMode::Thumb, Test.Bytes, Test.Address);

    auto PCWrite = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::COPY &&
             Op.Output == NdVar::reg(armreg::PC, 4) && Op.NumInputs == 1;
    });
    PCWrite = std::find_if(PCWrite, Ops.end(), [&](const LowOp &Op) {
      return Op.Opcode == NdOp::COPY &&
             Op.Output == NdVar::reg(armreg::PC, 4) && Op.NumInputs == 1 &&
             Op.Inputs[0].isConst() && Op.Inputs[0].Offset == Test.Address + 4;
    });
    ASSERT_NE(PCWrite, Ops.end());
    ASSERT_TRUE(PCWrite->Inputs[0].isConst());
    EXPECT_EQ(PCWrite->Inputs[0].Offset, 0x1006u);
    EXPECT_EQ(PCWrite->Inputs[0].Provenance,
              ConstantAddressProvenance::Address);

    auto Scale = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::INT_LEFT && Op.NumInputs == 2 &&
             Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 1 &&
             Op.Inputs[1].Size == 4;
    });
    ASSERT_NE(Scale, Ops.end());
    EXPECT_EQ(Scale->Inputs[1].Provenance, ConstantAddressProvenance::Scalar);
  }
}

TEST(LowInstructionBoundary, X86EIPRelativeAddressWrapsBeforeZeroExtension) {
  constexpr va_t HighAddress = 0x100001000ULL;
  struct Case {
    std::vector<uint8_t> Bytes;
    va_t ExpectedTarget;
    uint64_t ExpectedTaggedLeaf;
    ConstantAddressProvenance ExpectedProvenance;
  };
  const std::vector<Case> Cases = {
      {{0x67, 0x48, 0x8d, 0x05, 0xf9, 0xff, 0xff, 0xff},
       0x1001,
       0x1001,
       ConstantAddressProvenance::Address},
      {{0x48, 0x8d, 0x05, 0xf9, 0xff, 0xff, 0xff},
       HighAddress,
       HighAddress + 7,
       ConstantAddressProvenance::Address},
  };

  for (const Case &Test : Cases) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64, InstructionMode::Default));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Test.Bytes.data(), Test.Bytes.size(),
                                   HighAddress, Insn),
              static_cast<int>(Test.Bytes.size()));
    EXPECT_EQ(Dec.pcRelCodeRefTarget(Insn), Test.ExpectedTarget);

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    bool SawTaggedLeaf = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        SawTaggedLeaf |= Op.Inputs[I].isConst() &&
                         Op.Inputs[I].Provenance == Test.ExpectedProvenance &&
                         Op.Inputs[I].Offset == Test.ExpectedTaggedLeaf;
    EXPECT_TRUE(SawTaggedLeaf);
  }
}

TEST(LowInstructionBoundary, X86I386EffectiveAddressWrapsAtGuestPointerWidth) {
  // mov ecx, dword ptr [eax + 1]
  const std::vector<LowOp> Ops =
      liftX86Instruction(Arch::X86, {0x8b, 0x48, 0x01});

  auto BaseCopy = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.Output.Size == 4 &&
           Op.NumInputs == 1 && Op.Inputs[0] == NdVar::reg(x86reg::RAX, 4);
  });
  ASSERT_NE(BaseCopy, Ops.end());

  auto Add = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ADD && Op.Output.Size == 4 &&
           Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
           Op.Inputs[1].Offset == 1 && Op.Inputs[1].Size == 4;
  });
  ASSERT_NE(Add, Ops.end());

  auto Extend = std::find_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ZEXT && Op.Output.Size == 8 &&
           Op.NumInputs == 1 && Op.Inputs[0] == Add->Output;
  });
  ASSERT_NE(Extend, Ops.end());
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == NdOp::LOAD && Op.NumInputs != 0 &&
           Op.Inputs[0] == Extend->Output;
  }));

  // In particular, EAX=0xffffffff plus one is computed by the i32 ADD and
  // wraps to zero before the internal 8-byte VA representation is formed.
  EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    for (uint8_t I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I] == NdVar::reg(x86reg::RAX, 8))
        return true;
    return false;
  }));
}

TEST(LowInstructionBoundary, X86AddressOverrideIgnoresTheHighHalfOfRAX) {
  // addr32 mov ecx, dword ptr [eax + 1]
  const std::vector<LowOp> Ops =
      liftX86Instruction(Arch::X64, {0x67, 0x8b, 0x48, 0x01});

  auto Add = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ADD && Op.Output.Size == 4 &&
           Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
           Op.Inputs[1].Offset == 1 && Op.Inputs[1].Size == 4;
  });
  ASSERT_NE(Add, Ops.end());
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.Output.Size == 4 &&
           Op.NumInputs == 1 && Op.Inputs[0] == NdVar::reg(x86reg::RAX, 4);
  }));
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ZEXT && Op.Output.Size == 8 &&
           Op.NumInputs == 1 && Op.Inputs[0] == Add->Output;
  }));
  EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    for (uint8_t I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I] == NdVar::reg(x86reg::RAX, 8))
        return true;
    return false;
  }));
}

TEST(LowInstructionBoundary, X86Native64EffectiveAddressKeepsRAXWidth) {
  // mov ecx, dword ptr [rax + 1]
  const std::vector<LowOp> Ops =
      liftX86Instruction(Arch::X64, {0x8b, 0x48, 0x01});

  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.Output.Size == 8 &&
           Op.NumInputs == 1 && Op.Inputs[0] == NdVar::reg(x86reg::RAX, 8);
  }));
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ADD && Op.Output.Size == 8 &&
           Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
           Op.Inputs[1].Offset == 1 && Op.Inputs[1].Size == 8;
  }));
}

TEST(LowInstructionBoundary,
     X86RelocatedDisplacementDoesNotTagEqualStoredImmediate) {
  constexpr va_t DataVA = 0x250;
  // movl $0x250, 0x250(%ebx); ret
  //
  // The disp32 and imm32 deliberately have the same bits.  Only the disp32
  // relocation occurrence forms the destination address; the stored value is
  // an ordinary integer and must not inherit DataAddress provenance merely
  // because it is numerically equal to the relocation target.
  const std::vector<uint8_t> Bytes = {0xc7, 0x83, 0x50, 0x02, 0x00, 0x00,
                                      0x50, 0x02, 0x00, 0x00, 0xc3};

  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Mode = InstructionMode::Default;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Base = 0;
  Image.Entry = kEntry;

  Segment Data;
  Data.VA = 0x200;
  Data.Size = 0x100;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));

  Segment Text;
  Text.VA = kEntry;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = Bytes;
  Image.Segments.push_back(std::move(Text));

  // c7 /0 uses a ModR/M byte at +1, disp32 at +2, and imm32 at +6.
  Image.DataAddressRelocOperands[kEntry + 2] = {DataVA, DataVA, 4, 0x200};

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  CFGBuilder Builder;
  LowFunc Function =
      Builder.build(Image, Dec, kEntry, "relocated_displacement_occurrence");

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  const LowOp *Store = nullptr;
  unsigned TaggedTargetOccurrences = 0;
  for (const LowOp &Op : Entry->Ops) {
    if (Op.Opcode == NdOp::STORE)
      Store = &Op;
    for (uint8_t I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I].isConst() &&
          (Op.Inputs[I].Offset & uint64_t{0xffffffff}) == DataVA &&
          Op.Inputs[I].Provenance == ConstantAddressProvenance::DataAddress)
        ++TaggedTargetOccurrences;
  }

  ASSERT_NE(Store, nullptr);
  ASSERT_GE(Store->NumInputs, 2u);
  EXPECT_TRUE(Store->Inputs[1].isConst());
  EXPECT_EQ(Store->Inputs[1].Offset, DataVA);
  EXPECT_EQ(Store->Inputs[1].Provenance, ConstantAddressProvenance::Unknown);
  EXPECT_EQ(TaggedTargetOccurrences, 1u);
}

TEST(LowInstructionBoundary,
     X86BaseLessIndexedDisplacementIsScalarWithoutExactRelocation) {
  // lea 0x2b(,%r8,4), %esi
  //
  // The displacement is one arithmetic component, not a complete address
  // occurrence.  In a low-VA object 0x2b may also be a lifted block start;
  // tagging this leaf Address would let numeric equality turn an index formula
  // into a BlockAddress.  A loader descriptor still overrides this default for
  // a genuine absolute table/global relocation.
  const std::vector<uint8_t> Bytes = {0x42, 0x8d, 0x34, 0x85,
                                      0x2b, 0x00, 0x00, 0x00};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
            static_cast<int>(Bytes.size()));

  auto displacementProvenance =
      [&](llvm::ArrayRef<RelocatedAddressOperand> Relocs) {
        std::vector<LowOp> Ops;
        Dec.liftToLow(Insn, Ops, Relocs);
        std::optional<ConstantAddressProvenance> Result;
        for (const LowOp &Op : Ops)
          for (uint8_t I = 0; I < Op.NumInputs; ++I)
            if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x2b &&
                Op.Inputs[I].Size == 8)
              Result = Op.Inputs[I].Provenance;
        return Result;
      };

  auto Plain = displacementProvenance({});
  ASSERT_TRUE(Plain.has_value());
  EXPECT_EQ(*Plain, ConstantAddressProvenance::Scalar);

  RelocatedAddressOperand Reloc;
  Reloc.FieldVA = kEntry + 4;
  Reloc.EncodedValue = 0x2b;
  Reloc.TargetVA = 0x2b;
  Reloc.Width = 4;
  Reloc.Provenance = ConstantAddressProvenance::DataAddress;
  auto Relocated = displacementProvenance(
      llvm::ArrayRef<RelocatedAddressOperand>(&Reloc, 1));
  ASSERT_TRUE(Relocated.has_value());
  EXPECT_EQ(*Relocated, ConstantAddressProvenance::DataAddress);
}

TEST(LowInstructionBoundary,
     X86RelocatedDisplacementPreservesZeroAndUnsignedHighBitTargets) {
  struct Case {
    uint32_t Encoded;
    std::vector<uint8_t> Bytes;
  };
  const std::vector<Case> Cases = {
      {0, {0x8b, 0x83, 0x00, 0x00, 0x00, 0x00}},
      {0x80000000u, {0x8b, 0x83, 0x00, 0x00, 0x00, 0x80}},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Encoded);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X86));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(C.Bytes.data(), C.Bytes.size(), kEntry, Insn),
        static_cast<int>(C.Bytes.size()));

    RelocatedAddressOperand Reloc;
    Reloc.FieldVA = kEntry + 2;
    Reloc.EncodedValue = C.Encoded;
    Reloc.TargetVA = C.Encoded;
    Reloc.Width = 4;
    Reloc.Provenance = ConstantAddressProvenance::DataAddress;
    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops,
                  llvm::ArrayRef<RelocatedAddressOperand>(&Reloc, 1));

    unsigned Matches = 0;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() &&
            Op.Inputs[I].Provenance == ConstantAddressProvenance::DataAddress) {
          EXPECT_EQ(Op.Inputs[I].Offset, uint64_t(C.Encoded));
          ++Matches;
        }
    EXPECT_EQ(Matches, 1u);
  }
}

TEST(LowInstructionBoundary, X86ConflictingRelocationOwnersFailClosed) {
  // mov 0x20(%rbx), %eax
  const std::vector<uint8_t> Bytes = {0x8b, 0x43, 0x20};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
            static_cast<int>(Bytes.size()));

  const RelocatedAddressOperand Relocs[] = {
      {kEntry + 2, 0x20, 0x20, 1, ConstantAddressProvenance::DataAddress},
      {kEntry + 2, 0x20, 0x20, 1, ConstantAddressProvenance::CodeAddress},
  };
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops, Relocs), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST(LowInstructionBoundary, X86GeneratedShiftMaskIsAlwaysScalar) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
  };
  const std::vector<Case> Cases = {
      {"shl", {0x48, 0xd3, 0xe0}},
      {"rol", {0x48, 0xd3, 0xc0}},
      {"shld", {0x48, 0x0f, 0xa5, 0xd8}},
      {"shrd", {0x48, 0x0f, 0xad, 0xd8}},
      {"rcr", {0x48, 0xd3, 0xd8}},
      {"rcl", {0x48, 0xd3, 0xd0}},
      {"rorx", {0xc4, 0xe3, 0xfb, 0xf0, 0xc3, 0x07}},
      {"shrx", {0xc4, 0xe2, 0xf3, 0xf7, 0xc3}},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(C.Bytes.data(), C.Bytes.size(), kEntry, Insn),
        static_cast<int>(C.Bytes.size()));

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    bool SawCountMask = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        const NdVar &Input = Op.Inputs[I];
        if (!Input.isConst())
          continue;
        EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::Scalar);
        if (Op.Opcode == NdOp::INT_AND && Input.Offset == 0x3f &&
            Input.Size == 8)
          SawCountMask = true;
      }
    EXPECT_TRUE(SawCountMask);
  }
}

TEST(LowInstructionBoundary, X86GeneratedArithmeticConstantsAreScalar) {
  // incq %rax synthesizes a pointer-width `1` for the add and its flags.  In a
  // low-VA object another function may genuinely live at VA 1; the generated
  // increment must not inherit that function's value-global CodeRef identity.
  const std::vector<uint8_t> Bytes = {0x48, 0xff, 0xc0};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
            static_cast<int>(Bytes.size()));

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  bool SawPointerWidthOne = false;
  for (const LowOp &Op : Ops)
    for (uint8_t I = 0; I < Op.NumInputs; ++I) {
      const NdVar &Input = Op.Inputs[I];
      if (!Input.isConst())
        continue;
      EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::Scalar);
      SawPointerWidthOne |= Input.Offset == 1 && Input.Size == 8;
    }
  EXPECT_TRUE(SawPointerWidthOne);
}

TEST(LowInstructionBoundary,
     NumericLowIROperandsDefaultToScalarWithoutErasingExactProvenance) {
  std::vector<LowOp> Ops;
  LiftStateBase State(kEntry, 4, Ops);

  State.emit(NdOp::INT_AND, NdVar::tmp(0, 4),
             {NdVar::reg(0, 4), NdVar::cst(0xff, 4)});
  State.emit(NdOp::INT_LEFT, NdVar::tmp(1, 4),
             {NdVar::reg(4, 4), NdVar::cst(31, 4)});
  State.emit(NdOp::INT_ADD, NdVar::tmp(2, 8),
             {NdVar::reg(8, 8), NdVar::dataAddress(0x2000, 8)});
  State.emit(NdOp::COPY, NdVar::tmp(3, 8), {NdVar::cst(0x3000, 8)});

  ASSERT_EQ(Ops.size(), 4u);
  EXPECT_EQ(Ops[0].Inputs[1].Provenance, ConstantAddressProvenance::Scalar);
  EXPECT_EQ(Ops[1].Inputs[1].Provenance, ConstantAddressProvenance::Scalar);
  EXPECT_EQ(Ops[2].Inputs[1].Provenance,
            ConstantAddressProvenance::DataAddress);
  EXPECT_EQ(Ops[3].Inputs[0].Provenance, ConstantAddressProvenance::Unknown);
}

TEST(LowInstructionBoundary,
     CopyPropagatedImmediatesAcquireTheNumericUseRoleOnlyAtThatUse) {
  LowFunc Low;
  Low.Entry = kEntry;
  Low.Name = "copy_propagated_numeric_occurrence";

  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = kEntry;
  Block.EndAddr = kEntry + 0x10;

  const NdVar UnknownValue = NdVar::tmp(0, 4);
  LowOp MaterializeUnknown;
  MaterializeUnknown.Opcode = NdOp::COPY;
  MaterializeUnknown.Addr = kEntry;
  MaterializeUnknown.Output = UnknownValue;
  MaterializeUnknown.addInput(NdVar::cst(0x83, 4));
  Block.Ops.push_back(std::move(MaterializeUnknown));

  LowOp NumericUse;
  NumericUse.Opcode = NdOp::INT_ADD;
  NumericUse.Addr = kEntry + 4;
  NumericUse.Output = NdVar::tmp(8, 4);
  NumericUse.addInput(NdVar::reg(0, 4));
  NumericUse.addInput(UnknownValue);
  Block.Ops.push_back(std::move(NumericUse));

  const NdVar ExactAddress = NdVar::tmp(16, 4);
  LowOp MaterializeAddress;
  MaterializeAddress.Opcode = NdOp::COPY;
  MaterializeAddress.Addr = kEntry + 8;
  MaterializeAddress.Output = ExactAddress;
  MaterializeAddress.addInput(NdVar::dataAddress(0x83, 4, 0x80));
  Block.Ops.push_back(std::move(MaterializeAddress));

  LowOp AddressUse;
  AddressUse.Opcode = NdOp::INT_ADD;
  AddressUse.Addr = kEntry + 0xc;
  AddressUse.Output = NdVar::tmp(24, 4);
  AddressUse.addInput(NdVar::reg(4, 4));
  AddressUse.addInput(ExactAddress);
  Block.Ops.push_back(std::move(AddressUse));
  Low.Blocks.push_back(std::move(Block));

  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits32;
  Segment Data;
  Data.Name = ".rodata";
  Data.VA = 0x80;
  Data.Size = 0x20;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  Image.RelocDataAddrs.insert(0x83);

  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  MedFunc Med = Converter.convert(Low, Arch::X86, BinaryFormat::ELF);
  ASSERT_EQ(Med.Blocks.size(), 1u);

  auto inputAt = [&](va_t Addr) -> const MedVar * {
    for (const MedOp &Op : Med.Blocks.front().Ops)
      if (Op.Addr == Addr && Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2)
        return &Op.Inputs[1];
    return nullptr;
  };
  const MedVar *ScalarUse = inputAt(kEntry + 4);
  const MedVar *RelocatableUse = inputAt(kEntry + 0xc);
  ASSERT_NE(ScalarUse, nullptr);
  ASSERT_NE(RelocatableUse, nullptr);
  ASSERT_TRUE(ScalarUse->isConst());
  ASSERT_TRUE(RelocatableUse->isConst());
  EXPECT_EQ(ScalarUse->Provenance, ConstantAddressProvenance::Scalar);
  EXPECT_EQ(RelocatableUse->Provenance, ConstantAddressProvenance::DataAddress);
}

TEST(LowInstructionBoundary, X86GeneratedStringAndLoopConstantsAreScalar) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    NdOp Opcode;
    uint64_t Value;
  };
  const std::vector<Case> Cases = {
      // rep lodsq: RCX-1 selects the final load and RCX*8 advances RSI.
      {"rep-lods", {0xf3, 0x48, 0xad}, NdOp::INT_SUB, 1},
      // loop $-2: the encoded branch target remains a control-flow address,
      // but the lifter-generated RCX decrement must be a scalar occurrence.
      {"loop", {0xe2, 0xfe}, NdOp::INT_SUB, 1},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(C.Bytes.data(), C.Bytes.size(), kEntry, Insn),
        static_cast<int>(C.Bytes.size()));

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    bool SawGeneratedScalar = false;
    for (const LowOp &Op : Ops) {
      if (Op.Opcode != C.Opcode)
        continue;
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        const NdVar &Input = Op.Inputs[I];
        if (!Input.isConst() || Input.Offset != C.Value || Input.Size != 8)
          continue;
        EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::Scalar);
        SawGeneratedScalar = true;
      }
    }
    EXPECT_TRUE(SawGeneratedScalar);
  }
}

TEST(LowInstructionBoundary,
     X86VSIBGatherConsumesTheExactRelocatedDisplacement) {
  // vpgatherdd %ymm2,0x11223344(,%ymm1,4),%ymm0.  The gather constructs its
  // lane addresses directly instead of calling computeEA, but it must consume
  // the same exact relocation descriptor (including a relocated zero) as an
  // ordinary memory operand.  Its lane scale remains scalar.
  const std::vector<uint8_t> Bytes = {0xc4, 0xe2, 0x6d, 0x90, 0x04,
                                      0x8d, 0x44, 0x33, 0x22, 0x11};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
            static_cast<int>(Bytes.size()));

  RelocatedAddressOperand Reloc;
  Reloc.FieldVA = kEntry + 6;
  Reloc.EncodedValue = 0x11223344;
  Reloc.TargetVA = 0x11223344;
  Reloc.Width = 4;
  Reloc.Provenance = ConstantAddressProvenance::DataAddress;
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops, llvm::ArrayRef<RelocatedAddressOperand>(&Reloc, 1));

  unsigned AddressMatches = 0;
  bool SawScalarScale = false;
  for (const LowOp &Op : Ops)
    for (uint8_t I = 0; I < Op.NumInputs; ++I) {
      const NdVar &Input = Op.Inputs[I];
      if (!Input.isConst())
        continue;
      if (Input.Offset == Reloc.TargetVA && Input.Size == 8) {
        EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::DataAddress);
        ++AddressMatches;
      }
      if (Op.Opcode == NdOp::INT_MULT && Input.Offset == 4 && Input.Size == 8) {
        EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::Scalar);
        SawScalarScale = true;
      }
    }
  EXPECT_EQ(AddressMatches, 1u);
  EXPECT_TRUE(SawScalarScale);
}

TEST(LowInstructionBoundary,
     ArithmeticImmediatesAreScalarUnlessTheirExactFieldRelocates) {
  {
    // cmp rax,0x2000.  A large low-VA object can contain address 0x2000, but
    // this unrelocated encoded bound is still a scalar occurrence.
    const std::vector<uint8_t> Bytes = {0x48, 0x3d, 0x00, 0x20, 0x00, 0x00};
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
              static_cast<int>(Bytes.size()));

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    bool SawScalarBound = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x2000) {
          EXPECT_EQ(Op.Inputs[I].Provenance, ConstantAddressProvenance::Scalar);
          SawScalarBound = true;
        }
    EXPECT_TRUE(SawScalarBound);

    // The same bytes with a loader-authenticated relocation on the exact imm32
    // field must retain address identity.  A value-equal constant elsewhere in
    // the instruction would not match FieldVA and therefore cannot inherit it.
    RelocatedAddressOperand Reloc;
    Reloc.FieldVA = kEntry + 2;
    Reloc.EncodedValue = 0x2000;
    Reloc.TargetVA = 0x2000;
    Reloc.Width = 4;
    Reloc.Provenance = ConstantAddressProvenance::DataAddress;
    Ops.clear();
    Dec.liftToLow(Insn, Ops,
                  llvm::ArrayRef<RelocatedAddressOperand>(&Reloc, 1));
    bool SawRelocatedBound = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x2000) {
          EXPECT_EQ(Op.Inputs[I].Provenance,
                    ConstantAddressProvenance::DataAddress);
          SawRelocatedBound = true;
        }
    EXPECT_TRUE(SawRelocatedBound);
  }

  {
    // cmp r0,#0x100.  In a relocatable ARM object 0x100 can simultaneously be
    // the .bss base; the compare occurrence must remain a scalar bound.
    const std::vector<LowOp> Ops =
        liftARMInstruction(InstructionMode::ARM, {0x01, 0x0c, 0x50, 0xe3});
    bool SawScalarBound = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x100) {
          EXPECT_EQ(Op.Inputs[I].Provenance, ConstantAddressProvenance::Scalar);
          SawScalarBound = true;
        }
    EXPECT_TRUE(SawScalarBound);
  }
}

TEST(LowInstructionBoundary, AArch64AddressDisplacementsCarryScalarProvenance) {
  struct Case {
    std::vector<uint8_t> Bytes;
    uint64_t Displacement;
  };
  // add x12,x12,#0x248; str x11,[sp,#16]. The first value deliberately
  // matches the low-VA table_b address in the issue fixture: its instruction
  // role, not numeric segment membership, proves that it is a displacement.
  const std::vector<Case> Cases = {
      {{0x8c, 0x21, 0x09, 0x91}, 0x248},
      {{0xeb, 0x0b, 0x00, 0xf9}, 16},
  };

  for (const Case &Test : Cases) {
    const std::vector<LowOp> Ops = liftAArch64Instruction(Test.Bytes);
    bool SawScalarDisplacement = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        SawScalarDisplacement |=
            Op.Inputs[I].isConst() &&
            Op.Inputs[I].Offset == Test.Displacement &&
            Op.Inputs[I].Provenance == ConstantAddressProvenance::Scalar;
    EXPECT_TRUE(SawScalarDisplacement);
  }
}

TEST(LowInstructionBoundary,
     AArch64BarePageBaseCompletesOnlyForExactDereference) {
  // adrp x8,0x9000; str x8,[sp,#0x18]; ldr x8,[x8]; ret
  //
  // The page base is itself the complete address of the pointer table.  It is
  // spilled as a first-class pointer and dereferenced before x8 is redefined,
  // so retaining AddressFragment would later reject the stack reload even
  // though this exact instruction occurrence proved the zero page offset.
  LowFunc Function =
      buildAArch64PageBaseUse({0x48, 0x00, 0x00, 0x90, 0xe8, 0x0f, 0x00, 0xf9,
                               0x08, 0x01, 0x40, 0xf9, 0xc0, 0x03, 0x5f, 0xd6});

  const LowOp *Materialization = findAddressMaterialization(Function, kEntry);
  ASSERT_NE(Materialization, nullptr);
  ASSERT_EQ(Materialization->Inputs[0].Offset, 0x9000u);
  EXPECT_EQ(Materialization->Inputs[0].Provenance,
            ConstantAddressProvenance::DataAddress);
  EXPECT_EQ(Materialization->Inputs[0].AddressOwnerVA, 0x9000u);
}

TEST(LowInstructionBoundary,
     AArch64PageStartCollisionDoesNotCompleteOffsetAddress) {
  // adrp x8,0x9000; add x8,x8,#0x20; ldr x8,[x8]; ret
  //
  // A real pointer slot also begins at 0x9000, but this occurrence addresses
  // 0x9020.  Numeric equality with the unrelated page-start slot must not turn
  // the incomplete ADRP result into that slot's relocatable identity.
  LowFunc Function =
      buildAArch64PageBaseUse({0x48, 0x00, 0x00, 0x90, 0x08, 0x81, 0x00, 0x91,
                               0x08, 0x01, 0x40, 0xf9, 0xc0, 0x03, 0x5f, 0xd6});

  const LowOp *Materialization = findAddressMaterialization(Function, kEntry);
  ASSERT_NE(Materialization, nullptr);
  ASSERT_EQ(Materialization->Inputs[0].Offset, 0x9000u);
  EXPECT_EQ(Materialization->Inputs[0].Provenance,
            ConstantAddressProvenance::AddressFragment);
  EXPECT_EQ(Materialization->Inputs[0].AddressOwnerVA, InvalidVA);
}

TEST(LowInstructionBoundary, ARMExceptionReturnsFailClosed) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x04, 0xf0, 0x5e, 0xe2}, // subs pc, lr, #4
      {0x0e, 0xf0, 0xb0, 0xe1}, // movs pc, lr
      {0x00, 0x80, 0xfd, 0xe8}, // ldm sp!, {pc} ^
      {0x00, 0x0a, 0xbd, 0xf8}, // rfeia sp!
      {0x6e, 0x00, 0x60, 0xe1}, // eret
  };

  for (size_t CaseIndex = 0; CaseIndex < Cases.size(); ++CaseIndex) {
    SCOPED_TRACE(::testing::Message() << "case " << CaseIndex);
    const std::vector<uint8_t> &Bytes = Cases[CaseIndex];
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::ARM));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
              static_cast<int>(Bytes.size()));
    std::vector<LowOp> Ops;
    EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  }
}

TEST(LowInstructionBoundary, ARMTerminatorClassificationUsesOperands) {
  struct Case {
    InstructionMode Mode;
    std::vector<uint8_t> Bytes;
    bool IsTerminator;
  };
  const std::vector<Case> Cases = {
      {InstructionMode::Thumb, {0x10, 0xbc}, false}, // pop {r4}
      {InstructionMode::Thumb, {0x00, 0xbd}, true},  // pop {pc}
      {InstructionMode::ARM, {0x10, 0x00, 0xbd, 0xe8}, false},
      {InstructionMode::ARM, {0x10, 0x80, 0xb0, 0xe8}, true},
      {InstructionMode::ARM, {0x00, 0x80, 0xbd, 0x08}, false},
  };

  for (const Case &Test : Cases) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::ARM, Test.Mode));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Test.Bytes.data(), Test.Bytes.size(), kEntry,
                                   Insn),
              static_cast<int>(Test.Bytes.size()));
    EXPECT_EQ(Dec.isFunctionTerminator(Insn), Test.IsTerminator);
  }
}

TEST(LowInstructionBoundary, ARMNonPCPopDoesNotTerminateDecode) {
  // pop {r4}; bx lr
  LowFunc Function = buildFunction(Arch::ARM, InstructionMode::Thumb,
                                   {0x10, 0xbc, 0x70, 0x47});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 2u);
  EXPECT_EQ(Entry->InstructionBoundaries[0].Control,
            LowInstructionControl::None);
  EXPECT_EQ(Entry->InstructionBoundaries[1].Control,
            LowInstructionControl::Return);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary, ARMConditionalPCWritesKeepFalseFallthrough) {
  struct Case {
    std::vector<uint8_t> Bytes;
    LowInstructionControl Control;
    bool IsIndirect;
  };
  const std::vector<Case> Cases = {
      {{0x00, 0xf0, 0x90, 0x05}, LowInstructionControl::Branch, true},
      {{0x00, 0x80, 0xbd, 0x08},
       LowInstructionControl::ConditionalReturn,
       false},
      {{0x0e, 0xf0, 0xa0, 0x01},
       LowInstructionControl::ConditionalReturn,
       false},
      {{0x01, 0xf0, 0x80, 0x00}, LowInstructionControl::Branch, true},
  };

  for (const Case &Test : Cases) {
    std::vector<uint8_t> Bytes = Test.Bytes;
    Bytes.insert(Bytes.end(), {0x1e, 0xff, 0x2f, 0xe1}); // bx lr
    LowFunc Function =
        buildFunction(Arch::ARM, InstructionMode::ARM, std::move(Bytes));

    const LowBlock *Entry = findBlock(Function, kEntry);
    const LowBlock *Fallthrough = findBlock(Function, kEntry + 4);
    ASSERT_NE(Entry, nullptr);
    ASSERT_NE(Fallthrough, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
    const LowInstructionBoundary &Boundary =
        Entry->InstructionBoundaries.front();
    EXPECT_EQ(Boundary.Control, Test.Control);
    EXPECT_TRUE(hasLowInstructionControlFlag(
        Boundary.ControlFlags, LowInstructionControlFlag::Conditional));
    EXPECT_TRUE(hasLowInstructionControlFlag(
        Boundary.ControlFlags, LowInstructionControlFlag::InstructionGuard));
    EXPECT_EQ(hasLowInstructionControlFlag(Boundary.ControlFlags,
                                           LowInstructionControlFlag::Indirect),
              Test.IsIndirect);
    EXPECT_EQ(
        std::count(Entry->Succs.begin(), Entry->Succs.end(), Fallthrough->Id),
        1);
    EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
        Function, LowInstructionBoundaryRequirement::Required)));
  }
}

TEST(LowInstructionBoundary, ARMConditionalDirectBranchIsNotLocalGuard) {
  // bne +0 (to 0x1008); bx lr; bx lr.  The lifter spells the branch as
  // `COND_BR next,!NE; BRANCH target`, but that pair is the guest control flow
  // itself rather than a same-instruction guard around a memory/call effect.
  LowFunc Function = buildFunction(
      Arch::ARM, InstructionMode::ARM,
      {0x00, 0x00, 0x00, 0x1a, 0x1e, 0xff, 0x2f, 0xe1, 0x1e, 0xff, 0x2f, 0xe1});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
  const LowInstructionBoundary &Boundary = Entry->InstructionBoundaries.front();
  EXPECT_EQ(Boundary.Control, LowInstructionControl::Branch);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Boundary.ControlFlags, LowInstructionControlFlag::Conditional));
  EXPECT_FALSE(hasLowInstructionControlFlag(
      Boundary.ControlFlags, LowInstructionControlFlag::InstructionGuard));
  EXPECT_EQ(Entry->Succs.size(), 2u);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary, ARMConditionalMemoryUsesOneInstructionLocalGuard) {
  struct Case {
    std::vector<uint8_t> Bytes;
    NdOp Effect;
  };
  const std::vector<Case> Cases = {
      {{0x00, 0x10, 0x90, 0x05}, NdOp::LOAD},  // ldreq r1, [r0]
      {{0x00, 0x10, 0x80, 0x05}, NdOp::STORE}, // streq r1, [r0]
  };

  for (const Case &Test : Cases) {
    std::vector<uint8_t> Bytes = Test.Bytes;
    Bytes.insert(Bytes.end(), {0x1e, 0xff, 0x2f, 0xe1}); // bx lr
    LowFunc Function =
        buildFunction(Arch::ARM, InstructionMode::ARM, std::move(Bytes));

    const LowBlock *Entry = findBlock(Function, kEntry);
    const LowBlock *Continuation = findBlock(Function, kEntry + 4);
    ASSERT_NE(Entry, nullptr);
    ASSERT_NE(Continuation, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
    EXPECT_TRUE(hasLowInstructionControlFlag(
        Entry->InstructionBoundaries.front().ControlFlags,
        LowInstructionControlFlag::InstructionGuard));
    EXPECT_TRUE(containsOp(Entry->Ops, NdOp::COND_BR));
    EXPECT_TRUE(containsOp(Entry->Ops, Test.Effect));
    EXPECT_FALSE(containsOp(Entry->Ops, NdOp::SELECT));
    if (Test.Effect == NdOp::STORE)
      EXPECT_FALSE(containsOp(Entry->Ops, NdOp::LOAD))
          << "a skipped store must not use a read-modify-write fallback";

    auto Guard =
        std::find_if(Entry->Ops.begin(), Entry->Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::COND_BR;
        });
    auto Effect =
        std::find_if(Entry->Ops.begin(), Entry->Ops.end(),
                     [&](const LowOp &Op) { return Op.Opcode == Test.Effect; });
    ASSERT_NE(Guard, Entry->Ops.end());
    ASSERT_NE(Effect, Entry->Ops.end());
    EXPECT_LT(Guard, Effect);

    ASSERT_EQ(Entry->Succs.size(), 1u);
    EXPECT_EQ(Entry->Succs.front(), Continuation->Id);
    EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
        Function, LowInstructionBoundaryRequirement::Required)));
  }
}

TEST(LowInstructionBoundary, RejectsForgedOrMissingInstructionGuardMetadata) {
  LowFunc Function = buildFunction(
      Arch::ARM, InstructionMode::ARM,
      {0x00, 0x10, 0x90, 0x05, 0x1e, 0xff, 0x2f, 0xe1}); // ldreq; bx lr
  LowBlock *Entry = Function.blockFor(kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);

  LowInstructionBoundary &Boundary = Entry->InstructionBoundaries.front();
  const uint16_t WithoutGuard =
      static_cast<uint16_t>(Boundary.ControlFlags) &
      ~static_cast<uint16_t>(LowInstructionControlFlag::InstructionGuard);
  Boundary.ControlFlags = static_cast<LowInstructionControlFlag>(WithoutGuard);
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                Function, LowInstructionBoundaryRequirement::Required)),
            "");

  LowFunc Ordinary = buildFunction(
      Arch::ARM, InstructionMode::ARM,
      {0x00, 0x00, 0x00, 0xea, 0x1e, 0xff, 0x2f, 0xe1}); // b; bx lr
  LowBlock *OrdinaryEntry = Ordinary.blockFor(kEntry);
  ASSERT_NE(OrdinaryEntry, nullptr);
  ASSERT_EQ(OrdinaryEntry->InstructionBoundaries.size(), 1u);
  OrdinaryEntry->InstructionBoundaries.front().ControlFlags |=
      LowInstructionControlFlag::InstructionGuard;
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                Ordinary, LowInstructionBoundaryRequirement::Required)),
            "");
}

TEST(LowInstructionBoundary, ARMPredicatedMemoryIsMaterializedBeforeMedSSA) {
  // ldreq r1, [r0]; mov r0, r1; bx lr.  The use of r1 keeps the effect and its
  // join phi live through the complete LowToMed pipeline.
  LowFunc Low = buildFunction(
      Arch::ARM, InstructionMode::ARM,
      {0x00, 0x10, 0x90, 0x05, 0x01, 0x00, 0xa0, 0xe1, 0x1e, 0xff, 0x2f, 0xe1});
  LowBlock *LowGuard = Low.blockFor(kEntry);
  LowBlock *LowHandler = Low.blockFor(kEntry + 4);
  ASSERT_NE(LowGuard, nullptr);
  ASSERT_NE(LowHandler, nullptr);
  ExceptionalEdge ToHandler;
  ToHandler.BlockId = LowHandler->Id;
  ToHandler.TargetVA = LowHandler->StartAddr;
  ToHandler.Kind = ExceptionalEdgeKind::ItaniumCatchPad;
  LowGuard->ExceptionalSuccs.push_back(ToHandler);
  ExceptionalEdge FromGuard = ToHandler;
  FromGuard.BlockId = LowGuard->Id;
  LowHandler->ExceptionalPreds.push_back(FromGuard);
  MedFunc Med = LowToMedConverter().convert(Low, Arch::ARM, BinaryFormat::ELF);

  const MedBlock *Guard = nullptr;
  for (const MedBlock &Block : Med.Blocks)
    if (std::any_of(Block.Ops.begin(), Block.Ops.end(), [](const MedOp &Op) {
          return Op.Opcode == NdOp::COND_BR;
        })) {
      Guard = &Block;
      break;
    }
  ASSERT_NE(Guard, nullptr);
  ASSERT_EQ(Guard->Succs.size(), 2u);

  const MedBlock &Skip = Med.Blocks[Guard->Succs[0]];
  const MedBlock &Effect = Med.Blocks[Guard->Succs[1]];
  EXPECT_TRUE(Guard->ExceptionalSuccs.empty())
      << "the skipped path must not retain the effect's exception edge";
  ASSERT_EQ(Effect.ExceptionalSuccs.size(), 1u);
  EXPECT_EQ(Effect.ExceptionalSuccs.front().BlockId, LowHandler->Id);
  EXPECT_NE(std::find_if(Skip.ExceptionalPreds.begin(),
                         Skip.ExceptionalPreds.end(),
                         [&](const ExceptionalEdge &Edge) {
                           return Edge.BlockId == Effect.Id;
                         }),
            Skip.ExceptionalPreds.end());
  EXPECT_EQ(std::find_if(Skip.ExceptionalPreds.begin(),
                         Skip.ExceptionalPreds.end(),
                         [&](const ExceptionalEdge &Edge) {
                           return Edge.BlockId == Guard->Id;
                         }),
            Skip.ExceptionalPreds.end());
  EXPECT_TRUE(
      std::any_of(Effect.Ops.begin(), Effect.Ops.end(),
                  [](const MedOp &Op) { return Op.Opcode == NdOp::LOAD; }));
  ASSERT_EQ(Effect.Succs.size(), 1u);
  EXPECT_EQ(Effect.Succs.front(), Skip.Id);
  EXPECT_NE(std::find(Skip.Preds.begin(), Skip.Preds.end(), Guard->Id),
            Skip.Preds.end());
  EXPECT_NE(std::find(Skip.Preds.begin(), Skip.Preds.end(), Effect.Id),
            Skip.Preds.end());

  EXPECT_TRUE(std::any_of(Skip.Phis.begin(), Skip.Phis.end(),
                          [](const PhiNode &Phi) {
                            return Phi.Output.Kind == MedVar::Reg &&
                                   Phi.Output.RegOff == armreg::R1;
                          }))
      << "the skip edge must retain old r1 while the effect edge defines it";
  EXPECT_TRUE(verifyMedFunc(Med, "test-predicated-memory-materialization"));
}

TEST(LowInstructionBoundary, ConditionalIndirectBranchKeepsFallthrough) {
  // bxeq r0; bx lr
  LowFunc Function =
      buildFunction(Arch::ARM, InstructionMode::Default,
                    {0x10, 0xff, 0x2f, 0x01, 0x1e, 0xff, 0x2f, 0xe1});

  const LowBlock *Entry = findBlock(Function, kEntry);
  const LowBlock *Fallthrough = findBlock(Function, kEntry + 4);
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Fallthrough, nullptr);
  EXPECT_TRUE(Entry->hasSucc(Fallthrough->Id));
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
  EXPECT_EQ(Entry->InstructionBoundaries.front().TargetMode,
            LowInstructionTargetMode::FromTargetBit0);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary,
     MemoryIndirectTailCallKeepsInstructionLocalTargetSlice) {
  // jmpq *(%rcx,%rax,8).  An unresolved terminal indirect jump is modeled as
  // an indirect tail call.  Its target LOAD is part of this same instruction;
  // dropping that definition leaves a reusable instruction-local temp whose
  // next occurrence can silently become the call target during SSA renaming.
  LowFunc Function =
      buildFunction(Arch::X64, InstructionMode::Default, {0xff, 0x24, 0xc1});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  auto Call =
      std::find_if(Entry->Ops.begin(), Entry->Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::INDIR_CALL;
      });
  ASSERT_NE(Call, Entry->Ops.end());
  ASSERT_EQ(Call->NumInputs, 1u);
  EXPECT_TRUE(Call->Inputs[0].isTemp());

  auto TargetLoad =
      std::find_if(Entry->Ops.begin(), Call, [&](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD &&
               Op.Output.Space == Call->Inputs[0].Space &&
               Op.Output.Offset == Call->Inputs[0].Offset &&
               Op.Output.Size == Call->Inputs[0].Size;
      });
  ASSERT_NE(TargetLoad, Call)
      << "the tail-call target must retain its same-instruction LOAD";
  EXPECT_NE(
      std::find_if(std::next(Call), Entry->Ops.end(),
                   [](const LowOp &Op) { return Op.Opcode == NdOp::RETURN; }),
      Entry->Ops.end());
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));

  MedFunc Med =
      LowToMedConverter().convert(Function, Arch::X64, BinaryFormat::ELF);
  ASSERT_EQ(Med.Blocks.size(), 1u);
  auto MedCall = std::find_if(
      Med.Blocks[0].Ops.begin(), Med.Blocks[0].Ops.end(),
      [](const MedOp &Op) { return Op.Opcode == NdOp::INDIR_CALL; });
  ASSERT_NE(MedCall, Med.Blocks[0].Ops.end());
  ASSERT_EQ(MedCall->NumInputs, 1u);
  EXPECT_NE(std::find_if(Med.Blocks[0].Ops.begin(), MedCall,
                         [&](const MedOp &Op) {
                           return Op.Opcode == NdOp::LOAD &&
                                  Op.Output.Kind == MedCall->Inputs[0].Kind &&
                                  Op.Output.Id == MedCall->Inputs[0].Id &&
                                  Op.Output.SSAVer == MedCall->Inputs[0].SSAVer;
                         }),
            MedCall);
  EXPECT_TRUE(verifyMedFunc(Med, "memory-indirect-tail-call"));
}

TEST(LowInstructionBoundary,
     RelocationAddressTakenTrivialBlockSurvivesMedSimplification) {
  constexpr va_t TrivialAddress = kEntry + 0x10;
  constexpr va_t TargetAddress = kEntry + 0x20;
  constexpr va_t PointerSlot = 0x2000;

  LowFunc Low;
  Low.Entry = kEntry;
  Low.Name = "address_taken_trivial_block";

  auto makeBranchBlock = [](int Id, va_t Address, int Succ, va_t Target) {
    LowBlock Block;
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 2;
    Block.Succs = {Succ};
    LowOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.Addr = Address;
    Branch.addInput(NdVar::cst(Target, 8));
    Block.Ops.push_back(std::move(Branch));
    return Block;
  };
  Low.Blocks.push_back(makeBranchBlock(0, kEntry, 1, TrivialAddress));
  Low.Blocks.push_back(makeBranchBlock(1, TrivialAddress, 2, TargetAddress));
  Low.Blocks[1].Preds = {0};
  LowBlock Target;
  Target.Id = 2;
  Target.StartAddr = TargetAddress;
  Target.EndAddr = TargetAddress + 1;
  Target.Preds = {1};
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = TargetAddress;
  Target.Ops.push_back(std::move(Return));
  Low.Blocks.push_back(std::move(Target));

  auto convert = [&](bool AddressTaken) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.Name = ".text";
    Text.VA = kEntry;
    Text.Size = 0x40;
    Text.FileSz = Text.Size;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0);
    Image.Segments.push_back(std::move(Text));
    Segment Data;
    Data.Name = ".data.rel.ro";
    Data.VA = PointerSlot;
    Data.Size = 8;
    Data.FileSz = Data.Size;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.assign(Data.Size, 0);
    for (unsigned I = 0; I < 8; ++I)
      Data.Data[I] = static_cast<uint8_t>(TrivialAddress >> (I * 8));
    Image.Segments.push_back(std::move(Data));
    if (AddressTaken)
      Image.CodePtrRelocSlots.insert(PointerSlot);

    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    return Converter.convert(Low, Arch::X64, BinaryFormat::ELF);
  };
  auto hasBlockAt = [](const MedFunc &Func, va_t Address) {
    return std::any_of(
        Func.Blocks.begin(), Func.Blocks.end(),
        [&](const MedBlock &Block) { return Block.StartAddr == Address; });
  };

  const MedFunc Unreferenced = convert(false);
  const MedFunc AddressTaken = convert(true);
  EXPECT_FALSE(hasBlockAt(Unreferenced, TrivialAddress));
  EXPECT_TRUE(hasBlockAt(AddressTaken, TrivialAddress));
  EXPECT_TRUE(verifyMedFunc(AddressTaken, "address-taken-trivial-block"));
}

TEST(LowInstructionBoundary, ConditionalNoReturnCallKeepsFalsePath) {
  Symbol Abort = Symbol::makeFunc(kEntry + 0x10);
  Abort.Name = "abort";

  // bleq abort; bx lr
  LowFunc Function = buildFunction(
      Arch::ARM, InstructionMode::Default,
      {0x02, 0x00, 0x00, 0x0b, 0x1e, 0xff, 0x2f, 0xe1}, {std::move(Abort)});

  const LowBlock *Entry = findBlock(Function, kEntry);
  const LowBlock *Fallthrough = findBlock(Function, kEntry + 4);
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Fallthrough, nullptr);
  EXPECT_TRUE(Entry->hasSucc(Fallthrough->Id));
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
  const LowInstructionBoundary &Call = Entry->InstructionBoundaries.front();
  EXPECT_EQ(Call.Control, LowInstructionControl::ConditionalCall);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Call.ControlFlags, LowInstructionControlFlag::NoReturn));
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));

  MedFunc Med =
      LowToMedConverter().convert(Function, Arch::ARM, BinaryFormat::ELF);
  const MedBlock *Guard = nullptr;
  for (const MedBlock &Block : Med.Blocks)
    if (std::any_of(Block.Ops.begin(), Block.Ops.end(), [](const MedOp &Op) {
          return Op.Opcode == NdOp::COND_BR;
        })) {
      Guard = &Block;
      break;
    }
  ASSERT_NE(Guard, nullptr);
  ASSERT_EQ(Guard->Succs.size(), 2u);
  const MedBlock &Taken = Med.Blocks[Guard->Succs[1]];
  EXPECT_TRUE(Taken.Succs.empty())
      << "a selected no-return call must not rejoin its false continuation";
  EXPECT_TRUE(
      std::any_of(Taken.Ops.begin(), Taken.Ops.end(), [](const MedOp &Op) {
        return Op.Opcode == NdOp::CALL && Op.DoesNotReturn;
      }));
  EXPECT_TRUE(verifyMedFunc(Med, "test-predicated-no-return-call"));
}

TEST(LowInstructionBoundary, CrossChecksControlAgainstLowOpSlice) {
  LowFunc Function = buildFunction(Arch::X64, InstructionMode::Default,
                                   {0xe8, 0x03, 0x00, 0x00, 0x00, 0xc3});
  LowBlock *Entry = Function.blockFor(kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_FALSE(Entry->InstructionBoundaries.empty());
  LowInstructionBoundary &Call = Entry->InstructionBoundaries.front();
  ASSERT_TRUE(Call.Immediate.has_value());

  ++*Call.Immediate;
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Function, LowInstructionBoundaryRequirement::Required))
          .find("direct target disagrees"),
      std::string::npos);
  --*Call.Immediate;

  Call.TargetMode = LowInstructionTargetMode::Thumb;
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Function, LowInstructionBoundaryRequirement::Required))
          .find("fixed target mode requires"),
      std::string::npos);
  Call.TargetMode = LowInstructionTargetMode::Preserve;

  auto CallOp =
      std::find_if(Entry->Ops.begin(), Entry->Ops.end(),
                   [](const LowOp &Op) { return Op.Opcode == NdOp::CALL; });
  ASSERT_NE(CallOp, Entry->Ops.end());
  CallOp->Opcode = NdOp::INDIR_CALL;
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Function, LowInstructionBoundaryRequirement::Required))
          .find("control flags disagree"),
      std::string::npos);

  LowFunc Return = buildFunction(Arch::X64, InstructionMode::Default, {0xc3});
  LowBlock *ReturnEntry = Return.blockFor(kEntry);
  ASSERT_NE(ReturnEntry, nullptr);
  LowInstructionBoundary &ReturnBoundary =
      ReturnEntry->InstructionBoundaries.front();
  ReturnBoundary.Control = LowInstructionControl::None;
  ReturnBoundary.ControlFlags = LowInstructionControlFlag::None;
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                          Return, LowInstructionBoundaryRequirement::Required))
                .find("control class disagrees"),
            std::string::npos);

  LowFunc Trap =
      buildFunction(Arch::X64, InstructionMode::Default, {0x0f, 0x0b});
  const LowBlock *TrapEntry = findBlock(Trap, kEntry);
  ASSERT_NE(TrapEntry, nullptr);
  ASSERT_EQ(TrapEntry->InstructionBoundaries.size(), 1u);
  EXPECT_EQ(TrapEntry->InstructionBoundaries.front().Control,
            LowInstructionControl::Terminator);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Trap, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary, AllowsZeroOpInstructionsAndDetectsAbsence) {
  LowBlock ZeroOp;
  ZeroOp.Id = 0;
  ZeroOp.StartAddr = kEntry;
  ZeroOp.EndAddr = kEntry + 2;
  ZeroOp.InstructionBoundaries = {
      {kEntry, 1, 0, 0, InstructionMode::Default, LowInstructionControl::None,
       LowInstructionControlFlag::None, LowInstructionTargetMode::Preserve,
       std::nullopt},
      {kEntry + 1, 1, 0, 0, InstructionMode::Default,
       LowInstructionControl::None, LowInstructionControlFlag::None,
       LowInstructionTargetMode::Preserve, std::nullopt},
  };
  EXPECT_TRUE(ZeroOp.hasInstructionBoundaries());
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      ZeroOp, LowInstructionBoundaryRequirement::Required)));

  LowBlock Legacy;
  Legacy.Id = 1;
  Legacy.StartAddr = 0x2000;
  Legacy.EndAddr = 0x2001;
  LowOp Nop;
  Nop.Opcode = NdOp::NOP;
  Nop.Addr = Legacy.StartAddr;
  Legacy.Ops.push_back(Nop);

  EXPECT_FALSE(Legacy.hasInstructionBoundaries());
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Legacy, LowInstructionBoundaryRequirement::Optional)));
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                          Legacy, LowInstructionBoundaryRequirement::Required))
                .find("metadata is missing"),
            std::string::npos);
}

TEST(LowInstructionBoundary, RejectsOverflowAndNonCanonicalOpCoverage) {
  LowBlock Overflow;
  Overflow.Id = 0;
  Overflow.StartAddr = std::numeric_limits<va_t>::max() - 1;
  Overflow.EndAddr = std::numeric_limits<va_t>::max();
  Overflow.InstructionBoundaries.push_back(
      {Overflow.StartAddr, 2, 0, 0, InstructionMode::Default,
       LowInstructionControl::None, LowInstructionControlFlag::None,
       LowInstructionTargetMode::Preserve, std::nullopt});
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Overflow, LowInstructionBoundaryRequirement::Required))
          .find("overflows"),
      std::string::npos);

  LowBlock Gap;
  Gap.Id = 1;
  Gap.StartAddr = 0x3000;
  Gap.EndAddr = 0x3001;
  LowOp Nop;
  Nop.Opcode = NdOp::NOP;
  Nop.Addr = Gap.StartAddr;
  Gap.Ops.push_back(Nop);
  Gap.InstructionBoundaries.push_back(
      {Gap.StartAddr, 1, 1, 0, InstructionMode::Default,
       LowInstructionControl::None, LowInstructionControlFlag::None,
       LowInstructionTargetMode::Preserve, std::nullopt});
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                          Gap, LowInstructionBoundaryRequirement::Required))
                .find("non-canonical op slice"),
            std::string::npos);

  LowBlock Complete;
  Complete.Id = 2;
  Complete.StartAddr = 0x4000;
  Complete.EndAddr = 0x4001;
  Complete.InstructionBoundaries.push_back(
      {Complete.StartAddr, 1, 0, 0, InstructionMode::Default,
       LowInstructionControl::None, LowInstructionControlFlag::None,
       LowInstructionTargetMode::Preserve, std::nullopt});
  LowBlock Missing;
  Missing.Id = 3;
  Missing.StartAddr = 0x5000;
  Missing.EndAddr = 0x5001;
  LowFunc Partial;
  Partial.Blocks = {Complete, Missing};
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                          Partial, LowInstructionBoundaryRequirement::Optional))
                .find("metadata is missing"),
            std::string::npos);
}

} // namespace
