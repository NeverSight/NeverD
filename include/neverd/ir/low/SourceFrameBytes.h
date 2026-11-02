//===- SourceFrameBytes.h - Stack byte observations ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Authenticate constant bytes carried by the shared private-frame proof.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_SOURCEFRAMEBYTES_H
#define NEVERD_IR_LOW_SOURCEFRAMEBYTES_H

#include "neverd/ir/SourceFrameBytes.h"

#include <vector>

namespace neverd {

struct BinaryImage;
struct LowFunc;

/// Observe initialized constant prefixes at call sites in source views.
/// Unknown bytes end a prefix. Image reads must be complete, immutable and
/// free of relocations. All queries share one bounded frame-analysis budget.
std::vector<SourceFrameBytes> observeSourceFrameBytes(const LowFunc &Function,
                                                      const BinaryImage &Image);

} // namespace neverd

#endif // NEVERD_IR_LOW_SOURCEFRAMEBYTES_H
