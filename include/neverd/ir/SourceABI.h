#ifndef NEVERD_IR_SOURCEABI_H
#define NEVERD_IR_SOURCEABI_H

#include "neverd/ir/SourceTypeHint.h"

namespace neverd {

/// Assign Darwin's ordinary fixed scalar calling convention. This describes
/// the requested scalar signature; it does not establish that a binary had
/// that declaration. Inferred native hints must retain their observed
/// locations.
bool assignDarwinScalarSourceABI(SourceFunctionTypeHint &Hint,
                                 Arch Architecture, std::string &Diagnostic);

/// Assign Darwin's fixed scalar Objective-C ABI. Integer and FP registers are
/// allocated independently; overflowing values use the entry-SP stack area.
/// This is source projection metadata, never authenticated rewrite evidence.
bool assignDarwinObjCSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                               std::string &Diagnostic);

/// Validate an explicit scalar register/stack description, including Swift
/// source hints whose receiver is in a dedicated register. This validates the
/// description's shape; it does not authenticate its origin or truth.
bool validateSourceABI(const SourceFunctionTypeHint &Hint,
                       std::string &Diagnostic);

} // namespace neverd
#endif
