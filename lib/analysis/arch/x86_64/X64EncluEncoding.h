//===- X64EncluEncoding.h - Code64 ENCLU prefix rules ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Classify the original instruction shared by software SGX and native
/// transfer.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64ENCLUENCODING_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64ENCLUENCODING_H

#include "X64SegmentEncoding.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::analysis::detail {

enum class X64EncluForm { Transfer, Undefined };

inline std::optional<X64EncluForm>
x64EncluEncoding(llvm::ArrayRef<uint8_t> Bytes) {
  static constexpr uint8_t Opcode[]{0x0f, 0x01, 0xd7};
  X64SegmentPrefixes Prefix;
  size_t Offset = 0;
  bool LockOrRep = false;
  if (!x64SegmentPrefixes(Bytes, Offset, Prefix, &LockOrRep) ||
      Bytes.drop_front(Offset) != llvm::ArrayRef(Opcode))
    return std::nullopt;
  // Intel SDM, ENCLU: address/segment overrides and REX leave the implicit
  // GPR operands at their code-mode width. Operand size, LOCK and REP #UD.
  return LockOrRep || Prefix.Operand16 ? X64EncluForm::Undefined
                                       : X64EncluForm::Transfer;
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64ENCLUENCODING_H
