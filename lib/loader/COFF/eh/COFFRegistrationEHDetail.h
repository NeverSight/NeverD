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

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/Compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {
class IndexedPE32CodeBytes;
}

namespace neverd::coff_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE registration_detail {

/// Bound on any single decoded table.  A scope table or `FuncInfo` map larger
/// than this is a mis-identification, not a program.
inline constexpr uint32_t MaxRegistrationRecords =
    limits::kMaxRegistrationEHRecords;

void diagnose(ExceptionFunction &F, ExceptionParseStatus Status,
              const std::string &Message);

bool isExecutableAddress(const BinaryImage &Img, va_t Address);

/// Apply a signed instruction displacement without wrapping past the address
/// space, so a corrupt operand cannot fabricate an in-image target.
std::optional<va_t> addSignedOffset(va_t Base, int64_t Displacement);

/// Disprove a heuristic boundary using decoded callback instructions.
/// A separately checked physical entry call returns to its own fallthrough.
/// After other control transfers, only instruction-interior guesses can go,
/// plus a guess created from an operand whose last byte is trap padding.
/// A bounded absolute jump table is followed like a direct jump: a slot that
/// names the boundary keeps it, and an in-range case can show that a later
/// guess sits inside an instruction. Independent entries, ambiguous decoding
/// and exhausted budgets retain the boundary.
bool disprovesCallbackBoundary(
    const BinaryImage &Img, va_t Entry, va_t Boundary, size_t &Work,
    std::optional<va_t> ReturningEntryCall = std::nullopt);

/// Extend an exact padding guess only through a closed, unambiguous callback
/// graph with a prior ordinary branch and no independent callee in the range.
std::optional<va_t> checkedCallbackRangeEnd(const BinaryImage &Img, va_t Entry,
                                            va_t Boundary, va_t Limit,
                                            size_t &Work);

/// Bound a registration body by all ordinary and table-owned entries. An
/// unresolved or escaping edge leaves its existing conservative range intact.
std::optional<va_t>
checkedRegistrationRangeEnd(const BinaryImage &Img,
                            const ExceptionAddressRange &Range,
                            const std::vector<va_t> &Roots, size_t &Work);

/// Whether the ordinary body at \p Begin ends before \p Boundary on every path.
/// A direct jump out of the range and an `int3` end a path, unlike in a body's
/// extent: only falling into the boundary, an edge that says nothing or an
/// instruction across it keeps the two together.
bool checkedRegistrationBodyClosesBefore(const BinaryImage &Img, va_t Begin,
                                         va_t Boundary, size_t &Work);

/// A word a prologue writes to its frame: `mov dword ptr [ebp + Slot], Value`.
struct RegistrationFrameWord {
  int32_t Slot = 0;
  uint32_t Value = 0;
};

/// The constant words of the frame the instructions from \p Begin to \p End
/// write, when they decode to exactly \p End and all run straight on.
std::optional<std::vector<RegistrationFrameWord>>
checkedRegistrationStraightPrologue(const BinaryImage &Img, va_t Begin,
                                    va_t End, size_t &Work);

/// Prove that an exact instruction belongs to a closed ordinary body rooted
/// at Entry. Every reached instruction must have immutable, unambiguous byte
/// and fixup ownership. This structural witness grants no returning-call ABI.
/// CodeBytes is the caller's scoped index for this same image and work budget.
bool checkedRegistrationBodyOwnsInstruction(const BinaryImage &Img,
                                            const ExceptionAddressRange &Range,
                                            va_t Entry, va_t Instruction,
                                            size_t &Work,
                                            IndexedPE32CodeBytes &CodeBytes);

/// Recover only candidates reached by a decoded direct call, whose preceding
/// padding guess is independently inside a decoded instruction or owned by a
/// closed earlier physical body. Stated entries remain boundaries. Paths start
/// at the image entry or already authenticated physical registration entries.
/// This proves structural boundaries, not a callee ABI or native frame.
std::set<va_t> checkedRegistrationCallBoundaries(
    const BinaryImage &Img, const std::set<va_t> &PhysicalEntries,
    const std::map<va_t, va_t> &Candidates, size_t &Work);

template <typename T>
std::optional<T> readScalar(const BinaryImage &Img, va_t Address) {
  const uint8_t *P = Img.readVA(Address, sizeof(T));
  if (!P)
    return std::nullopt;
  return readLE<T>(P);
}

struct PhysicalRegistrationEntry {
  va_t Address = 0;
  std::optional<va_t> ReturningCall;
};

/// Half-open code ranges of every discovered function, sorted by start.  A
/// recovered registration record belongs to whichever function contains its
/// install site; without that attribution the record would have no code range
/// and could not take part in CFG or structuring.
class FunctionRangeMap {
public:
  explicit FunctionRangeMap(
      const BinaryImage &Img,
      const std::vector<PhysicalRegistrationEntry> &PhysicalEntries = {})
      : Guesses(Img.boundaryGuessFunctionStarts()) {
    // These entries come from complete physical registration prefixes, not
    // from a range that merely contains a chain-head read. They constrain
    // body/callback ownership without promoting an ordinary callable symbol.
    for (const auto &Entry : PhysicalEntries)
      if (isExecutableAddress(Img, Entry.Address)) {
        Starts.push_back(Entry.Address);
        Guesses.erase(Entry.Address);
        if (Entry.ReturningCall)
          ReturningEntryCalls.emplace(Entry.Address, *Entry.ReturningCall);
      }
    // PE export entries and the image entry are format-owned boundaries even
    // when no COFF symbol table or preceding padding made them discoverable.
    for (const Export &Entry : Img.Exports)
      if (isExecutableAddress(Img, Entry.Addr)) {
        Starts.push_back(Entry.Addr);
        Guesses.erase(Entry.Addr);
      }
    if (isExecutableAddress(Img, Img.Entry))
      Starts.push_back(Img.Entry);
    Guesses.erase(Img.Entry);
    for (const Symbol &Sym : Img.Symbols) {
      if (!Sym.IsFunc || !isExecutableAddress(Img, Sym.Addr))
        continue;
      Starts.push_back(Sym.Addr);
    }
    // A PE32 image entry can call initialization before tail-jumping to its
    // actual body. Retain that direct target as an ownership boundary even
    // when a preceding padding guess cannot prove a closed function. This
    // proves neither that the call returns nor a callable/frame contract.
    if (const auto Prefix = readImmutablePE32CodeBytes(Img, Img.Entry, 10, {});
        Prefix && (*Prefix)[0] == 0xe8 && (*Prefix)[5] == 0xe9) {
      const auto Target =
          addSignedOffset(Img.Entry + 10, readLE<int32_t>(Prefix->data() + 6));
      if (Target && readImmutablePE32CodeBytes(Img, *Target, 1, {})) {
        Starts.push_back(*Target);
        Guesses.erase(*Target);
      }
    }
    // An exact PE32 direct entry jump can name a regenerated frame whose
    // prologue lives in another section. Retain its target as a boundary;
    // only the later registration/layout proofs can make it callable.
    const auto OriginalStarts = Starts;
    size_t Work = 0;
    for (va_t Start : OriginalStarts) {
      std::vector<va_t> Seen{Start};
      for (unsigned Depth = 0;
           Depth != ::neverd::limits::loader::kMaxRegistrationNullArgumentDepth;
           ++Depth) {
        if (++Work > limits::kMaxRegistrationEHStateWork ||
            Start > uint64_t(UINT32_MAX) - 4)
          break;
        const auto *Code = Img.readVA(Start, 5);
        if (!Code || Code[0] != 0xe9)
          break;
        const va_t Target = uint32_t(Start + 5 + readLE<uint32_t>(Code + 1));
        if (!isExecutableAddress(Img, Target) ||
            std::find(Seen.begin(), Seen.end(), Target) != Seen.end())
          break;
        Starts.push_back(Target);
        Seen.push_back(Target);
        Start = Target;
      }
    }
    std::sort(Starts.begin(), Starts.end());
    Starts.erase(std::unique(Starts.begin(), Starts.end()), Starts.end());
  }

  /// The function containing \p Address, bounded by the next function start
  /// and by the end of the executable segment it lives in.
  std::optional<ExceptionAddressRange>
  find(const BinaryImage &Img, va_t Address,
       bool AuthenticatedEntry = false) const {
    auto It = std::upper_bound(Starts.begin(), Starts.end(), Address);
    if (It == Starts.begin())
      return std::nullopt;
    va_t Begin = *std::prev(It);
    // A padding guess cannot truncate an authenticated entry's instructions.
    // Exact entries after control transfers, ambiguous decoding and every
    // stated symbol, export and other confirmed boundary remain independent.
    const auto Call = ReturningEntryCalls.find(Address);
    const std::optional<va_t> ReturningCall =
        Call != ReturningEntryCalls.end() ? std::optional<va_t>(Call->second)
                                          : std::nullopt;
    if (AuthenticatedEntry)
      while (It != Starts.end() && Guesses.count(*It) &&
             disprovesCallbackBoundary(Img, Address, *It, CallbackWork,
                                       ReturningCall))
        ++It;
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

  /// The function a proved prologue at \p Begin starts, up to the next start
  /// past
  /// \p Address, which is in it: no symbol or padding need say where it begins.
  std::optional<ExceptionAddressRange>
  findFrom(const BinaryImage &Img, va_t Begin, va_t Address) const {
    const Segment *Seg = Img.getSegmentFor(Begin);
    if (!Seg || !Seg->isExecutable())
      return std::nullopt;
    const uint64_t Usable = std::min<uint64_t>(Seg->Size, Seg->Data.size());
    if (Usable > InvalidVA - Seg->VA)
      return std::nullopt;
    const va_t SegEnd = Seg->VA + Usable;
    const auto It = std::upper_bound(Starts.begin(), Starts.end(), Address);
    const va_t End = It == Starts.end() ? SegEnd : std::min(*It, SegEnd);
    if (End <= Begin || Address >= End)
      return std::nullopt;
    return ExceptionAddressRange{Begin, End};
  }

  /// A language table authenticates this callback entry. A closed instruction
  /// graph can additionally disprove a padding guess within a local branch;
  /// stated entries and the other table-owned callbacks remain boundaries.
  std::optional<ExceptionAddressRange>
  findCallback(const BinaryImage &Img, va_t Address, va_t TableLimit) const {
    auto Range = find(Img, Address, true);
    if (!Range)
      return std::nullopt;
    const auto *Segment = Img.getSegmentFor(Address);
    if (!Segment || !Segment->isExecutable())
      return Range;
    const uint64_t Size =
        std::min<uint64_t>(Segment->Size, Segment->Data.size());
    if (Size > InvalidVA - Segment->VA)
      return Range;
    va_t Limit = std::min(TableLimit, Segment->VA + Size);
    auto It = std::upper_bound(Starts.begin(), Starts.end(), Address);
    while (It != Starts.end() && Guesses.count(*It))
      ++It;
    if (It != Starts.end())
      Limit = std::min(Limit, *It);
    if (Range->End < Limit)
      if (const auto End = checkedCallbackRangeEnd(Img, Address, Range->End,
                                                   Limit, CallbackWork))
        Range->End = *End;
    return Range;
  }

  /// Keep every stated or physical entry strictly between these boundaries.
  bool hasIndependentEntry(va_t Begin, va_t End) const {
    for (auto It = std::upper_bound(Starts.begin(), Starts.end(), Begin);
         It != Starts.end() && *It < End; ++It)
      if (!Guesses.count(*It))
        return true;
    return false;
  }

  bool isBoundaryGuess(va_t Address) const { return Guesses.count(Address); }

private:
  std::vector<va_t> Starts;
  std::set<va_t> Guesses;
  std::map<va_t, va_t> ReturningEntryCalls;
  mutable size_t CallbackWork = 0;
};

/// Recover table-owned callback chunks without claiming intervening cleanup
/// relays or unrelated functions as part of the registration body.
void recoverRegistrationCallbackRanges(ExceptionFunction &F,
                                       const BinaryImage &Img,
                                       const FunctionRangeMap &Functions,
                                       const RegistrationChainInfo &Chain);

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
    if (ConfigVA > UINT32_MAX - 0x47) {
      Invalid = true;
      return;
    }
    auto TableVA = readScalar<uint32_t>(Img, ConfigVA + 0x40);
    auto Count = readScalar<uint32_t>(Img, ConfigVA + 0x44);
    if (!TableVA || !Count) {
      Invalid = true;
      return;
    }
    if (*TableVA == 0 && *Count == 0)
      return;
    Present = true;
    if (*TableVA == 0 || *Count == 0 || *Count > MaxRegistrationRecords ||
        uint64_t(*TableVA) + uint64_t(*Count) * 4 > uint64_t(UINT32_MAX) + 1) {
      Invalid = true;
      return;
    }
    for (uint32_t I = 0; I < *Count; ++I) {
      auto Entry = readScalar<uint32_t>(Img, va_t(*TableVA) + uint64_t(I) * 4);
      if (!Entry || *Entry > InvalidVA - Img.Base ||
          !isExecutableAddress(Img, Img.Base + *Entry) ||
          (!Handlers.empty() && Handlers.back() >= Img.Base + *Entry)) {
        Invalid = true;
        return;
      }
      Handlers.push_back(Img.Base + *Entry);
    }
    TablePointer = std::make_pair(ConfigVA + 0x40, va_t(*TableVA));
  }

  bool isPresent() const { return Present; }
  bool isMalformed() const { return Invalid; }
  std::optional<std::pair<va_t, va_t>> tablePointer() const {
    return TablePointer;
  }
  bool contains(va_t Address) const {
    return !Invalid &&
           std::binary_search(Handlers.begin(), Handlers.end(), Address);
  }

private:
  std::vector<va_t> Handlers;
  std::optional<std::pair<va_t, va_t>> TablePointer;
  bool Present = false;
  bool Invalid = false;
};

/// What a handler address turned out to be once its name and, failing that,
/// its instruction shape were followed.
struct HandlerIdentity {
  ExceptionPersonality Personality = ExceptionPersonality::Unknown;
  std::string Name;
  /// `FuncInfo` address recovered from a `__ehhandler$` thunk.
  va_t CxxFuncInfoVA = 0;
  va_t CxxThunkBodyVA = 0;
  va_t CxxThunkEndVA = 0;
  va_t CxxDispatchVA = 0;
  va_t CxxDispatchIATVA = 0;
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
  bool HasPhysicalEntry = false;
  std::optional<va_t> ReturningEntryCall;
};

/// Observe the complete MSVC entry-aligned C++ installation prefix. Its
/// runtime node has no fixed source-EBP displacement; this supplies an entry
/// boundary only, not registration slots or a native frame protocol.
std::optional<va_t> alignedCxxInstallEntry(const BinaryImage &Img,
                                           const InstallSite &Site);

/// Reuse the full fixed C++ layout proof to recover a cookie-publishing entry
/// inside a guessed owner. A checked allocating probe retains its returning
/// instruction boundary, without an ordinary callee or native frame ABI.
std::optional<PhysicalRegistrationEntry>
directCxxCookieInstallEntry(const BinaryImage &Img, const InstallSite &Site);

/// The current complete installation prefix, before comparing it with a scan
/// observation or a retained frame descriptor. Pointer sources come from the
/// same decoder as the physical layout; neither is inferred from a literal.
struct RegistrationInstallation {
  va_t ChainReadVA = 0;
  va_t HandlerPointerVA = 0;
  va_t ChainInstallVA = 0;
  int32_t RegistrationOffset = 0;
  int32_t TryLevelOffset = 0;
  std::optional<RegistrationRealignedFrame> RealignedFrame;
  std::vector<va_t> AddressOperands;

  bool applyTo(va_t ExpectedChainRead, RegistrationChainInfo &Chain) const {
    if (ChainReadVA != ExpectedChainRead)
      return false;
    Chain.RegistrationOffset = RegistrationOffset;
    Chain.TryLevelOffset = TryLevelOffset;
    Chain.ChainInstallVA = ChainInstallVA;
    Chain.RealignedFrame = RealignedFrame;
    return true;
  }
};
std::optional<RegistrationInstallation>
decodeFixedCxxRegistrationLayout(const BinaryImage &Img,
                                 const InstallSite &Site);

std::optional<RegistrationInstallation>
decodeRealignedCxxRegistrationLayout(const BinaryImage &Img,
                                     const InstallSite &Site);

std::vector<InstallSite> findInstallSites(const BinaryImage &Img,
                                          const FunctionRangeMap &Functions,
                                          const SafeSEHTable &SafeSEH);
void expandPrologueHelpers(const BinaryImage &Img,
                           const FunctionRangeMap &Functions,
                           std::vector<InstallSite> &Sites);
/// Shared C++ prologues receive the per-function handler in EAX. These source
/// observations do not establish frame coordinates or physical call effects.
std::vector<InstallSite> findCxxEAXPrologueCallers(
    const BinaryImage &Img, const FunctionRangeMap &Functions,
    const SafeSEHTable &SafeSEH, const std::set<va_t> &PhysicalEntries);

uint32_t decodeScopeRecords(const BinaryImage &Img, va_t ArrayVA, va_t Limit,
                            bool IsEH4,
                            std::vector<RegistrationScopeRecord> &Scopes,
                            bool &BudgetExhausted);
void recoverTryLevelStores(const BinaryImage &Img,
                           const ExceptionAddressRange &Range, int32_t Seed,
                           size_t ScopeCount, RegistrationChainInfo &Chain);
bool decodeEH4Header(const BinaryImage &Img, va_t TableVA,
                     RegistrationChainInfo &Chain);

bool decodeX86FuncInfo(
    ExceptionFunction &F, const BinaryImage &Img, va_t FuncInfoVA,
    std::map<va_t, va_t> *CallbackSources = nullptr,
    std::vector<ExceptionAddressRange> *RecordRanges = nullptr);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE registration_detail
} // namespace neverd::coff_loader

#endif // NEVERD_LIB_LOADER_COFF_EH_COFFREGISTRATIONEHDETAIL_H
