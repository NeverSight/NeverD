//===- TranslationJITLinkerInternal.h - Internal linker policy -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_TRANSLATIONJITLINKERINTERNAL_H
#define NEVERD_LIB_TRANSLATE_TRANSLATIONJITLINKERINTERNAL_H

#include <cstdint>

namespace neverd::translate::detail {

/// Return true only when a finalized AArch64 Branch26 preserves the exact
/// original B/BL opcode and resolves to the expected sealed target.
bool isSealedAArch64Branch26FixupV1(uint32_t OriginalInstruction,
                                    uint32_t FixedInstruction,
                                    uint64_t FixupAddress,
                                    uint64_t ExpectedTargetAddress);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_TRANSLATIONJITLINKERINTERNAL_H
