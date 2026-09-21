#ifndef NEVERD_IR_SOURCETYPEHINT_H
#define NEVERD_IR_SOURCETYPEHINT_H

#include "neverd/ir/NdTypes.h"

namespace neverd {

enum class SourceABICarrierKind : uint8_t {
  None,
  IntegerRegister,
  FloatingRegister,
  Stack,
  /// A hidden pointer to caller-owned result storage, separate from ordinary
  /// parameters. ValueBytes describes the pointer, not the returned record.
  IndirectResultPointer
};

/// Physical location of one scalar source value. Stack offsets are relative
/// to the original entry SP, before any prologue. Register values occupy only
/// the low ValueBytes; upper vector lanes are not implied by a scalar hint.
struct SourceABIValueLocation {
  SourceABICarrierKind Kind = SourceABICarrierKind::None;
  uint64_t RegisterOffset = 0;
  int64_t EntryStackOffset = 0;
  uint16_t ValueBytes = 0;
  // Darwin integer register parameters, and arm64 integer results, narrower
  // than 32 bits are sign/zero-extended to 32 bits by their declared
  // signedness. This is explicit ABI evidence, not an inference from an
  // observed low lane; it does not define the high 32 bits or any stack
  // padding.
  bool ExtendTo32Bits = false;
};

struct SourceParameterTypeHint {
  enum class Role : uint8_t {
    Ordinary,
    /// Caller-owned result storage in Swift's dedicated indirect-result
    /// register. The emitted declaration uses swift_indirect_result.
    SwiftIndirectResult,
    /// Swift self/context in its dedicated register. The emitted declaration
    /// uses swift_context; this is not an ordinary integer argument.
    SwiftContext
  };
  std::string Name;
  TypeRef Type;
  SourceABIValueLocation Location;
  /// Record members in increasing byte order. Location is empty when these
  /// components are present; the source parameter remains one logical value.
  std::vector<SourceABIValueLocation> Components;
  Role TheRole = Role::Ordinary;
};

/// A runtime declaration or observed native machine signature for source
/// projection only. NativeAnalysis does not recover an original C prototype.
/// This is NOT authenticated debug information, proof of ABI correctness,
/// ownership evidence, or permission to rewrite/recompile a binary. Keep it
/// separate from MedReturnValueEvidence and the debug authentication APIs.
/// Objective-C encodings describe the fixed arguments; they do not establish
/// whether the source declaration also had an ellipsis.
struct SourceFunctionTypeHint {
  enum class OriginKind : uint8_t {
    ObjCRuntime,
    SwiftMangled,
    NativeAnalysis,
    BlockRuntime,
    SwiftRuntime,
    DarwinRuntime,
    SwiftStringBridge,
    /// A source call uses a compiler-derived framework declaration in
    /// addition to any declarations present in the binary itself.
    ObjCSDK,
    /// A fixed C ABI from compiler-derived declarations and exact library
    /// export evidence. The loader revalidates the actual import identity.
    DarwinSDK,
    /// A compiler-observed Swift SDK declaration with exact export evidence.
    SwiftSDK
  };
  enum class ConventionKind : uint8_t { C, Swift };
  OriginKind Origin = OriginKind::ObjCRuntime;
  ConventionKind Convention = ConventionKind::C;
  TypeRef ReturnType;
  std::vector<SourceParameterTypeHint> Parameters;
  Arch Architecture = Arch::Unknown;
  bool HasExplicitABI = false;
  SourceABIValueLocation ReturnLocation;
  /// An integer pair or a supported record returned in multiple registers,
  /// ordered by source byte offset. ReturnLocation is empty in this case.
  std::vector<SourceABIValueLocation> ReturnComponents;
};

} // namespace neverd
#endif
