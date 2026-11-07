//===- COFFRegistrationEHCxxHelper.cpp - Shared PE32 C++ prologues --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate EAX-handler prologue calls without granting a native frame ABI.

#include "../../ReadOnlyBytesIndex.h"
#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace neverd::coff_loader {
namespace {

enum class ExtraFrameStore { None, StackPointer, EncodedCookie };
struct HelperProtocol {
  ExceptionAddressRange Code;
  va_t SecurityCookieVA;
  ExtraFrameStore Extra;
};
using HelperCache = std::map<va_t, std::optional<HelperProtocol>>;

struct PrologueReads {
  const BinaryImage &Image;
  size_t *Work = nullptr;
  size_t Limit = 0;
  std::optional<IndexedPE32CodeBytes> Code;

  explicit PrologueReads(const BinaryImage &Image) : Image(Image) {}
  PrologueReads(const BinaryImage &Image, size_t &Work, size_t Limit)
      : Image(Image), Work(&Work), Limit(Limit) {}

  const uint8_t *read(va_t Address, size_t Size) {
    if (Work)
      for (size_t Cost : {Size, Image.Segments.size()}) {
        if (Cost > Limit - std::min(*Work, Limit)) {
          *Work = Limit;
          return nullptr;
        }
        *Work += Cost;
      }
    return Image.readVA(Address, Size);
  }

  std::optional<std::vector<uint8_t>>
  immutable(va_t Address, uint32_t Size, const std::vector<va_t> &Operands) {
    if (!Work)
      return readImmutablePE32CodeBytes(Image, Address, Size, Operands);
    // Most queried entries are not shared prologues. Raw shape reads already
    // pay their bounded cost; only a candidate needs the full fixup index.
    if (!Code)
      Code.emplace(Image, *Work, Limit);
    return Code->read(Address, Size, Operands);
  }
};

std::optional<HelperProtocol> readCxxEAXPrologue(const BinaryImage &Img,
                                                 va_t Entry,
                                                 HelperCache &Checked,
                                                 PrologueReads &Reads) {
  const auto Existing = Checked.find(Entry);
  if (Existing != Checked.end())
    return Existing->second;
  // The caller's handler in EAX is pushed below its return PC. The helper
  // saves EBP in the allocation word, seeds -1 and publishes EBP-12.
  static constexpr uint8_t Prefix[] = {
      0x50, 0x64, 0xff, 0x35, 0,    0,    0,    0,    0x8d, 0x44, 0x24, 0x0c,
      0x2b, 0x64, 0x24, 0x0c, 0x53, 0x56, 0x57, 0x89, 0x28, 0x8b, 0xe8, 0xa1};
  static constexpr uint8_t Tail[] = {0xff, 0x75, 0xfc, 0xc7, 0x45, 0xfc, 0xff,
                                     0xff, 0xff, 0xff, 0x8d, 0x45, 0xf4, 0x64,
                                     0xa3, 0,    0,    0,    0,    0xc3};
  const auto *Bytes = Reads.read(Entry, 51);
  if (!Bytes || !std::equal(std::begin(Prefix), std::end(Prefix), Bytes))
    return std::nullopt;
  // Cache only actual helper-shaped candidates, not arbitrary call targets.
  auto [Result, New] = Checked.emplace(Entry, std::nullopt);
  (void)New;
  if (Img.Base > UINT32_MAX || !Img.DynInfo.SecurityCookieRVA ||
      Img.DynInfo.SecurityCookieRVA > UINT32_MAX - Img.Base)
    return std::nullopt;
  const va_t Cookie = Img.Base + Img.DynInfo.SecurityCookieRVA;
  if (Cookie > UINT32_MAX - 3 || readLE<uint32_t>(Bytes + 24) != Cookie ||
      !Reads.read(Cookie, 4) || Bytes[28] != 0x33 || Bytes[29] != 0xc5 ||
      Bytes[30] != 0x50)
    return std::nullopt;
  size_t Suffix = 31;
  ExtraFrameStore Extra = ExtraFrameStore::None;
  // The compiler variants additionally save ESP or the encoded GS cookie.
  if (Bytes[Suffix] == 0x89 &&
      (Bytes[Suffix + 1] == 0x65 || Bytes[Suffix + 1] == 0x45) &&
      Bytes[Suffix + 2] == 0xf0) {
    Extra = Bytes[Suffix + 1] == 0x65 ? ExtraFrameStore::StackPointer
                                      : ExtraFrameStore::EncodedCookie;
    Suffix += 3;
  }
  Bytes = Reads.read(Entry, Suffix + sizeof(Tail));
  if (!Bytes || !std::equal(std::begin(Tail), std::end(Tail), Bytes + Suffix) ||
      Entry > UINT32_MAX - (Suffix + sizeof(Tail) - 1) ||
      !Reads.immutable(Entry, Suffix + sizeof(Tail), {Entry + 24}))
    return std::nullopt;
  Result->second =
      HelperProtocol{{Entry, Entry + Suffix + sizeof(Tail)}, Cookie, Extra};
  return Result->second;
}

std::optional<X86CxxPrologueCall> readCxxEAXPrologueCall(const BinaryImage &Img,
                                                         va_t Entry,
                                                         HelperCache &Checked,
                                                         PrologueReads &Reads) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || Entry > UINT32_MAX - 14)
    return std::nullopt;
  const auto *Bytes = Reads.read(Entry, 12);
  if (!Bytes || (Bytes[0] != 0x6a && Bytes[0] != 0x68))
    return std::nullopt;
  const uint32_t PushSize = Bytes[0] == 0x6a ? 2 : 5;
  const uint32_t Size = PushSize + 10;
  Bytes = Reads.read(Entry, Size);
  if (!Bytes || Bytes[PushSize] != 0xb8 || Bytes[PushSize + 5] != 0xe8)
    return std::nullopt;
  const int32_t Allocation =
      PushSize == 2 ? int8_t(Bytes[1]) : readLE<int32_t>(Bytes + 1);
  if (Allocation < 0 || Allocation > limits::kMaxSourceCallBorrowedBytes ||
      !Reads.immutable(Entry, Size, {Entry + PushSize + 1}))
    return std::nullopt;
  const va_t Call = Entry + PushSize + 5;
  const auto Target = registration_detail::addSignedOffset(
      Call + 5, readLE<int32_t>(Bytes + PushSize + 6));
  const auto Helper =
      Target ? readCxxEAXPrologue(Img, *Target, Checked, Reads) : std::nullopt;
  if (!Helper)
    return std::nullopt;
  X86CxxPrologueCall Result;
  Result.Caller = {Entry, Entry + Size};
  Result.Helper = Helper->Code;
  Result.CallVA = Call;
  Result.HandlerVA = readLE<uint32_t>(Bytes + PushSize + 1);
  Result.HandlerPointerVA = Entry + PushSize + 1;
  Result.SecurityCookieVA = Helper->SecurityCookieVA;
  Result.AllocationBytes = Allocation;
  Result.PostCallStackOffset = -28 - Allocation;
  using Value = X86CxxPrologueWrite::Value;
  Result.Writes = {
      {X86CxxPrologueCall::RegistrationOffset + 4, Value::Handler},
      {X86CxxPrologueCall::RegistrationOffset, Value::PreviousChain},
      {-16 - Allocation, Value::EBX},
      {-20 - Allocation, Value::ESI},
      {-24 - Allocation, Value::EDI},
      {0, Value::PreviousEBP},
      {Result.PostCallStackOffset, Value::EncodedCookie}};
  if (Helper->Extra != ExtraFrameStore::None)
    Result.Writes.push_back({-16, Helper->Extra == ExtraFrameStore::StackPointer
                                      ? Value::StackPointer
                                      : Value::EncodedCookie});
  Result.Writes.push_back(
      {X86CxxPrologueCall::TryLevelOffset, Value::InitialState});
  return Result;
}

std::optional<va_t> eaxCallerEntry(const Segment &Seg, size_t CallOffset) {
  if (CallOffset < 7 || Seg.Data[CallOffset - 5] != 0xb8)
    return std::nullopt;
  size_t Begin = CallOffset - 7;
  if (Seg.Data[Begin] != 0x6a) {
    if (CallOffset < 10 || Seg.Data[CallOffset - 10] != 0x68)
      return std::nullopt;
    Begin = CallOffset - 10;
  }
  return Seg.VA + Begin;
}
} // namespace

std::optional<X86CxxPrologueCall>
getCheckedX86CxxPhysicalPrologueCall(const BinaryImage &Img, va_t Entry,
                                     size_t &Work, size_t Limit) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 || !Img.isCOFF())
    return std::nullopt;
  PrologueReads Reads(Img, Work, Limit);
  HelperCache Checked;
  return readCxxEAXPrologueCall(Img, Entry, Checked, Reads);
}

std::optional<X86CxxPrologueCall>
getCheckedX86CxxPrologueCall(const BinaryImage &Img,
                             const ExceptionFunction &Function) {
  if (Function.ParseStatus != ExceptionParseStatus::Complete ||
      Function.Encoding != ExceptionEncoding::X86CxxFuncInfo ||
      !Function.Registration || !Function.CodeRange.isValid())
    return std::nullopt;
  HelperCache Checked;
  PrologueReads Reads(Img);
  auto Call =
      readCxxEAXPrologueCall(Img, Function.CodeRange.Begin, Checked, Reads);
  if (!Call || Call->Caller.End > Function.CodeRange.End ||
      Function.CodeRange.overlaps(Call->Helper) ||
      Call->HandlerVA != Function.PersonalityVA ||
      Call->CallVA != Function.Registration->ChainInstallVA ||
      !getCheckedX86CxxHandlerABI(Img, Function))
    return std::nullopt;
  const registration_detail::SafeSEHTable SafeSEH(Img);
  if (SafeSEH.isMalformed() ||
      (SafeSEH.isPresent() &&
       !isCheckedX86SafeSEHHandlerReference(Img, Call->HandlerVA)))
    return std::nullopt;
  return Call;
}

namespace registration_detail {
std::vector<InstallSite> findCxxEAXPrologueCallers(
    const BinaryImage &Img, const FunctionRangeMap &Functions,
    const SafeSEHTable &SafeSEH, const std::set<va_t> &PhysicalEntries) {
  std::vector<InstallSite> Sites;
  std::vector<InstallSite> Deferred;
  std::map<va_t, va_t> PrecedingGuesses;
  HelperCache Helpers;
  PrologueReads Reads(Img);
  std::set<va_t> Entries;
  size_t BoundaryWork = 0;
  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 12)
      continue;
    for (size_t I = 7; I + 5 <= Seg.Data.size(); ++I) {
      const auto *Call = static_cast<const uint8_t *>(
          std::memchr(Seg.Data.data() + I, 0xe8, Seg.Data.size() - I - 4));
      if (!Call)
        break;
      I = Call - Seg.Data.data();
      const auto Entry = eaxCallerEntry(Seg, I);
      if (!Entry)
        continue;
      const va_t CallVA = Seg.VA + I;
      const auto Protocol = readCxxEAXPrologueCall(Img, *Entry, Helpers, Reads);
      if (!Protocol || Protocol->CallVA != CallVA)
        continue;
      const va_t Handler = Protocol->HandlerVA;
      if (!isExecutableAddress(Img, Handler) ||
          (SafeSEH.isPresent() && !SafeSEH.contains(Handler)))
        continue;
      HandlerIdentity Identity;
      if (!decodeCxxHandlerThunk(Img, Handler, Identity) ||
          !Identity.CxxFuncInfoVA ||
          (Identity.Personality != ExceptionPersonality::CxxFrameHandler3 &&
           Identity.Personality != ExceptionPersonality::CxxFrameHandlerX86))
        continue;
      auto Range = Functions.find(Img, CallVA);
      if (!Range || !Range->contains(*Entry) || !Range->contains(CallVA + 4) ||
          Range->overlaps(Protocol->Helper))
        continue;
      const auto Next = Entries.upper_bound(CallVA);
      if (Next != Entries.begin())
        Range->Begin = std::max(Range->Begin, *std::prev(Next));
      if (*Entry < Range->Begin || Entries.count(*Entry) ||
          PrecedingGuesses.count(*Entry))
        continue;
      const bool Closed =
          Range->Begin == *Entry ||
          checkedRegistrationRangeEnd(Img, {Range->Begin, *Entry},
                                      {Range->Begin}, BoundaryWork);
      if (!Closed) {
        // A stated entry remains authoritative. A padding guess needs both
        // an independent instruction-ownership witness and a reached direct
        // call to this complete helper protocol.
        if (!Functions.isBoundaryGuess(Range->Begin) ||
            std::any_of(Img.Symbols.begin(), Img.Symbols.end(),
                        [&](const auto &S) {
                          return S.IsFunc && S.Addr == Range->Begin &&
                                 !S.IsBoundaryGuess;
                        }))
          continue;
        PrecedingGuesses.emplace(*Entry, Range->Begin);
      }
      Range->Begin = *Entry;
      InstallSite Site;
      Site.Range = *Range;
      Site.HasPhysicalEntry = true;
      Site.ReturningEntryCall = CallVA;
      Site.InstallVA = CallVA;
      Site.HandlerVA = Handler;
      Site.TryLevel = -1;
      Site.Identity = std::move(Identity);
      if (Closed) {
        Entries.insert(*Entry);
        Sites.push_back(std::move(Site));
      } else
        Deferred.push_back(std::move(Site));
    }
  }
  if (!Deferred.empty()) {
    auto Roots = PhysicalEntries;
    Roots.insert(Entries.begin(), Entries.end());
    const auto Recovered = checkedRegistrationCallBoundaries(
        Img, Roots, PrecedingGuesses, BoundaryWork);
    for (auto &Site : Deferred)
      if (Recovered.count(Site.Range.Begin))
        Sites.push_back(std::move(Site));
  }
  std::sort(Sites.begin(), Sites.end(), [](const auto &A, const auto &B) {
    return A.Range.Begin < B.Range.Begin;
  });
  for (size_t I = 1; I < Sites.size(); ++I)
    Sites[I - 1].Range.End =
        std::min(Sites[I - 1].Range.End, Sites[I].Range.Begin);
  return Sites;
}
} // namespace registration_detail
} // namespace neverd::coff_loader
