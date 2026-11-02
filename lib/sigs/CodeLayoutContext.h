//===- CodeLayoutContext.h - Source code contribution evidence -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Complete, nonsplittable COFF code layouts and their independent query roots.
/// These observations support candidates, never source identity or new roots.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_CODELAYOUTCONTEXT_H
#define NEVERD_SIGS_CODELAYOUTCONTEXT_H

#include "neverd/sigs/Signature.h"

#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>

namespace neverd::sigs {

bool isValidCodeLayoutContext(const StoredModule &Mod);
uint32_t codeLayoutMemberEnd(const StoredModule &Mod, uint32_t Offset);
/// Public-name indices of strong members with an image-owned import operand.
llvm::SmallVector<size_t, 4> codeLayoutAnchors(const StoredModule &Mod);

} // namespace neverd::sigs

#endif // NEVERD_SIGS_CODELAYOUTCONTEXT_H
