//===- FlowParameterViews.cpp - Parameter provenance admission ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep low-bit observations exact and reject ambiguous native source ranges.
/// Sorting bounds overlap checks without an all-pairs parameter scan.
//===----------------------------------------------------------------------===//

#include "FlowParameterViews.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <tuple>

namespace neverd::diff {
std::string parameterViewSemantics(const FlowParameterView &V) {
  std::string Out = "i" + std::to_string(V.ViewBits) + ";attrs=" + V.Attributes;
  if (V.NativeSite)
    Out += ";native-register=" + std::to_string(V.NativeSite->Register);
  // MedIndex is the snapshot's MedVar.Id, not an ABI parameter ordinal. Keep
  // it in source provenance and fresh binding, never in correspondence keys.
  return Out;
}

std::vector<uint8_t>
unambiguousParameterSites(llvm::ArrayRef<FlowParameterView> Views,
                          llvm::function_ref<void(uint64_t)> Charge) {
  struct Range {
    uint64_t Begin, End;
    int64_t Index;
    uint32_t Position;
  };
  Charge(Views.size());
  std::vector<uint8_t> Out(Views.size());
  std::vector<Range> Ranges;
  bool Overflow = false;
  for (uint32_t I = 0; I < Views.size(); ++I) {
    Charge(1);
    const auto &V = Views[I];
    if (!V.NativeSite || V.NativeSite->Register == UINT64_MAX)
      continue;
    const auto &S = *V.NativeSite;
    const uint64_t Bytes = (V.SourceBits + 7) / 8;
    if (!Bytes || S.Register > UINT64_MAX - Bytes) {
      Overflow = true;
      continue;
    }
    Out[I] = S.Index >= 0;
    Ranges.push_back({S.Register, S.Register + Bytes, S.Index, I});
  }
  // Malformed ranges cannot establish a disjoint native source inventory.
  if (Overflow) {
    std::fill(Out.begin(), Out.end(), 0);
    return Out;
  }
  llvm::sort(Ranges, [&](const auto &A, const auto &B) {
    Charge(1);
    return std::tie(A.Begin, A.End, A.Position) <
           std::tie(B.Begin, B.End, B.Position);
  });
  uint64_t PreviousEnd = 0;
  for (size_t I = 0; I < Ranges.size(); ++I) {
    Charge(1);
    const auto &R = Ranges[I];
    if (R.Begin < PreviousEnd ||
        (I + 1 < Ranges.size() && Ranges[I + 1].Begin < R.End))
      Out[R.Position] = 0;
    PreviousEnd = std::max(PreviousEnd, R.End);
  }
  llvm::sort(Ranges, [&](const auto &A, const auto &B) {
    Charge(1);
    return std::tie(A.Index, A.Position) < std::tie(B.Index, B.Position);
  });
  for (size_t I = 1; I < Ranges.size(); ++I) {
    Charge(1);
    if (Ranges[I - 1].Index == Ranges[I].Index) {
      Out[Ranges[I - 1].Position] = 0;
      Out[Ranges[I].Position] = 0;
    }
  }
  return Out;
}

std::optional<std::string>
validateParameterViews(const DataFlowGraph &G, const DataFlowOptions &Options,
                       llvm::function_ref<void(uint64_t)> Charge) {
  if (!hasParameterViews(G.Profile))
    return G.ParameterViews.empty()
               ? std::nullopt
               : std::optional<std::string>(
                     "parameter views disagree with the graph profile");
  if (G.retainedItems() > Options.MaxNodes)
    return "dataflow parameters and nodes exceed the retained-item allowance";
  uint64_t OriginalNodes = G.Nodes.size();
  for (const auto &V : G.ParameterViews) {
    Charge(1 + V.Attributes.size());
    if (!V.SourceBits || V.SourceBits > 64 || !V.ViewBits || V.ViewBits > 64 ||
        V.Attributes.size() > limits::diff::kMaxFlowSemanticsBytes ||
        parameterViewSemantics(V).size() > limits::diff::kMaxFlowSemanticsBytes)
      return "invalid parameter type or attribute contract";
    if (V.ElidedTruncations > Options.MaxNodes - OriginalNodes)
      return "elided operations exceed the original node allowance";
    OriginalNodes += V.ElidedTruncations;
    Charge(V.ElidedTruncations);
    if (V.ElidedTruncations
            ? (V.SourceBits != 64 || V.ViewBits != 32 || !V.Attributes.empty())
            : (V.SourceBits != V.ViewBits))
      return "invalid parameter low-bit view or elision count";
  }
  const auto Sites = unambiguousParameterSites(G.ParameterViews, Charge);
  for (uint32_t I = 0; I < G.ParameterViews.size(); ++I) {
    Charge(1);
    if (G.ParameterViews[I].ElidedTruncations && !Sites[I])
      return "projected parameter has no unambiguous native site";
  }
  return {};
}
} // namespace neverd::diff
