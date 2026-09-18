#ifndef NEVERD_LOADER_OBJC_OBJCSOURCEDECLARATIONS_H
#define NEVERD_LOADER_OBJC_OBJCSOURCEDECLARATIONS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include "llvm/ADT/StringRef.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// Merge every known declaration for a dynamically dispatched selector.
/// Runtime declarations and applicable framework declarations must agree;
/// unsupported or variadic declarations veto the source call signature.
/// This supplies source-call ABI facts, never a dynamic receiver identity or
/// permission to rewrite a method implementation.
std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector);

/// Resolve an otherwise conflicting selector only when one complete declared
/// signature uniquely defines every byte of a caller-observed result-register
/// read. RequiredResult describes the exact physical read, not a guessed
/// source type. Missing declarations, incompatible carriers, and multiple
/// compatible signatures remain unresolved.
std::optional<SourceFunctionTypeHint> objcSelectorSourceTypeHintForResultUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceABIValueLocation &RequiredResult);

/// Resolve a selector conflict when a caller's exact declared pointer-to-
/// pointer parameter reaches one message argument unchanged. MethodEntry and
/// Source are revalidated against all runtime method records sharing the
/// entry; bare object pointers and other source types supply no evidence.
std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHintForArgumentTypeUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorArgumentTypeEvidence &Evidence);

/// Unsupported or conflicting declarations veto a narrowed call contract.
/// Missing external hierarchy instead requires selector-wide agreement: it
/// cannot justify excluding other owners. Self includes known subclasses.
struct ObjCReceiverDeclaration {
  bool HasDeclaration = false;
  std::optional<SourceFunctionTypeHint> Signature;
  bool RequiresGlobalAgreement = false;
  /// Declared instance class returned by the agreed pointer-valued method.
  /// Related result types are instantiated using the current receiver class.
  std::optional<std::string> ReturnClass;
};
ObjCReceiverDeclaration
objcReceiverSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                           const ObjCReceiverTypeHint &Receiver);

std::optional<ObjCReceiverTypeHint>
objcMethodReceiverTypeHint(const BinaryImage &Image, va_t Entry);
std::optional<SourceFunctionTypeHint>
objcMethodSourceTypeHint(const BinaryImage &Image, va_t Entry);
bool objcReceiverTypeHintValid(const BinaryImage &Image,
                               const ObjCReceiverTypeHint &Receiver);

/// Extend a receiver proof by loading a declared object field. An exact
/// offset reference or a complete byte offset must identify one field in its
/// recorded class lineage; partial and ambiguous field accesses are rejected.
std::optional<ObjCReceiverTypeHint>
objcReceiverIvarTypeHint(const BinaryImage &Image,
                         const ObjCReceiverTypeHint &Receiver, va_t OffsetSlot);
std::optional<ObjCReceiverTypeHint>
objcReceiverFieldTypeHint(const BinaryImage &Image,
                          const ObjCReceiverTypeHint &Receiver,
                          uint64_t Offset);

/// Extend type provenance using an agreed object-return declaration. Bare id
/// does not add a class fact; dynamic dispatch and ownership effects remain.
std::optional<ObjCReceiverTypeHint>
objcReceiverCallResultTypeHint(const BinaryImage &Image,
                               const ObjCReceiverTypeHint &Receiver,
                               llvm::StringRef Selector);

struct ObjCFormatDeclaration {
  SourceFunctionTypeHint Signature;
  unsigned FormatParameter = 0;
  SourceCallTypeHint::FormatSyntax Syntax =
      SourceCallTypeHint::FormatSyntax::NSString;
};

/// An NSString attribute or documented predicate-language contract, verified
/// against SDK declarations and every runtime/protocol declaration. Signature
/// contains the fixed prefix; callers must prove all actual format arguments.
std::optional<ObjCFormatDeclaration>
objcSelectorFormatDeclaration(const BinaryImage &Image,
                              llvm::StringRef Selector);
} // namespace neverd
#endif
