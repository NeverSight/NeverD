//===- X86LegacyMpx.h - Protected bounds execution ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt the shared MPX instruction owner to paged legacy RAM and IDT faults.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYMPX_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYMPX_H

#include "neverd/lift/X86/X86MpxEncoding.h"
#include "neverd/translate/X86/X86LegacyExecution.h"

namespace neverd::translate::detail {

llvm::Error validateLegacyMpxContinuation(const X86LegacyExecutionState &State,
                                          const X86PagedMemory &Memory,
                                          llvm::ArrayRef<uint8_t> Bytes);

llvm::Expected<X86MpxContext>
bindLegacyMpx(const X86LegacyExecutionState &State,
              const X86PagedMemory &Memory);

llvm::Error admitLegacyMpxMemory(const X86MpxEncoding &Encoding,
                                 const X86MpxContext &Context,
                                 const X86LegacyExecutionState &State,
                                 const X86PagedMemory &Memory,
                                 const X86LegacyExecutionOptions &Options);

llvm::Expected<X86MemoryAccessResult>
executeLegacyMpx(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                 llvm::ArrayRef<uint8_t> Bytes, X86MpxContext &Context,
                 const X86LegacyExecutionOptions &Options);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYMPX_H
