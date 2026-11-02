//===- DirectTailCallMetadataTests.cpp - Tail-call targets ----------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <set>
#include <vector>

using namespace neverd;

namespace {

LowFunc build(std::vector<uint8_t> Bytes, std::set<va_t> Entries,
              Arch Architecture = Arch::X64) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Mode = InstructionMode::Default;
  Image.Bits = Architecture == Arch::X64 || Architecture == Arch::AArch64
                   ? Bitness::Bits64
                   : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Base = 0x1000;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(Text));
  Decoder Dec;
  if (!Dec.init(Image)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  return Builder.build(Image, Dec, 0x1000, "tail_metadata");
}

void expectDirectTarget(const LowFunc &Function, va_t Target,
                        bool Synthetic = false) {
  auto Error = validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  unsigned Tails = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Control == LowInstructionControl::TailCall) {
        ++Tails;
        EXPECT_EQ(Boundary.Immediate, Target);
        bool FoundCall = false, FoundReturn = false;
        for (uint64_t I = Boundary.FirstOp;
             I < Boundary.FirstOp + Boundary.OpCount; ++I) {
          const auto &Op = Block.Ops[I];
          EXPECT_EQ(Op.Seq < 0, Synthetic);
          if (Op.Opcode == NdOp::CALL) {
            ASSERT_EQ(Op.NumInputs, 1u);
            EXPECT_TRUE(Op.Inputs[0].isConst());
            EXPECT_EQ(Op.Inputs[0].Offset, Target);
            FoundCall = true;
          }
          FoundReturn |= Op.Opcode == NdOp::RETURN;
        }
        EXPECT_TRUE(FoundCall);
        EXPECT_TRUE(FoundReturn);
      }
  EXPECT_EQ(Tails, 1u);
}

TEST(DirectTailCallMetadata, FallthroughEntryPublishesItsTarget) {
  for (auto Architecture : {Arch::X86, Arch::X64})
    expectDirectTarget(build({0x90, 0xc3}, {0x1000, 0x1001}, Architecture),
                       0x1001, true);
  expectDirectTarget(build({0x1f, 0x20, 0x03, 0xd5, 0xc0, 0x03, 0x5f, 0xd6},
                           {0x1000, 0x1004}, Arch::AArch64),
                     0x1004, true);
}

TEST(DirectTailCallMetadata, ConditionalEntryPublishesItsTarget) {
  for (auto Architecture : {Arch::X86, Arch::X64})
    expectDirectTarget(
        build({0x75, 0x01, 0xc3, 0xc3}, {0x1000, 0x1003}, Architecture), 0x1003,
        true);
  expectDirectTarget(build({0x41, 0x00, 0x00, 0x54, 0xc0, 0x03, 0x5f, 0xd6,
                            0xc0, 0x03, 0x5f, 0xd6},
                           {0x1000, 0x1008}, Arch::AArch64),
                     0x1008, true);
}

TEST(DirectTailCallMetadata, ExistingDirectJumpKeepsItsTarget) {
  expectDirectTarget(build({0xeb, 0x00, 0xc3}, {0x1000, 0x1002}), 0x1002);
}

TEST(DirectTailCallMetadata, OrdinaryCallAndIndirectTailRemainDistinct) {
  auto Direct =
      build({0xe8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0xc3}, {0x1000, 0x1006});
  auto Indirect = build({0xff, 0xe0}, {0x1000});
  for (const auto *Function : {&Direct, &Indirect}) {
    auto Error = validateLowInstructionBoundaries(
        *Function, LowInstructionBoundaryRequirement::Required);
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }
  bool SawDirect = false, SawIndirect = false;
  for (const auto &Block : Direct.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Control == LowInstructionControl::Call) {
        EXPECT_EQ(Boundary.Immediate, 0x1006u);
        SawDirect = true;
      }
  for (const auto &Block : Indirect.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Control == LowInstructionControl::TailCall) {
        EXPECT_FALSE(Boundary.Immediate);
        EXPECT_TRUE(hasLowInstructionControlFlag(
            Boundary.ControlFlags, LowInstructionControlFlag::Indirect));
        SawIndirect = true;
      }
  EXPECT_TRUE(SawDirect);
  EXPECT_TRUE(SawIndirect);
}

} // namespace
