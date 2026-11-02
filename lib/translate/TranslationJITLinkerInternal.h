//===- TranslationJITLinkerInternal.h - Internal linker policy -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_TRANSLATIONJITLINKERINTERNAL_H
#define NEVERD_LIB_TRANSLATE_TRANSLATIONJITLINKERINTERNAL_H

#include "llvm/Support/Error.h"

#include <cstdint>

namespace llvm::jitlink {
class LinkGraph;
} // namespace llvm::jitlink

namespace neverd::translate::detail {

/// Retain every block accepted by the pre-prune audit before capturing the
/// sealed graph. Call only after auditTranslationLinkGraphPrePruneV1 succeeds.
llvm::Error markAuditedTranslationBlocksLiveV1(llvm::jitlink::LinkGraph &Graph);

/// Return true only when a finalized AArch64 Branch26 preserves the exact
/// original B/BL opcode and resolves to the expected sealed target.
bool isSealedAArch64Branch26FixupV1(uint32_t OriginalInstruction,
                                    uint32_t FixedInstruction,
                                    uint64_t FixupAddress,
                                    uint64_t ExpectedTargetAddress);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_TRANSLATIONJITLINKERINTERNAL_H
