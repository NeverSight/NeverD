#ifndef NEVERD_IR_SOURCETYPEHINT_H
#define NEVERD_IR_SOURCETYPEHINT_H

#include "neverd/ir/NdTypes.h"

namespace neverd {

enum class SourceABICarrierKind : uint8_t {
  None,
  IntegerRegister,
  FloatingRegister,
  Stack
};

/// Physical location of one scalar source value. Stack offsets are relative
/// to the original entry SP, before any prologue. Register values occupy only
/// the low ValueBytes; upper vector lanes are not implied by a scalar hint.
struct SourceABIValueLocation {
  SourceABICarrierKind Kind = SourceABICarrierKind::None;
  uint64_t RegisterOffset = 0;
  int64_t EntryStackOffset = 0;
  uint16_t ValueBytes = 0;
};

struct SourceParameterTypeHint {
  std::string Name;
  TypeRef Type;
  SourceABIValueLocation Location;
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
    BlockRuntime
  };
  OriginKind Origin = OriginKind::ObjCRuntime;
  TypeRef ReturnType;
  std::vector<SourceParameterTypeHint> Parameters;
  Arch Architecture = Arch::Unknown;
  bool HasExplicitABI = false;
  SourceABIValueLocation ReturnLocation;
};

} // namespace neverd
#endif
