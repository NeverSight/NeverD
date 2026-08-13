//===- COFFRegistrationEHDetail.h - Private x86-32 EH helpers ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation detail of the x86-32 registration-chain recovery.  The
/// bounds, types and helpers here are shared between the
/// `COFFRegistrationEH*.cpp` translation units under `lib/loader/COFF/eh` and
/// are not part of any public interface; nothing outside that directory may
/// include this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_COFF_EH_COFFREGISTRATIONEHDETAIL_H
#define NEVERD_LIB_LOADER_COFF_EH_COFFREGISTRATIONEHDETAIL_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/Compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::coff_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE registration_detail {

/// Bound on any single decoded table.  A scope table or `FuncInfo` map larger
/// than this is a mis-identification, not a program.
inline constexpr uint32_t MaxRegistrationRecords = 4096;

void diagnose(ExceptionFunction &F, ExceptionParseStatus Status,
              const std::string &Message);

bool isExecutableAddress(const BinaryImage &Img, va_t Address);

/// Apply a signed instruction displacement without wrapping past the address
/// space, so a corrupt operand cannot fabricate an in-image target.
std::optional<va_t> addSignedOffset(va_t Base, int64_t Displacement);

template <typename T>
std::optional<T> readScalar(const BinaryImage &Img, va_t Address) {
  const uint8_t *P = Img.readVA(Address, sizeof(T));
  if (!P)
    return std::nullopt;
  return readLE<T>(P);
}

/// Half-open code ranges of every discovered function, sorted by start.  A
/// recovered registration record belongs to whichever function contains its
/// install site; without that attribution the record would have no code range
/// and could not take part in CFG or structuring.
class FunctionRangeMap {
public:
  explicit FunctionRangeMap(const BinaryImage &Img) {
    for (const Symbol &Sym : Img.Symbols) {
      if (!Sym.IsFunc || !isExecutableAddress(Img, Sym.Addr))
        continue;
      Starts.push_back(Sym.Addr);
    }
    std::sort(Starts.begin(), Starts.end());
    Starts.erase(std::unique(Starts.begin(), Starts.end()), Starts.end());
  }

  /// The function containing \p Address, bounded by the next function start
  /// and by the end of the executable segment it lives in.
  std::optional<ExceptionAddressRange> find(const BinaryImage &Img,
                                            va_t Address) const {
    auto It = std::upper_bound(Starts.begin(), Starts.end(), Address);
    if (It == Starts.begin())
      return std::nullopt;
    va_t Begin = *std::prev(It);
    const Segment *Seg = Img.getSegmentFor(Begin);
    if (!Seg || !Seg->isExecutable())
      return std::nullopt;
    uint64_t Usable = std::min<uint64_t>(Seg->Size, Seg->Data.size());
    if (Usable > InvalidVA - Seg->VA)
      return std::nullopt;
    va_t SegEnd = Seg->VA + Usable;
    va_t End = It == Starts.end() ? SegEnd : std::min(*It, SegEnd);
    if (End <= Begin || Address >= End)
      return std::nullopt;
    return ExceptionAddressRange{Begin, End};
  }

private:
  std::vector<va_t> Starts;
};

/// The image's SafeSEH handler table, when the load configuration published
/// one.  It is the authority on which addresses the loader will accept as
/// exception handlers, so a scan hit whose handler is absent from a non-empty
/// table is a false positive rather than an undocumented handler.
class SafeSEHTable {
public:
  explicit SafeSEHTable(const BinaryImage &Img) {
    const va_t ConfigRVA = Img.DynInfo.LoadConfigRVA;
    if (ConfigRVA == 0 || Img.DynInfo.LoadConfigSize < 0x48 ||
        ConfigRVA > InvalidVA - Img.Base)
      return;
    const va_t ConfigVA = Img.Base + ConfigRVA;
    auto TableVA = readScalar<uint32_t>(Img, ConfigVA + 0x40);
    auto Count = readScalar<uint32_t>(Img, ConfigVA + 0x44);
    if (!TableVA || !Count || *TableVA == 0 || *Count == 0 ||
        *Count > MaxRegistrationRecords)
      return;
    for (uint32_t I = 0; I < *Count; ++I) {
      auto Entry = readScalar<uint32_t>(Img, va_t(*TableVA) + uint64_t(I) * 4);
      if (!Entry)
        return;
      if (*Entry > InvalidVA - Img.Base)
        return;
      Handlers.push_back(Img.Base + *Entry);
    }
    std::sort(Handlers.begin(), Handlers.end());
    Present = true;
  }

  bool isPresent() const { return Present; }
  bool contains(va_t Address) const {
    return std::binary_search(Handlers.begin(), Handlers.end(), Address);
  }

private:
  std::vector<va_t> Handlers;
  bool Present = false;
};

/// What a handler address turned out to be once its name and, failing that,
/// its instruction shape were followed.
struct HandlerIdentity {
  ExceptionPersonality Personality = ExceptionPersonality::Unknown;
  std::string Name;
  /// `FuncInfo` address recovered from a `__ehhandler$` thunk.
  va_t CxxFuncInfoVA = 0;
};

bool decodeCxxHandlerThunk(const BinaryImage &Img, va_t HandlerVA,
                           HandlerIdentity &Identity);
bool isExceptHandler4Wrapper(const BinaryImage &Img, va_t HandlerVA);
HandlerIdentity identifyHandler(const BinaryImage &Img, va_t HandlerVA);

/// One proven chain-head read and the operands the surrounding prologue
/// materialized for it.
struct InstallSite {
  va_t InstallVA = 0;
  va_t HandlerVA = 0;
  va_t TableVA = 0;
  std::optional<int32_t> TryLevel;
  HandlerIdentity Identity;
  ExceptionAddressRange Range;
};

std::vector<InstallSite> findInstallSites(const BinaryImage &Img,
                                          const FunctionRangeMap &Functions,
                                          const SafeSEHTable &SafeSEH);
void expandPrologueHelpers(const BinaryImage &Img,
                           const FunctionRangeMap &Functions,
                           std::vector<InstallSite> &Sites);

uint32_t decodeScopeRecords(const BinaryImage &Img, va_t ArrayVA, va_t Limit,
                            bool IsEH4,
                            std::vector<RegistrationScopeRecord> &Scopes);
void recoverTryLevelStores(const BinaryImage &Img,
                           const ExceptionAddressRange &Range, int32_t Seed,
                           size_t ScopeCount, RegistrationChainInfo &Chain);
bool decodeEH4Header(const BinaryImage &Img, va_t TableVA,
                     RegistrationChainInfo &Chain);

bool decodeX86FuncInfo(ExceptionFunction &F, const BinaryImage &Img,
                       va_t FuncInfoVA);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE registration_detail
} // namespace neverd::coff_loader

#endif // NEVERD_LIB_LOADER_COFF_EH_COFFREGISTRATIONEHDETAIL_H
