//===- LowInstructionBoundaryTests.cpp - LowIR instruction provenance ----===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/ARMRegs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kEntry = 0x1000;

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
                                      const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::ARM, Mode)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  int Size = Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn);
  if (Size <= 0) {
    ADD_FAILURE() << "instruction decode failed";
    return {};
  }
  EXPECT_EQ(static_cast<size_t>(Size), Bytes.size());

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
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
