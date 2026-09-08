#ifndef NEVERD_IR_SOURCETYPEHINT_H
#define NEVERD_IR_SOURCETYPEHINT_H

#include "neverd/ir/NdTypes.h"

namespace neverd {

struct SourceParameterTypeHint {
  std::string Name;
  TypeRef Type;
};

/// A declaration recovered from image bytes for source projection only.
/// This is NOT authenticated debug information, proof of ABI correctness,
/// ownership evidence, or permission to rewrite/recompile a binary. Keep it
/// separate from MedReturnValueEvidence and the debug authentication APIs.
/// Objective-C encodings describe the fixed arguments; they do not establish
/// whether the source declaration also had an ellipsis.
struct SourceFunctionTypeHint {
  enum class OriginKind : uint8_t { ObjCRuntime };
  OriginKind Origin = OriginKind::ObjCRuntime;
  TypeRef ReturnType;
  std::vector<SourceParameterTypeHint> Parameters;
};

} // namespace neverd
#endif
