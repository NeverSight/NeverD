//===- EVMDecoder.h - EVM bytecode decoder -------------------*- C++ -*-===//
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

#ifndef NEVERD_EVM_BYTECODE_EVMDECODER_H
#define NEVERD_EVM_BYTECODE_EVMDECODER_H

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

/// Controls opcode activation, downstream reachable-code validation, and
/// resources. Linear decoding is always lossless: Strict is carried into
/// LowIR so control-flow analysis can reject a reachable inactive or unknown
/// byte without mistaking unreachable legacy data for executed code.
struct DecodeOptions {
  Hardfork Fork = Hardfork::Latest;
  bool Strict = true;
  size_t MaxCodeSize = kMaxCodeSize;
#define EVM_ANALYSIS_LIMIT_DECODE(NAME, DEFAULT_VALUE)                         \
  size_t NAME = kDefault##NAME;
#define EVM_ANALYSIS_LIMIT_CONTROL_FLOW(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_MEDIUM_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_HIGH_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT(STAGE, NAME, DEFAULT_VALUE)                         \
  EVM_ANALYSIS_LIMIT_##STAGE(NAME, DEFAULT_VALUE)
#include "neverd/evm/analysis/EVMAnalysisLimits.def"
#undef EVM_ANALYSIS_LIMIT_DECODE
#undef EVM_ANALYSIS_LIMIT_CONTROL_FLOW
#undef EVM_ANALYSIS_LIMIT_MEDIUM_IR
#undef EVM_ANALYSIS_LIMIT_HIGH_IR
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

/// Decodes exactly one instruction at \p PC under \p Fork.
///
/// This is the single place the width of an instruction is decided. Where an
/// immediate ends is a question the fork answers — PUSH0 is a plain byte before
/// Shanghai, and a conditional immediate is only consumed when it decodes — so
/// any second walk over the same bytes that answers it independently will
/// eventually disagree with this one about where the next instruction starts.
///
/// \p PC must be inside \p Code. The returned NextPC always advances, so a walk
/// driven by this terminates. \p Diagnostics may be null when the caller has
/// nowhere to report a malformed immediate.
LowInstruction decodeInstructionAt(llvm::ArrayRef<uint8_t> Code, size_t PC,
                                   Hardfork Fork,
                                   std::vector<Diagnostic> *Diagnostics);

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

/// Formats one opcode byte with the protocol's fixed two hexadecimal digits.
std::string formatOpcodeByte(uint8_t Byte);

/// Returns a compact annotation for non-default decode state, or an empty
/// string for an active instruction with a complete (or absent) immediate.
std::string formatDecodeAnnotation(const LowInstruction &Instruction);

} // namespace neverd::evm

#endif // NEVERD_EVM_BYTECODE_EVMDECODER_H
