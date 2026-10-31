//===- ComparisonInitializers.cpp - Initial reference hints ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parallel capture through the same relocation-coordinate reader as CFG
/// roots. Interrupted neighborhoods are discarded as a whole.
///
//===----------------------------------------------------------------------===//

#include "ComparisonInitializers.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/CodePointerSources.h"
#include "neverd/support/Parallel.h"

namespace neverd::sdk {

std::optional<diff::ComparisonState> readComparisonInitializers(
    const BinaryImage &Image, const std::set<va_t> &Functions,
    uint64_t AvailableBytes, const diff::ComparisonControl &Control,
    std::vector<diff::CodePointerInitializer> &Result) {
  using Initializer = diff::CodePointerInitializer;
  if (Image.CodePtrRelocSlots.empty())
    return {};
  if (Control.Progress)
    Control.Progress("initializers", 0, Image.CodePtrRelocSlots.size());
  if (const auto State = Control.stop())
    return State;
  // Preflight retained facts and both temporary indexed arrays. The caller's
  // code capture is complete; no later input allocation spends this budget.
  constexpr uint64_t Charge =
      sizeof(Initializer) + sizeof(std::optional<Initializer>) + sizeof(va_t);
  if (Image.CodePtrRelocSlots.size() > AvailableBytes / Charge)
    return diff::ComparisonState::ResourceLimited;
  const std::vector<va_t> Sources(Image.CodePtrRelocSlots.begin(),
                                  Image.CodePtrRelocSlots.end());
  std::vector<std::optional<Initializer>> Facts(Sources.size());
  parallelForEach(Sources.size(), [&](auto Claim, size_t Count) {
    for (size_t I = Claim(); I < Count; I = Claim()) {
      if (Control.stop())
        return;
      if (const auto Target = normalizedRelocatedCodeTarget(Image, Sources[I]);
          Target && Functions.contains(*Target))
        Facts[I] = Initializer{Sources[I], *Target};
    }
  });
  if (const auto State = Control.stop())
    return State;
  Result.reserve(Facts.size());
  for (const auto &Fact : Facts) {
    if (const auto State = Control.stop()) {
      Result.clear();
      return State;
    }
    if (Fact)
      Result.push_back(*Fact);
  }
  if (Control.Progress)
    Control.Progress("initializers", Sources.size(), Sources.size());
  if (const auto State = Control.stop()) {
    Result.clear();
    return State;
  }
  return {};
}

} // namespace neverd::sdk
