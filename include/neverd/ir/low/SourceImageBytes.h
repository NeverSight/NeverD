//===- SourceImageBytes.h - Conditional byte-transform analysis -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe straight-line byte transforms without changing program semantics.

#ifndef NEVERD_IR_LOW_SOURCEIMAGEBYTES_H
#define NEVERD_IR_LOW_SOURCEIMAGEBYTES_H

#include "neverd/ir/SourceImageBytes.h"

#include <cstdint>
#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Propose constant initializer bytes, then evaluate transforms with SymExec
/// through authenticated direct/fallthrough continuations. Shared tails retain
/// each starting block's separate register and memory state; branches, calls
/// and cycles never supply a guessed continuation. Every substituted writable
/// input remains an explicit condition. Conflicting initializers, fresh/unknown
/// inputs and opaque effects do not produce observations. The local phase runs
/// before continuation exploration. An entry phase reuses converged scalar CFG
/// facts for otherwise unknown entry register bytes; all incoming paths must
/// agree, and the exact register conditions remain in the observation.
/// BodyMayWrite, when supplied, borrows CallRegisterEffects' complete original-
/// body summaries. Only untouched GPRs cross calls; ABI-only preservation,
/// save/restore and memory effects supply no inherited facts.
/// A separate display-candidate phase can propose numeric entry conditions
/// across calls. It proves no preservation: every proposed register byte and
/// writable input remains a required condition in the resulting observation.
/// Conditional initializer addresses cannot invalidate exact-address proposals.
/// Traverse the supported dependency graph to completion, without a fixed work,
/// observation-count or text-length cutoff. Explicit visited sets terminate
/// continuation cycles; the scalar owner runs to its finite fixed point.
/// A newly discovered initializer conflict invalidates affected observations.
/// The function's IR is never changed.
SourceImageByteObservations
observeSourceImageBytes(const LowFunc &Function, const BinaryImage &Image,
                        const std::map<va_t, uint32_t> *BodyMayWrite = nullptr);

} // namespace neverd

#endif // NEVERD_IR_LOW_SOURCEIMAGEBYTES_H
