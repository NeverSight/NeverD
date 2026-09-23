#ifndef NEVERD_LOADER_OBJC_OBJCENCODING_H
#define NEVERD_LOADER_OBJC_OBJCENCODING_H

#include "neverd/ir/SourceTypeHint.h"

#include "llvm/ADT/StringRef.h"

#include <optional>

namespace neverd {
/// Decode a source scalar or naturally aligned record declaration. ABI
/// classification is separate; opaque and unsupported value layouts fail.
TypeRef parseObjCSourceType(llvm::StringRef Encoding, size_t &Offset,
                            unsigned Depth = 0);

/// Decode one bounded scalar Objective-C encoding at Offset. The @? encoding
/// describes a block object pointer only; its invoke prototype requires an
/// independently validated block descriptor signature. Aggregates passed by
/// value fail closed; well-formed aggregate pointers use an opaque pointee.
TypeRef parseObjCScalarType(llvm::StringRef Encoding, size_t &Offset,
                            unsigned Depth = 0);

/// A complete, explicitly named object class encoding. Bare id, block,
/// protocol-qualified and other pointer encodings do not establish this fact.
std::optional<std::string> objcEncodedObjectClass(llvm::StringRef Encoding);

/// A complete object encoding qualified by exactly one named protocol.
/// Bare id, concrete classes, multiple protocols and class-plus-protocol
/// encodings do not establish this fact.
std::optional<std::string> objcEncodedObjectProtocol(llvm::StringRef Encoding);

/// Decode a complete fixed method declaration, including hidden parameters
/// and selector arity. Runtime encodings do not establish a variadic tail.
/// Physical locations are assigned separately by the authoritative source ABI.
std::optional<SourceFunctionTypeHint>
parseObjCMethodEncoding(llvm::StringRef Selector, llvm::StringRef Encoding);

/// Decode Clang's encoding of a fixed C function declaration. The caller
/// must separately prove that the declaration is neither variadic nor an
/// alternate calling convention; the encoding alone cannot establish those.
std::optional<SourceFunctionTypeHint>
parseObjCFunctionEncoding(llvm::StringRef Encoding);
} // namespace neverd
#endif
