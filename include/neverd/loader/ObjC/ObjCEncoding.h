#ifndef NEVERD_LOADER_OBJC_OBJCENCODING_H
#define NEVERD_LOADER_OBJC_OBJCENCODING_H

#include "neverd/ir/NdTypes.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {
/// Decode one bounded scalar Objective-C encoding at Offset. The @? encoding
/// describes a block object pointer only; its invoke prototype requires an
/// independently validated block descriptor signature. Aggregates fail closed.
TypeRef parseObjCScalarType(llvm::StringRef Encoding, size_t &Offset,
                            unsigned Depth = 0);
} // namespace neverd
#endif
