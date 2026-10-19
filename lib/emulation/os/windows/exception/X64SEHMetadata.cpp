//===- X64SEHMetadata.cpp - Retained x64 metadata byte ranges
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retained x64 metadata byte ranges.
///

#include "X64SEHMetadata.h"

#include <algorithm>
namespace neverd::emulation {
llvm::ArrayRef<llvm::StringLiteral> x64SEHPersonalityExports() {
  static constexpr llvm::StringLiteral Names[] = {
      "__C_specific_handler", "__GSHandlerCheck", "__GSHandlerCheck_SEH"};
  return Names;
}
llvm::Expected<X64SEHMetadataRanges>
x64SEHMetadataRanges(const ExceptionInfo &Info, uint64_t Base, uint64_t Size,
                     uint64_t Headers, llvm::ArrayRef<PEMetadataRange> Code,
                     uint64_t Limit) {
  auto Invalid = [] {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "invalid or excessive x64 exception metadata ranges");
  };
  if (!Size || Size > UINT64_MAX - Base || !Headers || Headers > Size ||
      Info.Functions.size() > Limit)
    return Invalid();
  X64SEHMetadataRanges Result{{{0, Headers}}, Info.Functions.size()};
  if (Info.DirectorySize)
    Result.Ranges.push_back({Info.DirectoryRVA, Info.DirectorySize});
  for (const auto &F : Info.Functions) {
    const uint64_t Scopes = F.SEH ? F.SEH->Scopes.size() : 0;
    if (Scopes > Limit - Result.Records || Scopes > (UINT64_MAX - 4) / 16)
      return Invalid();
    Result.Records += Scopes;
    if (!F.NativeUnwindBytes.empty())
      Result.Ranges.push_back({F.UnwindInfoRVA, F.NativeUnwindBytes.size()});
    uint64_t HandlerBytes = F.SEH ? 4 + Scopes * 16 : 0;
    if (F.GSCookie) {
      if (F.Cxx && !HandlerBytes)
        HandlerBytes = 4;
      if (F.GSCookie->Payload.size() > UINT64_MAX - HandlerBytes)
        return Invalid();
      HandlerBytes += F.GSCookie->Payload.size();
    }
    if (HandlerBytes) {
      if (F.HandlerDataVA < Base)
        return Invalid();
      Result.Ranges.push_back({F.HandlerDataVA - Base, HandlerBytes});
    }
  }
  for (const auto &R : Code) {
    if (!R.Size || R.RVA > Size || R.Size > Size - R.RVA)
      return Invalid();
    if (std::any_of(Info.Functions.begin(), Info.Functions.end(),
                    [&](const auto &F) {
                      return F.PersonalityVA >= Base &&
                             F.PersonalityVA - Base >= R.RVA &&
                             F.PersonalityVA - Base - R.RVA < R.Size;
                    }))
      Result.Ranges.push_back(R);
  }
  std::sort(Result.Ranges.begin(), Result.Ranges.end(),
            [](const auto &A, const auto &B) { return A.RVA < B.RVA; });
  std::vector<PEMetadataRange> Merged;
  for (const auto &R : Result.Ranges) {
    if (!R.Size || R.RVA > Size || R.Size > Size - R.RVA)
      return Invalid();
    if (!Merged.empty() && R.RVA <= Merged.back().RVA + Merged.back().Size)
      Merged.back().Size =
          std::max(Merged.back().Size, R.RVA + R.Size - Merged.back().RVA);
    else
      Merged.push_back(R);
  }
  Result.Ranges = std::move(Merged);
  return Result;
}
} // namespace neverd::emulation
