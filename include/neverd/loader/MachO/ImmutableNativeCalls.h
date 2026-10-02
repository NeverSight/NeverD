#ifndef NEVERD_LOADER_MACHO_IMMUTABLENATIVECALLS_H
#define NEVERD_LOADER_MACHO_IMMUTABLENATIVECALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

struct HighExpr;
using ImmutableNativeCallTarget =
    SourceCallTypeHint::ImmutableNativeCallEvidence;

/// Prove original ARM64 BLR targets loaded from exact immutable chained code
/// pointer slots. The bounded same-block trace validates the instructions that
/// construct the address and the full-width load. Crossing a direct call needs
/// an authenticated runtime declaration or an explicit native callee ABI and
/// a preserved register. Frame reloads and unknown calls remain unsupported.
/// This is target identity only; it supplies no ABI or source publication gate.
std::map<va_t, ImmutableNativeCallTarget> immutableNativeCallTargets(
    const BinaryImage &Image, const LowFunc &Function,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees = nullptr);
/// Attach only a complete current NativeAnalysis scalar ABI to the same
/// target proof. No callee ABI is inferred from a symbol or pointer slot.
std::map<va_t, SourceCallTypeHint> buildImmutableNativeCallHints(
    const BinaryImage &Image, const LowFunc &Function,
    const std::map<va_t, SourceFunctionTypeHint> &NativeCallees);

/// Receipt and declaration shape only, never machine authentication.
bool isImmutableNativeCallHint(const SourceCallTypeHint &Hint,
                               va_t FunctionEntry, Arch Architecture);
/// The logical source call has no residual machine target evaluation. The
/// original indirect occurrence remains in LowIR/MedIR and in the receipt.
bool isImmutableNativeSourceCall(const HighExpr &Expression, va_t FunctionEntry,
                                 Arch Architecture);
} // namespace neverd
#endif
