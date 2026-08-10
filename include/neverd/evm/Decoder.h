//===- Decoder.h - EVM bytecode decoder ----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares fork-aware, lossless decoding of the linear EVM instruction
/// stream. Control-flow and stack analysis are deliberately separate stages.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_DECODER_H
#define NEVERD_EVM_DECODER_H

#include "neverd/evm/EVMIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace neverd::evm {

/// Controls opcode activation, unknown/inactive-byte policy, and resources.
/// Strict mode still represents malformed conditional immediates because
/// their non-consumption is required to recover later instruction boundaries.
struct DecodeOptions {
  Hardfork Fork = Hardfork::Latest;
  bool Strict = true;
  size_t MaxCodeSize = kMaxCodeSize;
};

/// Lossless result of linear instruction decoding. An invalid conditional
/// immediate remains attached to its opcode as a fault, while the candidate
/// byte starts the following instruction as required by EIP-8024.
struct DecodedBytecode {
  Hardfork Fork = Hardfork::Latest;
  bool Strict = true;
  std::vector<uint8_t> Code;
  std::vector<LowInstruction> Instructions;
  std::set<uint64_t> JumpDestinations;
  std::vector<Diagnostic> Diagnostics;
};

/// Decodes bytecode without building a CFG or performing stack analysis.
llvm::Expected<DecodedBytecode> decodeBytecode(llvm::ArrayRef<uint8_t> Code,
                                               DecodeOptions Options = {});

/// Returns stable spellings for structured disassembly and diagnostics.
llvm::StringRef opcodeDecodeStatusName(OpcodeDecodeStatus Status);
llvm::StringRef immediateDecodeStatusName(ImmediateDecodeStatus Status);

/// Formats an immediate candidate at its declared width, including leading
/// zeroes. Invalid EIP-8024 candidates are formatted even though they are not
/// part of Encoding. Returns an empty string when no immediate was examined.
std::string formatImmediate(const LowInstruction &Instruction);

/// Returns a compact annotation for non-default decode state, or an empty
/// string for an active instruction with a complete (or absent) immediate.
std::string formatDecodeAnnotation(const LowInstruction &Instruction);

} // namespace neverd::evm

#endif // NEVERD_EVM_DECODER_H
