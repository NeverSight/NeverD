//===- RuntimeState.cpp - Select the captured OS's materializer
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "RuntimeState.h"

#include "../os/windows/RuntimeState.h"

#include "neverd/emulation/ProcessRuntimeState.h"

namespace neverd::unpack {
llvm::Expected<RebuiltImage> restoreRuntime(const InputImage &Input,
                                            const Capture &Observed,
                                            RebuiltImage Rebuilt) {
  if (!Observed.OwnedState)
    return failure("the process profile supplies no owned runtime state");
  switch (Observed.OwnedState->Profile) {
  case emulation::ProcessRuntimeState::Kind::WindowsPE64:
    return windows::restoreRuntime(Input, Observed, std::move(Rebuilt));
  case emulation::ProcessRuntimeState::Kind::WindowsDriverX64:
    return failure("retained kernel state has no native runtime materializer");
  }
  return failure("unsupported runtime-state profile");
}
} // namespace neverd::unpack
