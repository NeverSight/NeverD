//===- CallStackOffsets.h - Finite incoming stack offsets -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Numeric index covers for the incoming-stack argument summary.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_LOW_CALLSTACKOFFSETS_H
#define NEVERD_LIB_IR_LOW_CALLSTACKOFFSETS_H

#include "neverd/ir/low/CallRegisterBody.h"

#include <cstdint>
#include <map>
#include <utility>

namespace neverd {

/// Signed hulls of complete scalar covers at exact ADD/SUB input occurrences.
/// Empty/missing entries mean unknown. The caller retains stack provenance and
/// checks overflow when composing a range with its entry-relative address.
using CallStackOffsets =
    std::map<std::pair<const LowOp *, unsigned>, std::pair<int64_t, int64_t>>;

/// Query once, lazily, when the ordinary literal-offset proof needs an index.
/// Borrowed operation keys remain valid only while this body is unchanged.
CallStackOffsets finiteCallStackOffsets(CallRegisterBodyView Function);

} // namespace neverd

#endif // NEVERD_LIB_IR_LOW_CALLSTACKOFFSETS_H
