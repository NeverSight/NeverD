//===- COFFInplace.cpp - COFF/PE in-place patching ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// COFF/PE in-place binary rewriting implementation.  Replaces function
/// bodies within the existing .text section.  Supports both no-shift
/// (NOP pad) and with-shift (expand .text, fix cross-references) modes.
/// Handles PE32 and PE32+ via the same code path.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFInplace.h"

#include "neverd/Object/PELayout.h"
#include "neverd/backend/codegen/COFF/COFFReloc.h"

#define DEBUG_TYPE "neverd-coff-inplace"

namespace neverd {

std::unique_ptr<RelocResolver>
COFFInplaceRewriter::createRelocResolver() const {
  return std::make_unique<COFFRelocResolver>();
}

} // namespace neverd
