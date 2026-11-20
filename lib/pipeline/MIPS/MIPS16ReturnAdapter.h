//===- MIPS16ReturnAdapter.h - Source FP return helpers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_MIPS_MIPS16RETURNADAPTER_H
#define NEVERD_LIB_PIPELINE_MIPS_MIPS16RETURNADAPTER_H

#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/low/MIPS/MIPS16IndirectCallTransport.h"
#include "neverd/ir/low/MIPS/MIPS16ReturnTransport.h"

#include "llvm/ADT/ArrayRef.h"

#include <map>

namespace neverd::pipeline_detail {

struct MIPS16RuntimeTransport {
  std::vector<LowOp> Effects;
  std::optional<std::array<unsigned, 2>> IndirectFloatingPrefix;
  unsigned ResultBytes = 0;
  unsigned ResultElements = 0;
};

using MIPS16CallSites = std::map<SourceCallOccurrenceKey, SourceCallTypeHint>;

/// Prove each distinct direct helper once per source phase. Machine lifting
/// never consumes this inventory; no declaration or symbol name grants it.
std::map<va_t, MIPS16RuntimeTransport>
mips16RuntimeTransports(const BinaryImage &Image,
                        llvm::ArrayRef<LowFunc> Functions);

/// Expand proven register-only effects in a private source execution view.
/// Retain the original physical LowIR and its call occurrence identities.
std::optional<LowFunc> projectMIPS16RuntimeCalls(
    const BinaryImage &Image, const LowFunc &Source,
    const std::map<va_t, MIPS16RuntimeTransport> &Transports,
    const MIPS16CallSites *InputSites = nullptr,
    MIPS16CallSites *ProjectedSites = nullptr);

} // namespace neverd::pipeline_detail

#endif
