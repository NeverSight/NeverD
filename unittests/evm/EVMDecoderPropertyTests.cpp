//===- EVMDecoderPropertyTests.cpp - EVM decoder invariants -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/bytecode/EVMDecoder.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace neverd::evm {
namespace {

constexpr std::array AllHardforks = {
#define EVM_HARDFORK(NAME, SPELLING) Hardfork::NAME,
#include "neverd/evm/bytecode/EVMHardforks.def"
};

constexpr uint32_t kPropertySeed = 0x04e56444;
constexpr size_t kRandomCaseCount = 16'384;
constexpr size_t kMaximumRandomCodeSize = 4'096;

uint32_t xorshift32(uint32_t &State) {
  State ^= State << 13;
  State ^= State >> 17;
  State ^= State << 5;
  return State;
}

bool haveSameDiagnostics(llvm::ArrayRef<Diagnostic> Left,
                         llvm::ArrayRef<Diagnostic> Right) {
  if (Left.size() != Right.size())
    return false;
  for (size_t I = 0; I < Left.size(); ++I)
    if (Left[I].PC != Right[I].PC || Left[I].Message != Right[I].Message)
      return false;
  return true;
}

bool haveSameInstructions(llvm::ArrayRef<LowInstruction> Left,
                          llvm::ArrayRef<LowInstruction> Right) {
  if (Left.size() != Right.size())
    return false;
  for (size_t I = 0; I < Left.size(); ++I) {
    const LowInstruction &LHS = Left[I];
    const LowInstruction &RHS = Right[I];
    if (LHS.PC != RHS.PC || LHS.NextPC != RHS.NextPC ||
        LHS.Info.Op != RHS.Info.Op || LHS.Info.Name != RHS.Info.Name ||
        LHS.DecodeStatus != RHS.DecodeStatus ||
        LHS.Immediate != RHS.Immediate ||
        LHS.ImmediateStatus != RHS.ImmediateStatus ||
        LHS.StackOperandCount != RHS.StackOperandCount ||
        LHS.StackOperands != RHS.StackOperands || LHS.Encoding != RHS.Encoding)
      return false;
  }
  return true;
}

bool haveSameDecode(const DecodedBytecode &Left, const DecodedBytecode &Right) {
  return Left.Code == Right.Code &&
         Left.JumpDestinations == Right.JumpDestinations &&
         haveSameInstructions(Left.Instructions, Right.Instructions) &&
         haveSameDiagnostics(Left.Diagnostics, Right.Diagnostics);
}

bool isLosslessPartition(llvm::ArrayRef<uint8_t> Code,
                         const DecodedBytecode &Decoded) {
  if (Decoded.Code.size() != Code.size() ||
      !std::equal(Decoded.Code.begin(), Decoded.Code.end(), Code.begin()))
    return false;
  if (Code.empty())
    return Decoded.Instructions.empty() && Decoded.JumpDestinations.empty();
  if (Decoded.Instructions.empty())
    return false;
  uint64_t ExpectedPC = kEntryPC;
  std::set<uint64_t> ExpectedJumpDestinations;
  for (const LowInstruction &Instruction : Decoded.Instructions) {
    if (Instruction.PC != ExpectedPC || Instruction.NextPC <= Instruction.PC ||
        Instruction.NextPC > Code.size() ||
        Instruction.Encoding.size() != Instruction.NextPC - Instruction.PC ||
        !std::equal(Instruction.Encoding.begin(), Instruction.Encoding.end(),
                    Code.begin() + Instruction.PC))
      return false;
    if (Instruction.is(Opcode::JUMPDEST))
      ExpectedJumpDestinations.insert(Instruction.PC);
    ExpectedPC = Instruction.NextPC;
  }
  return ExpectedPC == Code.size() &&
         ExpectedJumpDestinations == Decoded.JumpDestinations;
}

bool changesDecoderContract(Hardfork Previous, Hardfork Current) {
  for (unsigned Byte = 0; Byte <= kByteMax; ++Byte) {
    const auto Left = opcodeInfo(static_cast<uint8_t>(Byte), Previous);
    const auto Right = opcodeInfo(static_cast<uint8_t>(Byte), Current);
    if (Left.has_value() != Right.has_value())
      return true;
    if (!Left)
      continue;
    if (Left->Op != Right->Op || Left->Name != Right->Name ||
        Left->ImmediateBytes != Right->ImmediateBytes ||
        Left->Immediate != Right->Immediate)
      return true;
  }
  return false;
}

std::vector<Hardfork> decoderChangingHardforks() {
  std::vector<Hardfork> Result{AllHardforks.front()};
  for (size_t I = 1; I < AllHardforks.size(); ++I)
    if (changesDecoderContract(AllHardforks[I - 1], AllHardforks[I]))
      Result.push_back(AllHardforks[I]);
  return Result;
}

TEST(EVMDecoderProperty,
     ExhaustsEveryTwoByteInputAtEveryDecoderChangingHardfork) {
  constexpr unsigned kByteValues = kByteMax + 1;
  for (Hardfork Fork : decoderChangingHardforks()) {
    DecodeOptions Options;
    Options.Fork = Fork;
    Options.Strict = false;
    for (unsigned First = 0; First < kByteValues; ++First) {
      for (unsigned Second = 0; Second < kByteValues; ++Second) {
        const std::array Code = {static_cast<uint8_t>(First),
                                 static_cast<uint8_t>(Second)};
        auto Decoded = decodeBytecode(Code, Options);
        ASSERT_TRUE(static_cast<bool>(Decoded))
            << hardforkName(Fork).str() << " first=" << First
            << " second=" << Second << " "
            << llvm::toString(Decoded.takeError());
        if (!isLosslessPartition(Code, *Decoded)) {
          ADD_FAILURE() << hardforkName(Fork).str() << " first=" << First
                        << " second=" << Second;
          return;
        }

        auto Repeated = decodeBytecode(Code, Options);
        ASSERT_TRUE(static_cast<bool>(Repeated))
            << llvm::toString(Repeated.takeError());
        if (!haveSameDecode(*Decoded, *Repeated)) {
          ADD_FAILURE() << hardforkName(Fork).str() << " first=" << First
                        << " second=" << Second;
          return;
        }
      }
    }
  }
}

TEST(EVMDecoderProperty, DeterministicHostileInputsTerminateLosslessly) {
  DecodeOptions Options;
  Options.Strict = false;
  const std::vector<Hardfork> Forks = decoderChangingHardforks();
  ASSERT_FALSE(Forks.empty());
  uint32_t State = kPropertySeed;
  for (size_t Case = 0; Case < kRandomCaseCount; ++Case) {
    Options.Fork = Forks[Case % Forks.size()];
    const size_t Size =
        xorshift32(State) % (kMaximumRandomCodeSize + size_t{1});
    std::vector<uint8_t> Code(Size);
    for (uint8_t &Byte : Code)
      Byte = static_cast<uint8_t>(xorshift32(State));

    auto First = decodeBytecode(Code, Options);
    auto Second = decodeBytecode(Code, Options);
    ASSERT_TRUE(static_cast<bool>(First))
        << "case=" << Case << " " << llvm::toString(First.takeError());
    ASSERT_TRUE(static_cast<bool>(Second))
        << "case=" << Case << " " << llvm::toString(Second.takeError());
    if (!isLosslessPartition(Code, *First)) {
      ADD_FAILURE() << "case=" << Case << " size=" << Code.size();
      return;
    }
    if (!haveSameDecode(*First, *Second)) {
      ADD_FAILURE() << "case=" << Case
                    << " fork=" << hardforkName(Options.Fork).str()
                    << " size=" << Code.size();
      return;
    }
  }
}

} // namespace
} // namespace neverd::evm
