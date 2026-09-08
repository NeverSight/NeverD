#ifndef NEVERD_LOADER_SWIFT_SWIFTABI_H
#define NEVERD_LOADER_SWIFT_SWIFTABI_H

#include "neverd/ir/SourceTypeHint.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Swift/SwiftMethods.h"

namespace neverd {
struct SwiftSelfABIProof {
  bool Proven = false;
  bool IsMutating = false;
  std::string SelfConvention;
  std::vector<SourceParameterTypeHint> SelfParameters;
  std::vector<std::string> Evidence;
  std::string Reason;
};

/// Prove a bounded fixed-layout self convention from native entry-value flow.
/// ExplicitHint contains only the already validated explicit scalar arguments.
/// NativeFunctions optionally supplies bodies for bounded direct native tail
/// transfers. This establishes a physical source projection, not original
/// source fidelity.
SwiftSelfABIProof
recoverSwiftSelfABI(const SwiftSourceSignature &Signature,
                    const BinaryImage &Image, const LowFunc &Function,
                    const SourceFunctionTypeHint &ExplicitHint,
                    const std::vector<LowFunc> &NativeFunctions = {});
} // namespace neverd
#endif
