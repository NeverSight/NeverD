//===- MachOInplace.cpp - Mach-O in-place patching ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Mach-O in-place binary rewriting implementation.  Replaces function
/// bodies in the existing __text section, optionally shifting later code
/// and adjusting all branch/PC-relative references.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOInplace.h"

#include "neverd/ArchSupport.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/backend/codegen/MachO/MachOReloc.h"

#include "llvm/BinaryFormat/MachO.h"

#include <cstring>

#define DEBUG_TYPE "neverd-macho-inplace"

namespace neverd {

std::unique_ptr<RelocResolver>
MachOInplaceRewriter::createRelocResolver() const {
  return std::make_unique<MachORelocResolver>();
}

} // namespace neverd
