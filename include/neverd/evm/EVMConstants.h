//===- EVMConstants.h - Shared EVM constants -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines shared EVM protocol widths, resource defaults, generated-backend
/// ABI values, and stable internal names.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_EVMCONSTANTS_H
#define NEVERD_EVM_EVMCONSTANTS_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace neverd::evm {

enum class ExitStatus : uint8_t {
#define EVM_EXIT_STATUS(NAME, C_NAME, VALUE) NAME = (VALUE),
#include "neverd/evm/EVMExitStatuses.def"
};

[[nodiscard]] constexpr uint8_t exitStatusCode(ExitStatus Status) {
  return static_cast<uint8_t>(Status);
}

inline constexpr unsigned kBitsPerByte = neverd::kBitsPerByte;
inline constexpr unsigned kHexRadix = 16;
inline constexpr unsigned kHexDigitBits = 4;
inline constexpr unsigned kHexDigitsPerByte = kBitsPerByte / kHexDigitBits;
inline constexpr unsigned kByteMax = (1U << kBitsPerByte) - 1U;
inline constexpr unsigned kWordBits = 256;
inline constexpr unsigned kWordBytes = kWordBits / kBitsPerByte;
inline constexpr unsigned kWordMaxByteIndex = kWordBytes - 1;
inline constexpr unsigned kWordMostSignificantBit = kWordBits - 1;
inline constexpr unsigned kWideWordBits = 2 * kWordBits;
inline constexpr unsigned kAddressBytes = 20;
inline constexpr unsigned kAddressBits = kAddressBytes * kBitsPerByte;
inline constexpr unsigned kSelectorBytes = 4;
inline constexpr unsigned kSelectorBits = kSelectorBytes * kBitsPerByte;
inline constexpr unsigned kSelectorHexDigits =
    kSelectorBytes * kHexDigitsPerByte;
inline constexpr unsigned kMetadataLengthBytes = 2;
inline constexpr std::size_t kMebibyte = std::size_t{1024} * 1024;
inline constexpr std::size_t kOpcodeSpaceSize = 1U << kBitsPerByte;
inline constexpr std::size_t kStackLimit = 1024;
inline constexpr std::size_t kMaxImmediateStackOperands = 2;
inline constexpr uint8_t kEIP8024SingleForbiddenFirst = 91;
inline constexpr uint8_t kEIP8024SingleForbiddenLast = 127;
inline constexpr uint8_t kEIP8024PairForbiddenFirst = 82;
inline constexpr uint8_t kEIP8024PairForbiddenLast =
    kEIP8024SingleForbiddenLast;
inline constexpr unsigned kEIP8024SingleDecodeBias = 145;
inline constexpr unsigned kEIP8024PairXorMask = 143;
inline constexpr unsigned kEIP8024PairGridBits = kHexDigitBits;
inline constexpr unsigned kEIP8024PairGridMask =
    (1U << kEIP8024PairGridBits) - 1U;
inline constexpr unsigned kEIP8024PairLowerTriangleSum = 29;
inline constexpr uint16_t kEIP8024MinimumSingleDepth = 17;
inline constexpr uint16_t kEIP8024MaximumSingleDepth = 235;
inline constexpr uint16_t kEIP8024MaximumPairDepth =
    kEIP8024PairLowerTriangleSum;
inline constexpr uint16_t kMaximumInstructionStackHeight =
    kEIP8024MaximumSingleDepth + 1;
inline constexpr uint64_t kEntryPC = 0;
inline constexpr uint64_t kCodeAlignment = 1;
inline constexpr std::size_t kMaxCodeSize = 64 * kMebibyte;
inline constexpr std::size_t kDefaultMaxSteps = 1'000'000;
inline constexpr std::size_t kDefaultMaxMemoryBytes = 64 * kMebibyte;
inline constexpr uint64_t kDefaultGasLimit = 30'000'000;
inline constexpr uint64_t kDefaultChainID = 1;
inline constexpr uint64_t kBlockHashHistoryWindow = 256;

static_assert(kWordBits % kBitsPerByte == 0);
static_assert(kByteMax == std::numeric_limits<uint8_t>::max());
static_assert(kOpcodeSpaceSize == static_cast<std::size_t>(kByteMax) + 1);
static_assert(kMaximumInstructionStackHeight <= kStackLimit);
static_assert(kEIP8024PairGridMask == kHexRadix - 1);
static_assert(1U << kHexDigitBits == kHexRadix);
static_assert(kWideWordBits == 2 * kWordBits);
static_assert((kWordBytes & (kWordBytes - 1)) == 0,
              "EVM memory rounding requires a power-of-two word size");
static_assert(kSelectorBytes <= kWordBytes);
static_assert(kAddressBytes <= kWordBytes);
static_assert(kAddressBits <= kWordBits);
static_assert(kMetadataLengthBytes <= sizeof(uint64_t));
static_assert(kStackLimit <= std::numeric_limits<uint32_t>::max());
static_assert(kBlockHashHistoryWindow > 0);
static_assert(kCodeAlignment > 0);

inline constexpr llvm::StringLiteral kUnknownOpcodeName = "UNKNOWN";
inline constexpr llvm::StringLiteral kUnknownName = "unknown";
inline constexpr llvm::StringLiteral kDefaultExecutionFunctionName =
    "evm_execute";
inline constexpr llvm::StringLiteral kDefaultContractName = "NeverDRecovered";
inline constexpr llvm::StringLiteral kDefaultSolidityPragma = ">=0.8.20 <0.9.0";
inline constexpr llvm::StringLiteral kDefaultLLVMModuleName = "neverd_evm";
inline constexpr llvm::StringLiteral kHostFunctionName = "neverd_evm_host_op";
inline constexpr llvm::StringLiteral kTraceFunctionName = "neverd_evm_trace";
inline constexpr llvm::StringLiteral kDefaultRecoveredWordType = "uint256";
inline constexpr llvm::StringLiteral kStackPhiValueName = "stack.phi";
inline constexpr llvm::StringLiteral kRecoveredFunctionPrefix = "func_";
inline constexpr llvm::StringLiteral kRecoveredArgumentPrefix = "arg";
inline constexpr llvm::StringLiteral kRecoveredDeclarationPrefix = "recovered_";
inline constexpr llvm::StringLiteral kUnknownStorageName = "storage_unknown";
inline constexpr llvm::StringLiteral kStorageSlotPrefix = "storage_slot_";
inline constexpr llvm::StringLiteral kRecoveredEventPrefix = "RecoveredEvent_";
inline constexpr llvm::StringLiteral kRecoveredErrorPrefix = "RecoveredError_";
inline constexpr llvm::StringLiteral kRecoveredRevertName = "RecoveredRevert";
} // namespace neverd::evm

#endif // NEVERD_EVM_EVMCONSTANTS_H
