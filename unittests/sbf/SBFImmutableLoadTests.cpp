//===- SBFImmutableLoadTests.cpp - Immutable image load facts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks relocated-image constants at the shared transfer boundary, including
/// CALLX refinement across blocks and refusal for mutable or unmapped memory.
///
//===----------------------------------------------------------------------===//

#include "SBFSolanaTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFDataflow.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <limits>

namespace neverd::sbf {
namespace {

TEST(SBFImmutableLoad, PreservesWidthAndReadsBaseBeforeOverwritingIt) {
  const std::array<uint8_t, 9> Bytes{0x11, 0x80, 0x81, 0x82, 0x83,
                                     0x84, 0x85, 0x86, 0x87};
  struct Case {
    Opcode Op;
    uint64_t Value;
  };
  for (const auto &Test :
       {Case{Opcode::LD_B_REG, 0x80}, Case{Opcode::LD_H_REG, 0x8180},
        Case{Opcode::LD_W_REG, 0x83828180},
        Case{Opcode::LD_DW_REG, UINT64_C(0x8786858483828180)}}) {
    auto Program = analyze(test::makeImage({test::encode(Opcode::MOV64_IMM, 2),
                                            test::encode(Test.Op, 2, 2, 1),
                                            test::encode(Opcode::EXIT)},
                                           Bytes));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    ASSERT_FALSE(Program->Med.Blocks.empty());
    const auto &Block = Program->Med.Blocks.front();
    EXPECT_EQ(Block.Outputs[2].ValueKind, RegisterValue::Kind::Constant);
    EXPECT_EQ(Block.Outputs[2].Value, Test.Value);
    MachineState Replayed;
    Replayed.Registers = Block.Inputs;
    for (const auto &Instruction : Program->Med.Instructions)
      applyTransfer(Instruction, Replayed, Program->ExecutableImage);
    EXPECT_EQ(Replayed.Registers, Block.Outputs);
  }
}

TEST(SBFImmutableLoad, RefinesAPointerLoadedAcrossABlockBoundary) {
  std::array<uint8_t, sizeof(uint64_t)> Bytes;
  llvm::support::endian::write64le(Bytes.data(),
                                   kBytecodeStart + 5 * kInstructionSize + 7);
  auto Program = analyze(test::makeImage(
      {test::encode(Opcode::MOV64_IMM, 3),
       test::encode(Opcode::LD_DW_REG, 7, 3), test::encode(Opcode::JA),
       test::encode(Opcode::CALL_REG, 7), test::encode(Opcode::EXIT),
       test::encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
       test::encode(Opcode::EXIT)},
      Bytes));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_EQ(Program->Low.Instructions[3].Call, CallKind::Internal);
  EXPECT_EQ(Program->Low.Instructions[3].CallTarget, 5u);
  EXPECT_NE(findFunction(*Program, "0x100000028"), nullptr);
}

TEST(SBFImmutableLoad, DoesNotReadMutableTruncatedOrNonDataVisibleRanges) {
  std::array<uint8_t, sizeof(uint64_t)> Bytes;
  llvm::support::endian::write64le(Bytes.data(),
                                   kBytecodeStart + 5 * kInstructionSize);
  for (const va_t Address :
       {kInputStart, kStackStart, kHeapStart, kBytecodeStart,
        kRodataStartV3 + 1, std::numeric_limits<uint64_t>::max()}) {
    SCOPED_TRACE(Address);
    auto Text = test::loadImm64(3, Address);
    Text.push_back(test::encode(Opcode::LD_DW_REG, 7, 3));
    Text.push_back(test::encode(Opcode::CALL_REG, 7));
    Text.push_back(test::encode(Opcode::EXIT));
    Text.push_back(test::encode(Opcode::EXIT));
    auto Program = analyze(test::makeImage(Text, Bytes));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    EXPECT_EQ(Program->Low.Instructions[3].Call, CallKind::Indirect);
    EXPECT_FALSE(Program->Low.Instructions[3].CallTarget);
  }
}

TEST(SBFImmutableLoad, AnUnknownCallCannotPreserveACallerSavedAddress) {
  std::array<uint8_t, sizeof(uint64_t)> Bytes;
  llvm::support::endian::write64le(Bytes.data(),
                                   kBytecodeStart + 5 * kInstructionSize);
  auto Program = analyze(test::makeImage(
      {test::encode(Opcode::MOV64_IMM, 3),
       test::encode(Opcode::CALL_IMM, 0, 1, 0, 3),
       test::encode(Opcode::LD_DW_REG, 7, 3), test::encode(Opcode::CALL_REG, 7),
       test::encode(Opcode::EXIT), test::encode(Opcode::EXIT)},
      Bytes));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_EQ(Program->Low.Instructions[3].Call, CallKind::Indirect);
  EXPECT_FALSE(Program->Low.Instructions[3].CallTarget);
}

} // namespace
} // namespace neverd::sbf
