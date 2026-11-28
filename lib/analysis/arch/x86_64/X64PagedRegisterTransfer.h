//===- X64PagedRegisterTransfer.h - Offline return checks -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify the register-only SYSRETQ epilogue before publishing staged state.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64PAGEDREGISTERTRANSFER_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64PAGEDREGISTERTRANSFER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MathExtras.h"

#include <cstdint>

namespace neverd::analysis::detail {
enum class X64PagedRegisterTransfer { Other, SwapGS, SysretQ };

inline X64PagedRegisterTransfer
x64PagedRegisterTransfer(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() == 3) {
    if (Bytes[0] == 0x48 && Bytes[1] == 0x0f && Bytes[2] == 0x07)
      return X64PagedRegisterTransfer::SysretQ;
    if (Bytes[0] == 0x0f && Bytes[1] == 0x01 && Bytes[2] == 0xf8)
      return X64PagedRegisterTransfer::SwapGS;
  }
  return X64PagedRegisterTransfer::Other;
}

enum class X64PagedRegisterResult {
  Complete,
  UndefinedOpcode,
  GeneralProtection,
  Native
};

/// Called only after original fetch in the admitted 64-bit, non-CET domain.
/// A noncanonical SYSRET target has processor-specific fault context. Leave
/// the instruction unexecuted when that context has no physical binding.
constexpr X64PagedRegisterResult
x64PagedRegisterResult(X64PagedRegisterTransfer Kind, uint64_t EFER,
                       unsigned CPL, uint64_t RCX, unsigned AddressBits) {
  if (Kind == X64PagedRegisterTransfer::Other)
    return X64PagedRegisterResult::Native;
  if (Kind == X64PagedRegisterTransfer::SysretQ && !(EFER & 1))
    return X64PagedRegisterResult::UndefinedOpcode;
  if (CPL)
    return X64PagedRegisterResult::GeneralProtection;
  if (Kind == X64PagedRegisterTransfer::SysretQ &&
      !llvm::isIntN(AddressBits, static_cast<int64_t>(RCX)))
    return X64PagedRegisterResult::Native;
  return X64PagedRegisterResult::Complete;
}
} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64PAGEDREGISTERTRANSFER_H
