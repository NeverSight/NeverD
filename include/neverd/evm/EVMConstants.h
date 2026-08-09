//===- EVMConstants.h - Shared EVM constants -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_EVMCONSTANTS_H
#define NEVERD_EVM_EVMCONSTANTS_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>

namespace neverd::evm {

enum class ExitStatus : uint8_t {
#define EVM_EXIT_STATUS(NAME, C_NAME, VALUE) NAME = VALUE,
#include "neverd/evm/EVMExitStatuses.def"
};

[[nodiscard]] constexpr uint8_t exitStatusCode(ExitStatus Status) {
  return static_cast<uint8_t>(Status);
}

inline constexpr unsigned kBitsPerByte = neverd::kBitsPerByte;
inline constexpr unsigned kHexDigitBits = 4;
inline constexpr unsigned kHexDigitsPerByte = kBitsPerByte / kHexDigitBits;
inline constexpr uint8_t kHexDigitMask = (1U << kHexDigitBits) - 1U;
inline constexpr unsigned kByteMax = (1U << kBitsPerByte) - 1U;
inline constexpr unsigned kWordBits = 256;
inline constexpr unsigned kWordBytes = kWordBits / kBitsPerByte;
inline constexpr unsigned kWordMaxByteIndex = kWordBytes - 1;
inline constexpr unsigned kWordMostSignificantBit = kWordBits - 1;
inline constexpr unsigned kWideWordBits = 2 * kWordBits;
inline constexpr unsigned kAddressBytes = 20;
inline constexpr unsigned kSelectorBytes = 4;
inline constexpr unsigned kSelectorBits = kSelectorBytes * kBitsPerByte;
inline constexpr unsigned kSelectorHexDigits =
    kSelectorBytes * kHexDigitsPerByte;
inline constexpr unsigned kMetadataLengthBytes = 2;
inline constexpr std::size_t kOpcodeSpaceSize = 1U << kBitsPerByte;
inline constexpr std::size_t kStackLimit = 1024;
inline constexpr std::size_t kMaxCodeSize = 64U * 1024U * 1024U;
inline constexpr std::size_t kDefaultMaxSteps = 1'000'000;
inline constexpr std::size_t kDefaultMaxMemoryBytes = 64U * 1024U * 1024U;
inline constexpr uint64_t kDefaultGasLimit = 30'000'000;
inline constexpr uint64_t kDefaultChainID = 1;

static_assert(kWordBits % kBitsPerByte == 0);
static_assert(kWideWordBits == 2 * kWordBits);
static_assert(kSelectorBytes <= kWordBytes);
static_assert(kAddressBytes <= kWordBytes);
static_assert(kMetadataLengthBytes <= sizeof(uint64_t));

inline constexpr llvm::StringLiteral kUnknownOpcodeName = "UNKNOWN";
inline constexpr llvm::StringLiteral kUnknownName = "unknown";
inline constexpr llvm::StringLiteral kDefaultExecutionFunctionName =
    "evm_execute";
inline constexpr llvm::StringLiteral kDefaultContractName = "NeverDRecovered";
inline constexpr llvm::StringLiteral kDefaultSolidityPragma =
    ">=0.8.20 <0.9.0";
inline constexpr llvm::StringLiteral kDefaultLLVMModuleName = "neverd_evm";
inline constexpr llvm::StringLiteral kDefaultRecoveredWordType = "uint256";
inline constexpr llvm::StringLiteral kStackPhiValueName = "stack.phi";
inline constexpr llvm::StringLiteral kRecoveredFunctionPrefix = "func_";
inline constexpr llvm::StringLiteral kRecoveredArgumentPrefix = "arg";
inline constexpr llvm::StringLiteral kUnknownStorageName = "storage_unknown";
inline constexpr llvm::StringLiteral kStorageSlotPrefix = "storage_slot_";
inline constexpr llvm::StringLiteral kRecoveredEventPrefix = "RecoveredEvent_";
inline constexpr llvm::StringLiteral kRecoveredErrorPrefix = "RecoveredError_";
inline constexpr llvm::StringLiteral kRecoveredRevertName = "RecoveredRevert";
} // namespace neverd::evm

#endif // NEVERD_EVM_EVMCONSTANTS_H
