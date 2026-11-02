//===- BytecodeOperands.cpp - SBF and EVM operand immediates -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Types the immediates of SBF and EVM rows for operand formats.
///
//===----------------------------------------------------------------------===//

#include "BytecodeOperands.h"

#include "SessionImpl.h"

#include "neverd/evm/bytecode/EVMDecoder.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"

#include "llvm/ADT/STLExtras.h"

#include <cstdint>
#include <limits>

namespace neverd::sdk {

std::optional<NativeImmediate>
sbfImmediate(const sbf::LowInstruction &Instruction, unsigned Index) {
  const auto Immediate = sbf::operandImmediate(Instruction, Index);
  if (!Immediate)
    return std::nullopt;
  return NativeImmediate{Index, Immediate->Value, Immediate->Bits};
}

std::optional<NativeImmediate>
evmImmediate(const evm::LowInstruction &Instruction, unsigned Index) {
  const auto Immediate = evm::operandImmediate(Instruction, Index);
  if (!Immediate ||
      Immediate->getBitWidth() > std::numeric_limits<uint64_t>::digits)
    return std::nullopt;
  return NativeImmediate{Index, Immediate->getZExtValue(),
                         Immediate->getBitWidth()};
}

std::optional<NativeImmediate>
bytecodeImmediateAt(Session &S, va_t Addr, unsigned Index, std::string &Error) {
  Error = "Select an actual decoded immediate operand";
  if (!S.ensurePipeline())
    return std::nullopt;
  if (S.Img.Arch == Arch::SBF && S.PipeResult.SBF) {
    const auto &Instructions = S.PipeResult.SBF->Low.Instructions;
    const auto Found = llvm::find_if(Instructions, [&](const auto &Item) {
      return Item.Address == Addr && !Item.IsContinuation;
    });
    if (Found != Instructions.end())
      return sbfImmediate(*Found, Index);
    return std::nullopt;
  }
  if (S.Img.Arch == Arch::EVM && S.PipeResult.EVM) {
    const auto &Instructions = S.PipeResult.EVM->Low.Instructions;
    const auto Found = llvm::find_if(
        Instructions, [&](const auto &Item) { return Item.PC == Addr; });
    if (Found == Instructions.end())
      return std::nullopt;
    if (const auto Immediate = evm::operandImmediate(*Found, Index);
        Immediate &&
        Immediate->getBitWidth() > std::numeric_limits<uint64_t>::digits)
      Error = "An enumerator names at most 64 bits; this PUSH carries " +
              std::to_string(Immediate->getBitWidth());
    return evmImmediate(*Found, Index);
  }
  return std::nullopt;
}

} // namespace neverd::sdk
