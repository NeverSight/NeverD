//===- RuntimePointerContext.h - Source runtime pointer metadata -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared parser/cache admission for source-owned PE load configuration fields.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_RUNTIMEPOINTERCONTEXT_H
#define NEVERD_SIGS_RUNTIMEPOINTERCONTEXT_H

#include "neverd/sigs/Signature.h"

namespace neverd::sigs {
bool isValidRuntimePointerContext(const StoredModule &Module);
} // namespace neverd::sigs

#endif // NEVERD_SIGS_RUNTIMEPOINTERCONTEXT_H
