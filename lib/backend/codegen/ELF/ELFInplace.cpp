//===- ELFInplace.cpp - ELF in-place patching ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ELF in-place binary rewriting implementation.  Replaces function
/// bodies within the existing .text section.  Supports both no-shift
/// (NOP pad) and with-shift (expand .text, fix cross-references) modes.
/// Handles both ELF32 and ELF64 in a unified code path.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/ELF/ELFInplace.h"

#include "neverd/object/ELFLayout.h"
#include "neverd/backend/codegen/ELF/ELFReloc.h"

#include "llvm/BinaryFormat/ELF.h"

#define DEBUG_TYPE "neverd-elf-inplace"

namespace neverd {

std::unique_ptr<RelocResolver> ELFInplaceRewriter::createRelocResolver() const {
  return std::make_unique<ELFRelocResolver>();
}

} // namespace neverd
