#ifndef NEVERD_IR_SOURCEABI_H
#define NEVERD_IR_SOURCEABI_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <optional>

namespace neverd {

/// C vector spelling for an entire AArch64 q-register passed by value.
/// Keep its 16 bytes distinct from a 128-bit integer's x-register ABI.
inline constexpr char kSourceAArch64Vector128CType[] =
    "uint8_t __attribute__((vector_size(16)))";

/// A genuine C Boolean preserves swiftcc i1, unlike an unsigned byte. Its
/// logical storage is one byte; physical definedness belongs to the carrier.
inline constexpr char kSourceBooleanCType[] = "_Bool";
bool isSourceBooleanType(const TypeRef &Type);

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
/// leaves, or six/sixteen doubles for indirect Darwin arm64 results.
/// A flat three-double/opaque-pointer record retains all four fields; only a
/// separately validated Swift result ABI may transport that mixed shape.
/// A flat unsigned-word/opaque-pointer/Boolean Swift result retains all three
/// fields and excludes the seven trailing padding bytes.
/// Nested records retain their declared layout. Other mixtures fail.
std::vector<SourceAggregateMember> sourceAggregateMembers(const TypeRef &Type);

/// Scalar pieces of a supported logical source value. A 128-bit integer
/// contributes two unsigned machine words; records retain their leaf types.
/// Offsets describe source bytes independently of their physical locations.
std::vector<SourceAggregateMember> sourceABIValueMembers(const TypeRef &Type);

struct SourceABIParameter {
  size_t ParameterIndex = 0;
  uint16_t ByteOffset = 0;
  std::string Name;
  TypeRef Type;
  SourceABIValueLocation Location;
};

/// Whether any logical parameter requires an indirect by-value copy. This
/// checks the representation, not its validity or any storage/effect proof.
bool hasIndirectSourceParameters(const SourceFunctionTypeHint &Hint);

/// Whether a signature has Swift's logical error-slot parameter. Consumers
/// must model its in/out register or independently prove an unchanged output.
bool hasSwiftErrorResult(const SourceFunctionTypeHint &Hint);

/// The validated physical in/out value of a logical Swift error-slot
/// parameter. The location is an output even though the platform ordinarily
/// preserves that register. An absent or invalid slot returns no result.
std::optional<SourceABIParameter>
sourceABIErrorResult(const SourceFunctionTypeHint &Hint);

/// Physical parameters of a validated signature, in source member order.
/// An indirect record contributes one pointer-to-record carrier; the logical
/// record stays in Hint.Parameters and is not a source pointer parameter.
/// An invalid signature returns no bindings, never a partial prefix.
std::vector<SourceABIParameter>
sourceABIParameters(const SourceFunctionTypeHint &Hint);

/// HighIR representation of a validated call result before source binding.
/// Scalars carry unsigned machine bits; void and complete records retain their
/// logical type. This does not define any bytes outside the declared result.
/// Invalid declarations have no representation.
TypeRef sourceABICallResultType(const SourceFunctionTypeHint &Hint);

/// Assign Darwin's ordinary fixed scalar calling convention, including a
/// 128-bit integer result in two registers. AArch64 parameters also admit
/// complete 128-bit integers, including consecutive odd-register pairs and
/// aligned whole-value stack spills.
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
/// ARM64 calls support three-signed-word and six/sixteen-double results through
/// the hidden x8 pointer. Six/sixteen-double ARM64 parameters carry a pointer
/// to a by-value copy, independently of x8. Their storage/effect proof and
/// source entry/call projection remain separate and unsupported.
bool assignDarwinFixedSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                                std::string &Diagnostic);

/// Assign a fixed Darwin swiftcc declaration with integer/pointer scalar
/// arguments and a scalar or one/two-word result. Arguments use the declared
/// narrow width and may continue on the stack after the integer register bank.
/// One declared swift_indirect_result and one swift_context pointer may use
/// their dedicated registers without consuming that bank. A final error-slot
/// pointer after swift_context transports its opaque pointee through x21/R12;
/// the descriptor alone does not authorize call or entry source projection.
/// A flat three-double/opaque-pointer result uses d0..d2+x0 on arm64 and
/// xmm0..xmm2+rax on x86_64. The latter additionally admits up to three
/// ordinary double parameters for this result shape. Mixed record parameters
/// fail. An unsigned-word/opaque-pointer/Boolean result uses x0/x1/bit 0 of x2,
/// or RAX/RDX/bit 0 of RCX. No other bit of its Boolean carrier is defined.
/// Asynchronous contexts remain unsupported.
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

/// The System V x86-64 assignment of a call's promoted scalar arguments, a
/// named prefix of \p FixedCount and an ellipsis: the integer and floating
/// banks continue independently into the unnamed values, as Darwin x86_64's
/// do (assignDarwinVariadicSourceABI).
bool assignSysVX64VariadicSourceABI(SourceFunctionTypeHint &Hint,
                                    unsigned FixedCount,
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
