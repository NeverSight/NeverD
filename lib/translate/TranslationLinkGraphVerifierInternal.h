//===- TranslationLinkGraphVerifierInternal.h - Internal graph audit -----===//

#ifndef NEVERD_LIB_TRANSLATE_TRANSLATIONLINKGRAPHVERIFIERINTERNAL_H
#define NEVERD_LIB_TRANSLATE_TRANSLATIONLINKGRAPHVERIFIERINTERNAL_H

#include "neverd/translate/TranslationLinkGraphVerifier.h"

namespace llvm::jitlink {
class LinkGraph;
} // namespace llvm::jitlink

namespace neverd::translate::detail {

/// Audit the exact, caller-owned graph before any JITLink pass mutates it.
/// This is a library-internal entry point so production callers cannot submit
/// an independently forged graph in place of compiler-owned artifact bytes.
llvm::Expected<TranslationLinkGraphAuditV1> auditTranslationLinkGraphPrePruneV1(
    llvm::jitlink::LinkGraph &Graph, llvm::ArrayRef<uint8_t> ObjectBytes,
    const ResolvedHostTarget &ExpectedHostTarget,
    llvm::ArrayRef<TranslationObjectSymbolV1> ExpectedBlockSymbols,
    llvm::ArrayRef<TranslationObjectSymbolV1> SealedRuntimeSymbols,
    llvm::StringRef SealedRuntimeRegistryIdentity);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_TRANSLATIONLINKGRAPHVERIFIERINTERNAL_H
