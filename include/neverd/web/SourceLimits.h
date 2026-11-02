//===- SourceLimits.h - Source analysis profile admission ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared source and work limits for explicit analysis profiles.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_SOURCELIMITS_H
#define NEVERD_WEB_SOURCELIMITS_H

#include "neverd/web/Limits.h"
#include "neverd/web/Source.h"

namespace neverd::web {

inline constexpr std::string_view LargeJavaScriptParserProfile =
    "hermes-602befee-large-source-js-v1";
inline constexpr std::string_view JavaScriptRecoveryProfile =
    "hermes-602befee-recovery-js-v2";

struct SourceLimits {
  std::string_view Profile;
  uint64_t Bytes, Nodes, Lexemes, StringUnits;
  uint64_t ModelBytes, WorkMultiplier;
  uint64_t ParserBytes, ParserAllocations, ParserSeconds;
};

inline constexpr SourceLimits InteractiveSourceLimits{JavaScriptParserProfile,
                                                      MaxJavaScriptBytes,
                                                      MaxJavaScriptNodes,
                                                      MaxJavaScriptLexemes,
                                                      MaxJavaScriptStringUnits,
                                                      128 * 1024 * 1024,
                                                      1,
                                                      32 * 1024 * 1024,
                                                      200000,
                                                      5};
inline constexpr SourceLimits LargeSourceLimits{LargeJavaScriptParserProfile,
                                                8 * 1024 * 1024,
                                                1500000,
                                                2000000,
                                                16 * 1024 * 1024,
                                                768 * 1024 * 1024,
                                                16,
                                                256 * 1024 * 1024,
                                                1600000,
                                                30};
/// Sequential readable export; semantic consumers reject recovery models.
inline constexpr SourceLimits RecoverySourceLimits{
    JavaScriptRecoveryProfile,
    limits::web::kRecoveryParserMaxRecoverySourceBytes,
    limits::web::kRecoveryParserMaxRecoveryNodes,
    limits::web::kRecoveryParserMaxRecoveryLexemes,
    limits::web::kRecoveryParserMaxRecoveryStringUnits,
    1024 * 1024 * 1024,
    1,
    256 * 1024 * 1024,
    1600000,
    30};

/// Recovery permits bounded source export only; semantic consumers reject its
/// model. Recovery owners do not change semantic-consumer admission.
SourceLimits sourceLimits(SourceAnalysisMode Mode);
SourceLimits sourceLimits(const SourceAnalysis &Source);
uint64_t sourceWorkLimit(const SourceAnalysis &Source, uint64_t Interactive);

} // namespace neverd::web

#endif // NEVERD_WEB_SOURCELIMITS_H
