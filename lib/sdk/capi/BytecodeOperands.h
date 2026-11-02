//===- BytecodeOperands.h - SBF and EVM operand immediates -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The immediates that SBF and EVM disassembly rows spell, typed as the
/// native decoders type theirs, so operand formats name the same constants
/// on every architecture.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SDK_CAPI_BYTECODEOPERANDS_H
#define NEVERD_LIB_SDK_CAPI_BYTECODEOPERANDS_H

#include "neverd/Common.h"
#include "neverd/decode/NativeOperands.h"

#include <optional>
#include <string>

namespace neverd {
namespace evm {
struct LowInstruction;
} // namespace evm
namespace sbf {
struct LowInstruction;
} // namespace sbf

namespace sdk {

struct Session;

/// Operand \p Index of an SBF row as a typed immediate, or none.
std::optional<NativeImmediate>
sbfImmediate(const sbf::LowInstruction &Instruction, unsigned Index);

/// Operand \p Index of an EVM row as a typed immediate, or none; PUSH data
/// wider than 64 bits has no such form.
std::optional<NativeImmediate>
evmImmediate(const evm::LowInstruction &Instruction, unsigned Index);

/// The immediate that operand \p Index of the SBF or EVM instruction at
/// \p Addr spells; \p Error says why there is none.
std::optional<NativeImmediate>
bytecodeImmediateAt(Session &S, va_t Addr, unsigned Index, std::string &Error);

} // namespace sdk
} // namespace neverd

#endif // NEVERD_LIB_SDK_CAPI_BYTECODEOPERANDS_H
