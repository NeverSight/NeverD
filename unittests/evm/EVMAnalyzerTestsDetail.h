//===- EVMAnalyzerTestsDetail.h - Shared EVM analyzer test fixtures -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Solidity-shaped selector dispatcher the medium-IR and high-IR analyzer
/// tests both build their programs around.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_EVM_EVMANALYZERTESTSDETAIL_H
#define NEVERD_UNITTESTS_EVM_EVMANALYZERTESTSDETAIL_H

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::evm::test {

inline constexpr uint8_t kTestFunctionEntry = 0x0f;
inline constexpr uint32_t kTestSelector = 0x12345678u;

inline std::vector<uint8_t> dispatcherFor(uint32_t Selector,
                                          std::vector<uint8_t> Body) {
  std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(Selector >> 24),
      static_cast<uint8_t>(Selector >> 16),
      static_cast<uint8_t>(Selector >> 8),
      static_cast<uint8_t>(Selector),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kTestFunctionEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
  };
  Code.insert(Code.end(), Body.begin(), Body.end());
  return Code;
}

inline std::vector<uint8_t> selectorDispatcher(std::vector<uint8_t> Body) {
  return dispatcherFor(kTestSelector, std::move(Body));
}

} // namespace neverd::evm::test

#endif // NEVERD_UNITTESTS_EVM_EVMANALYZERTESTSDETAIL_H
