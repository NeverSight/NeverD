//===- ComparisonScalarValidation.h - Audited native scalar lift -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Fresh per-function machine-ABI lifting for a revision-bound scalar query.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_COMPARISONSCALARVALIDATION_H
#define NEVERD_SDK_CAPI_COMPARISONSCALARVALIDATION_H

#include "neverd/diff/ScalarValidation.h"

namespace neverd::sdk {
struct Session;
struct ScalarLiftSummary {
  uint64_t Decoded = 0, Lifted = 0;
  bool Verified = false;
  std::string SHA256;
};
struct ComparisonScalarValidation {
  diff::ScalarValidationResult Result;
  ScalarLiftSummary Primary, Secondary;
};
ComparisonScalarValidation
validateComparisonScalars(Session &Primary, const diff::Program &A, uint32_t AI,
                          Session &Secondary, const diff::Program &B,
                          uint32_t BI,
                          const diff::ScalarValidationOptions &Options,
                          const diff::ComparisonControl &Control);
} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_COMPARISONSCALARVALIDATION_H
