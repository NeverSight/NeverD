//===- X87PortableRuntimeCHeader.c - Plain C portable runtime call --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile and invoke the runtime through its plain C declarations.
//===----------------------------------------------------------------------===//

#include "neverd/runtime/X87PortableRuntimeC.h"

uint32_t nvd_x87_c_status_probe(void *Execution, uint64_t *SourceReturn) {
  const uint32_t Status = nvd_x87_v1_step(Execution, 0xe0df, 0x4000, 0);
  if (Status)
    return Status;
  *SourceReturn = nvd_x87_v1_result(Execution);
  return 0;
}
