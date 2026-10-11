//===- Correlation.cpp - Auditable source and HAR candidate joins ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded equality of private absolute HTTP origin/path and fixed method.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/Interfaces.h"

#include <tuple>

namespace neverd::web {

InterfaceCorrelation correlateInterfaces(const SourceInterfaces &S,
                                         const HARCapture &H) {
  if (S.Status != "partial")
    throw Error("source_interfaces_unavailable");
  if (S.Records.size() > MaxInterfaceRecords ||
      H.Observations.size() > MaxInterfaceRecords)
    throw Error("interface_correlation_budget_exceeded");
  InterfaceCorrelation R;
  R.SourceAnalysisID = S.ID;
  R.CaptureID = H.ID;
  R.ID = identity("interface-correlation",
                  {S.ID, H.ID, InterfaceCorrelationProfile});
  using Key = std::tuple<std::string, std::string, std::string>;
  std::map<Key, std::vector<uint32_t>> Observations;
  uint64_t PrivateBytes = 0;
  auto Check = [&](const InterfaceEndpoint &E) {
    const auto Size = E.Origin.size() + E.Path.size();
    if (Size > 2 * MaxInterfacePrivateBytes - PrivateBytes ||
        E.Origin.size() > MaxInterfaceURLBytes ||
        E.Path.size() > MaxInterfaceURLBytes)
      throw Error("interface_correlation_budget_exceeded");
    PrivateBytes += Size;
  };
  auto Known = [](const std::string &Method) {
    return interfaceMethod(Method) != "other";
  };
  for (uint32_t I = 0; I < H.Observations.size(); ++I) {
    const auto &O = H.Observations[I];
    ++R.Steps;
    Check(O.Endpoint);
    if (!O.RequestPresent || !O.Endpoint.comparable() || !Known(O.Method))
      continue;
    ++R.EligibleObservations;
    Observations[{O.Method, O.Endpoint.Origin, O.Endpoint.Path}].push_back(I);
  }
  for (uint32_t I = 0; I < S.Records.size(); ++I) {
    const auto &C = S.Records[I];
    ++R.Steps;
    Check(C.Endpoint);
    if (C.Kind != "fetch" || !C.Endpoint.comparable() || !Known(C.Method) ||
        C.MethodStatus == "fetch_forbidden_method")
      continue;
    ++R.EligibleSources;
    const auto Found =
        Observations.find({C.Method, C.Endpoint.Origin, C.Endpoint.Path});
    if (Found == Observations.end())
      continue;
    for (const auto O : Found->second) {
      ++R.Steps;
      if (R.Pairs.size() >= MaxInterfacePairs)
        throw Error("interface_correlation_budget_exceeded");
      R.Pairs.push_back(
          {identity("interface-pair", {R.ID, C.ID, H.Observations[O].ID}), I,
           O});
    }
  }
  return R;
}
} // namespace neverd::web
