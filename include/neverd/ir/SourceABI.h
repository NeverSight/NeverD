#ifndef NEVERD_IR_SOURCEABI_H
#define NEVERD_IR_SOURCEABI_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <optional>

namespace neverd {

/// Compare supported source types structurally, including fixed C callback
/// signatures. Malformed, cyclic, and excessively deep types never compare
/// equal, even when both references identify the same object.
bool equalSourceTypes(const TypeRef &Left, const TypeRef &Right);

/// Compare complete source ABI descriptions, including every logical type and
/// every declared physical carrier. This compares declarations; it does not
/// authenticate where either declaration came from.
bool equalSourceABIs(const SourceFunctionTypeHint &Left,
                     const SourceFunctionTypeHint &Right);

struct SourceAggregateMember {
  TypeRef Type;
  uint16_t ByteOffset = 0;
};

/// Flatten a validated record: one to four homogeneous floating leaves, or
/// one to two 64-bit integer/pointer leaves, or three signed 64-bit integer
/// leaves. Nested records retain their
/// declared layout; padding, packed fields and mixed register classes fail.
std::vector<SourceAggregateMember> sourceAggregateMembers(const TypeRef &Type);

struct SourceABIParameter {
  size_t ParameterIndex = 0;
  uint16_t ByteOffset = 0;
  std::string Name;
  TypeRef Type;
  SourceABIValueLocation Location;
};

/// Physical parameters of a validated signature, in source member order.
/// An invalid signature returns no bindings, never a partial prefix.
std::vector<SourceABIParameter>
sourceABIParameters(const SourceFunctionTypeHint &Hint);

/// Assign Darwin's ordinary fixed scalar calling convention, including a
/// 128-bit integer result in two registers (parameters remain at most 64 bits).
/// This describes
/// the requested scalar signature; it does not establish that a binary had
/// that declaration. Inferred native hints must retain their observed
/// locations.
bool assignDarwinScalarSourceABI(SourceFunctionTypeHint &Hint,
                                 Arch Architecture, std::string &Diagnostic);

/// Assign Darwin's ordinary fixed calling convention to the declared scalar
/// and supported record values. Only explicit parameters consume carriers;
/// callers supply any language-specific hidden parameters. This describes a
/// signature without authenticating its declaration or authorizing rewriting.
/// ARM64 calls support three-signed-word results through the hidden x8 pointer;
/// entry projection and three-word parameters remain unsupported.
bool assignDarwinFixedSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                                std::string &Diagnostic);

/// Assign a fixed Darwin swiftcc declaration with integer/pointer scalar
/// arguments and a scalar or one/two-word result. Arguments use the declared
/// narrow width and may continue on the stack after the integer register bank.
/// One declared swift_indirect_result and one swift_context pointer may use
/// their dedicated registers without consuming that bank. Error results,
/// asynchronous contexts and floating values are unsupported.
bool assignDarwinSwiftSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                                std::string &Diagnostic);

/// Canonical ABI for a supported required Swift value-witness operation. This
/// describes the stable table entry and its physical carriers; a loader must
/// separately prove the matching lookup through the same metadata argument.
std::optional<SourceCallTypeHint> swiftValueWitnessSourceCallHint(
    Arch Architecture, SourceCallTypeHint::SwiftValueWitnessKind Operation);

/// Stable required-table index for a supported value-witness operation.
std::optional<unsigned>
swiftValueWitnessSlot(SourceCallTypeHint::SwiftValueWitnessKind Operation);

/// Revalidate a complete canonical value-witness binding. Extra names,
/// addresses, effects, receiver facts, and data identities are rejected.
bool isSwiftValueWitnessSourceCallHint(const SourceCallTypeHint &Hint,
                                       Arch Architecture);

/// Assign a call's complete promoted scalar arguments, with a named prefix
/// and an ellipsis. Darwin arm64 puts the unnamed values in eight-byte stack
/// slots; x86_64 continues its independent integer and floating banks.
bool assignDarwinVariadicSourceABI(SourceFunctionTypeHint &Hint,
                                   unsigned FixedCount, Arch Architecture,
                                   std::string &Diagnostic);

/// Assign Darwin's fixed Objective-C ABI. Integer and FP registers are
/// allocated independently; overflowing values use the entry-SP stack area.
/// ARM64 additionally supports naturally laid-out homogeneous floating records.
/// Both architectures support records of one or two 64-bit integers/pointers.
/// Indirect record results require separate nil-dispatch storage modeling and
/// remain unsupported, including ARM64 three-signed-word results.
/// This is source projection metadata, never authenticated rewrite evidence.
bool assignDarwinObjCSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                               std::string &Diagnostic);

/// Validate explicit scalar and record register/stack descriptions, including
/// Swift source hints whose receiver is in a dedicated register. This validates
/// the description's shape; it does not authenticate its origin or truth.
bool validateSourceABI(const SourceFunctionTypeHint &Hint,
                       std::string &Diagnostic);

} // namespace neverd
#endif
