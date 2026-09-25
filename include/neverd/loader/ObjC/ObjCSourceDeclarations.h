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
/// source type. RequiredType, when present, comes from an authenticated first
/// consumer and further requires that complete source kind. Missing
/// declarations, incompatible carriers, and multiple compatible signatures
/// remain unresolved.
std::optional<SourceFunctionTypeHint> objcSelectorSourceTypeHintForResultUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceABIValueLocation &RequiredResult,
    std::optional<NdTypeKind> RequiredType = std::nullopt);

/// Whether a complete declared source type can identify one message argument
/// while it is transported unchanged through an integer pointer carrier.
bool isObjCSelectorArgumentEvidenceType(const TypeRef &Type,
                                        bool ConsumedAsObject);

/// Resolve a selector conflict when a caller's exact declared pointer
/// parameter reaches one message argument unchanged. MethodEntry and Source
/// are revalidated against all runtime method records sharing the entry; the
/// complete source type must select exactly one declaration at that position.
std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHintForArgumentTypeUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorArgumentTypeEvidence &Evidence);

/// Reconstruct the source signature carried by an exact Objective-C method
/// forwarder without consulting declarations for the forwarded selector.
std::optional<SourceFunctionTypeHint> objcMethodForwardingSourceTypeHint(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorForwardingEvidence &Evidence);

/// Revalidate a reconstructed forwarding signature against every declaration
/// for the dynamic selector. Missing declarations are allowed; incomplete or
/// conflicting declarations veto the forwarding contract.
std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHintForForwardingUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorForwardingEvidence &Evidence);

/// Resolve a selector conflict when one argument is the exact address of
/// private frame storage. This proves pointer-to-pointer shape only; a unique
/// complete declaration must supply the pointee type and every other ABI fact.
std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHintForArgumentStorageUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorArgumentStorageEvidence &Evidence);

/// Resolve the compiler-declared object class written through one Objective-C
/// object-pointer parameter. The selector-wide ABI and every active SDK owner
/// must agree; runtime declarations without pointee class metadata veto it.
std::optional<std::string>
objcSelectorOutParameterClass(const BinaryImage &Image,
                              llvm::StringRef Selector, unsigned Parameter);

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
  /// Declared single protocol returned by the agreed pointer-valued method.
  /// This names a protocol contract, not a concrete dynamic class.
  std::optional<std::string> ReturnProtocol;
};
ObjCReceiverDeclaration
objcReceiverSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                           const ObjCReceiverTypeHint &Receiver);

/// Resolve a receiver-qualified declaration using the ordinary fixed Darwin
/// ABI when the exact receiver is the current method's non-null self. This is
/// only declaration evidence: callers must separately prove and publish the
/// hidden indirect-result storage at the call site.
ObjCReceiverDeclaration
objcNonNilSelfSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                             const ObjCReceiverTypeHint &Receiver);

/// Resolve an instance super dispatch from the exact current-class reference
/// stored in struct objc_super. The referenced local class must have a complete
/// superclass edge; declarations are searched from that superclass only, so
/// overrides on the current class or its subclasses do not participate.
ObjCReceiverDeclaration
objcSuperSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                        const ObjCReceiverTypeHint &CurrentClass);

std::optional<ObjCReceiverTypeHint>
objcMethodReceiverTypeHint(const BinaryImage &Image, va_t Entry);
/// Recover an exact embedded source parameter's declared object class when
/// Objective-C metadata erases it to id. The parameter remains dynamically
/// dispatched; this is only receiver type provenance.
std::optional<ObjCReceiverTypeHint>
objcMethodParameterReceiverTypeHint(const BinaryImage &Image, va_t Entry,
                                    unsigned Parameter);
/// Re-read an exact block descriptor and its encoded object parameter. The
/// caller must separately prove descriptor-to-invoke association.
std::optional<ObjCReceiverTypeHint>
objcBlockParameterReceiverTypeHint(const BinaryImage &Image, va_t Invoke,
                                   va_t Descriptor, uint32_t Flags,
                                   unsigned Parameter);
std::optional<SourceFunctionTypeHint>
objcMethodSourceTypeHint(const BinaryImage &Image, va_t Entry);
bool objcReceiverTypeHintValid(const BinaryImage &Image,
                               const ObjCReceiverTypeHint &Receiver);
/// Resolve an authenticated instance receiver's declared class after all
/// field and message-result type steps. Protocol-only and class receivers do
/// not establish a concrete class contract.
std::optional<std::string>
objcReceiverInstanceClassName(const BinaryImage &Image,
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

struct ObjCBlockParameterContract {
  enum class Lifetime { NonEscaping, Copied };
  SourceFunctionTypeHint Signature;
  Lifetime Storage;
};

/// Return an audited SDK block ABI and lifetime only when the message binding
/// matches the parent method, parameter, receiver hierarchy, and callback.
/// Copied contracts additionally require a qualified receiver. Dynamic
/// dispatch is preserved; this proves only the caller-side lifetime contract.
std::optional<ObjCBlockParameterContract>
objcBlockParameterContract(const BinaryImage &Image,
                           const SourceCallTypeHint &Call, unsigned Parameter);

/// Return only compiler-declared nonescaping block contracts.
std::optional<SourceFunctionTypeHint>
objcNonEscapingBlockSignature(const BinaryImage &Image,
                              const SourceCallTypeHint &Call,
                              unsigned Parameter);

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
