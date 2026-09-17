#ifndef NEVERD_IR_SOURCECALLTYPEHINT_H
#define NEVERD_IR_SOURCECALLTYPEHINT_H

#include "neverd/Common.h"
#include "neverd/ir/SourceTypeHint.h"

#include <tuple>

namespace neverd {

/// Source receiver provenance carried through full-width machine copies.
/// Method self has a declared base class; an exact class reference denotes
/// that class object. Neither fact selects a dynamic method implementation.
struct ObjCReceiverTypeHint {
  enum class OriginKind { MethodEntry, ClassReference };
  OriginKind Origin = OriginKind::MethodEntry;
  va_t Address = 0;
  /// Root receiver's declared class, before any type steps.
  std::string ClassName;
  bool IsClassMethod = false;
  struct TypeStep {
    enum class Kind { IvarLoad, MessageResult };
    va_t OffsetSlot = 0;
    /// Present only for a literal byte offset in the machine access. A
    /// runtime offset load may follow layout changes; a literal cannot.
    std::optional<uint32_t> ByteOffset;
    uint16_t OffsetWidth = 0;
    Kind TheKind = Kind::IvarLoad;
    std::string Selector;
    bool operator==(const TypeStep &Other) const {
      return std::tie(TheKind, OffsetSlot, ByteOffset, OffsetWidth, Selector) ==
             std::tie(Other.TheKind, Other.OffsetSlot, Other.ByteOffset,
                      Other.OffsetWidth, Other.Selector);
    }
    bool operator<(const TypeStep &Other) const {
      return std::tie(TheKind, OffsetSlot, ByteOffset, OffsetWidth, Selector) <
             std::tie(Other.TheKind, Other.OffsetSlot, Other.ByteOffset,
                      Other.OffsetWidth, Other.Selector);
    }
  };
  /// Ordered, bounded type provenance from the root receiver. A field step
  /// identifies exact storage; a result step identifies an agreed declaration.
  /// Neither supplies object identity or permission to remove operations.
  std::vector<TypeStep> Steps;

  bool operator==(const ObjCReceiverTypeHint &Other) const {
    return Origin == Other.Origin && Address == Other.Address &&
           ClassName == Other.ClassName &&
           IsClassMethod == Other.IsClassMethod && Steps == Other.Steps;
  }
};

/// A source projection binding, never authenticated ABI or safety evidence.
/// It describes the dispatch operation, not a statically selected method IMP.
struct SourceCallTypeHint {
  enum class Kind {
    Native,
    ObjCMessage,
    ObjCSuper2,
    BlockInvoke,
    RuntimeSelector,
    RuntimeClass,
    RuntimeMetaclass,
    RuntimeIvarOffset,
    NativeAddress,
    RuntimeBlockIsa,
    RuntimeBlockDescriptor,
    RuntimeBlockLiteral,
    /// Imported runtime routine with a known scalar ABI. TargetAddress is
    /// the import pointer slot, not a native source definition.
    ObjCRuntimeCall,
    /// A static key consumed only by associated-object runtime operations.
    /// TargetAddress identifies the original key; rebuilt methods share one
    /// opaque storage identity. This does not bind readable image contents.
    RuntimeAssociationKey,
    /// A loader-authenticated self-referential writable pointer slot. The
    /// original value is the slot's own address, so source rebuilds one shared
    /// opaque identity instead of retaining either original image address.
    RuntimeStaticIdentity,
    /// Exact scalar accesses rooted at a uniquely named writable data symbol.
    /// ByteCount is the proven storage prefix rebuilt across methods.
    RuntimeLocalStorageAddress,
    /// Base of a rebuilt numeric profiling-counter section. The SDK proves
    /// storage extents and permits only bounded, unordered memory accesses.
    RuntimeProfileCounterStorage,
    /// A Swift runtime or standard-library import with an explicitly declared
    /// fixed C or Swift ABI. Hidden contexts and register-specialized entries
    /// are excluded.
    SwiftRuntimeCall,
    /// A required Swift value-witness entry reached through exact runtime
    /// metadata. The machine proof binds the operation's canonical table slot
    /// and metadata argument; source emission repeats that lookup instead of
    /// retaining an address from the original image.
    SwiftValueWitness,
    /// A verified Darwin constant-string object with one rebuilt identity.
    RuntimeConstantString,
    /// A fixed Darwin platform C ABI emitted against its public SDK header.
    DarwinRuntimeCall,
    /// The fixed String-to-NSString bridge, emitted with the Swift convention.
    /// TargetAddress is its exact imported slot, not a local Swift function.
    SwiftStringBridge,
    /// Bytes copied for a proven bounded, read-only, nonescaping consumer.
    /// This reproduces contents, not the original pointer's identity.
    RuntimeBorrowedBytes,
    /// The fixed optional-NSString-to-String bridge. Its owned String bits
    /// occupy two return registers; emitted calls retain the Swift convention.
    SwiftStringFromNSString,
    /// Address loaded from an exact Darwin runtime data import. This binds
    /// the platform object's identity; it does not copy or fold its contents.
    DarwinRuntimeGlobalAddress,
    /// A protocol reference slot resolved to a validated local declaration.
    RuntimeProtocol,
    /// A validated immutable Darwin literal object graph with shared identity.
    RuntimeConstantObject,
    /// Immutable scalar table bytes, confined to proven indexed source loads.
    /// Bounds and every helper occurrence must be revalidated in the caller.
    RuntimeReadOnlyBytes,
    /// Complete immutable C-string literal section, rebuilt once per image.
    /// Interior offsets, embedded NULs and retained pointers share its
    /// identity.
    RuntimeCStringStorage
  };
  Kind CallKind = Kind::Native;
  enum class SwiftValueWitnessKind {
    Destroy,
    InitializeWithCopy,
    InitializeBufferWithCopyOfBuffer,
    AssignWithCopy,
    InitializeWithTake,
    AssignWithTake,
    GetEnumTagSinglePayload,
    StoreEnumTagSinglePayload
  };
  /// Present only for a dynamically loaded required Swift value witness.
  std::optional<SwiftValueWitnessKind> ValueWitness;
  /// The bound source routine has a noreturn contract. Runtime bindings must
  /// revalidate this effect against their authoritative catalog.
  bool DoesNotReturn = false;
  /// The imported routine may be absent at runtime. This is preserved only
  /// for an explicitly catalogued weak Darwin import and must be emitted with
  /// weak_import linkage so the reconstructed guard retains its meaning.
  bool WeakImport = false;
  /// The result is exactly this argument's pointer value. This does not
  /// remove call effects or establish memory immutability. Runtime bindings
  /// must revalidate the identity contract against the imported routine.
  std::optional<unsigned> ReturnedArgument;
  /// An imported runtime operation has the result type of these message
  /// sends, starting at one argument. This describes declaration lookup only;
  /// it neither identifies the returned object nor replaces the runtime call.
  struct ObjCResultType {
    unsigned ReceiverArgument = 0;
    std::vector<std::string> Selectors;
    bool operator==(const ObjCResultType &Other) const {
      return ReceiverArgument == Other.ReceiverArgument &&
             Selectors == Other.Selectors;
    }
  };
  std::optional<ObjCResultType> RuntimeObjCResultType;
  SourceFunctionTypeHint Signature;
  va_t TargetAddress = 0;
  std::string TargetName;
  std::string Selector;
  /// For a runtime ivar offset query, the class that declared the ivar.
  std::string OwnerClass;
  /// Nonzero only when a verified selector stub loads this exact runtime slot.
  va_t SelectorReferenceAddress = 0;
  /// Pairs of pointer and byte-count parameter indices. The imported routine
  /// reads at most that nonnegative count, never writes/retains the pointer,
  /// and does not observe its identity. These are call effects, not ABI types.
  std::vector<std::pair<unsigned, unsigned>> BorrowedByteInputs;
  /// Pairs of count/flags and storage parameter indices that carry an opaque
  /// Swift String value. A canonical runtime declaration may use this only to
  /// rebuild proven immortal literal storage; ownership and dynamic values
  /// remain unchanged.
  std::vector<std::pair<unsigned, unsigned>> SwiftStringInputs;
  enum class FormatSyntax { NSString, Predicate };
  /// Proven actual arguments of a declared format call. Signature contains
  /// every supplied value at its physical location; only FixedCount values
  /// belong in the emitted prototype. Revalidate the format object's identity
  /// and compiler-derived declaration before publishing source.
  struct FormatArguments {
    unsigned FixedCount = 0;
    unsigned FormatParameter = 0;
    va_t FormatAddress = 0;
    FormatSyntax Syntax = FormatSyntax::NSString;
  };
  std::optional<FormatArguments> Format;
  /// Restricts declaration agreement using revalidated receiver provenance.
  std::optional<ObjCReceiverTypeHint> Receiver;
  /// RuntimeBorrowedBytes/RuntimeReadOnlyBytes extent at TargetAddress.
  uint32_t ByteCount = 0;
  /// Constant strings/objects: the immutable relocated slot whose loaded
  /// value supplied TargetAddress. For RuntimeCStringStorage, TargetAddress
  /// names the shared pool and this slot identifies a revalidated pointer
  /// into it; its source helper preserves the current interior offset.
  /// Revalidate the complete slot and pool against the current image.
  va_t ImmutablePointerSlot = 0;
};

} // namespace neverd
#endif
