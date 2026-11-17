//===- MIPS16ReturnAdapter.h - Source FP return helpers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_MIPS_MIPS16RETURNADAPTER_H
#define NEVERD_LIB_PIPELINE_MIPS_MIPS16RETURNADAPTER_H

#include "neverd/ir/low/MIPS/MIPS16ReturnTransport.h"

#include "llvm/ADT/ArrayRef.h"

#include <map>

namespace neverd::pipeline_detail {

/// Prove each distinct direct helper once per source phase. Machine lifting
/// never consumes this inventory; no declaration or symbol name grants it.
std::map<va_t, MIPS16ReturnTransport>
mips16ReturnTransports(const BinaryImage &Image,
                       llvm::ArrayRef<LowFunc> Functions);

/// Expand proven register-only effects in a private source execution view.
/// Retain the original physical LowIR and its call occurrence identities.
std::optional<LowFunc> projectMIPS16ReturnCalls(
    const BinaryImage &Image, const LowFunc &Source,
    const std::map<va_t, MIPS16ReturnTransport> &Transports);

} // namespace neverd::pipeline_detail

#endif
