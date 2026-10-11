//===- RuntimeState.h - Profile-selected state materialization ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_DYNAMIC_RUNTIMESTATE_H
#define NEVERD_UNPACK_DYNAMIC_RUNTIMESTATE_H
#include "../core/Capture.h"
#include "../core/Image.h"

namespace neverd::unpack {
/// Restore the captured profile's declared objects, or leave the output
/// unpublished with an explicit unsupported-state diagnostic.
llvm::Expected<RebuiltImage> restoreRuntime(const InputImage &Input,
                                            const Capture &Observed,
                                            RebuiltImage Rebuilt);
} // namespace neverd::unpack
#endif
