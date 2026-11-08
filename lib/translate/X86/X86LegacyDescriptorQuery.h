//===- X86LegacyDescriptorQuery.h - Protected table queries ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read-only selector visibility and descriptor values over owned paged RAM.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYDESCRIPTORQUERY_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYDESCRIPTORQUERY_H

#include "neverd/translate/X86/X86LegacySegmentExecution.h"

#include "llvm/Support/Error.h"

#include <cstdint>

namespace neverd::translate::detail {

enum class X86LegacyDescriptorQuery { AccessRights, Limit, Read, Write };

struct X86LegacyDescriptorQueryResult : X86MemoryAccessResult {
  uint32_t Value = 0;
  bool Visible = false;
};

llvm::Expected<X86LegacyDescriptorQueryResult>
queryLegacyDescriptor(const X86LegacySegmentState &State,
                      X86PagedMemory &Memory, X86LegacyDescriptorQuery Query,
                      uint16_t Selector);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYDESCRIPTORQUERY_H
