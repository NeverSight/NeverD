//===- RuntimeState.h - Native Windows state materialization ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_WINDOWS_RUNTIMESTATE_H
#define NEVERD_UNPACK_WINDOWS_RUNTIMESTATE_H
#include "../../dynamic/RuntimeState.h"
namespace neverd::unpack::windows {
llvm::Expected<RebuiltImage> restoreRuntime(const InputImage &Input,
                                            const Capture &Observed,
                                            RebuiltImage Rebuilt);
}
#endif
