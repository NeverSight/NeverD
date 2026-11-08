//===- X86LegacySystemSelector.h - LDTR/TR transfers -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Protected system selectors and faultable GDT cache loading.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYSYSTEMSELECTOR_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYSYSTEMSELECTOR_H

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/Support/Error.h"

#include <cstdint>

namespace neverd::translate::detail {

enum class X86LegacySystemSelector { StoreLDT, StoreTR, LoadLDT, LoadTR };

/// The scalar owner checks CPL and reads the selector word first. Cache
/// publication follows the complete table access and, for LTR, busy write.
llvm::Expected<X86MemoryAccessResult>
loadLegacySystemSelector(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                         bool Task, uint16_t Selector);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYSYSTEMSELECTOR_H
