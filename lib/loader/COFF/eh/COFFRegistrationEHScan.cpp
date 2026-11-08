//===- COFFRegistrationEHScan.cpp - x86-32 chain install scanning --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/Limits.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd::coff_loader::registration_detail {
namespace {

/// Recognize a read of the `FS:[0]` chain head.  A frame must read the head
/// before it can link a record in front of it, which makes the read the one
/// instruction every install shape has in common.  Three encodings appear in
/// practice and they differ only in where the value lands:
///
///     64 A1 00 00 00 00      mov  eax, dword ptr fs:[0]     (MSVC)
///     64 8B 0D 00 00 00 00   mov  ecx, dword ptr fs:[0]     (clang-cl)
///     64 FF 35 00 00 00 00   push dword ptr fs:[0]          (LLVM)
///
/// Returns the encoded length, or zero when \p Code does not start with one.
size_t chainHeadReadLength(const uint8_t *Code, size_t Available) {
  if (Available >= 6 && Code[0] == 0x64 && Code[1] == 0xA1 &&
      readLE<uint32_t>(Code + 2) == 0)
    return 6;
  if (Available < 7 || Code[0] != 0x64 || readLE<uint32_t>(Code + 3) != 0)
    return 0;
  // ModRM mod=00 rm=101 is the absolute-displacement form; the reg field
  // selects the destination and does not change the encoding length.
  const bool IsAbsoluteDisp = (Code[2] & 0xC7) == 0x05;
  if (Code[1] == 0x8B && IsAbsoluteDisp)
    return 7;
  if (Code[1] == 0xFF && Code[2] == 0x35)
    return 7;
  return 0;
}

/// A 32-bit constant the prologue materialized shortly before linking the
/// record, and therefore a candidate for the handler or the scope table.
struct InstallOperand {
  va_t SiteVA = 0;
  uint32_t Value = 0;
};

/// Collect the 32-bit constants the prologue materializes in the window
/// preceding \p Offset.
///
/// The three install shapes place the handler and table differently — MSVC
/// pushes them, clang-cl stores them into frame slots — so rather than
/// hard-coding one instruction sequence this gathers every constant a
/// prologue could have produced and leaves the choice to validation.  Only the
/// two encodings that can carry an address are scanned, and each is
/// fixed-length from its opcode byte, so a hit is checked by re-deriving its
/// own length rather than by trusting the byte stream to be aligned:
///
///     68 <imm32>                 push  imm32
///     C7 45 <disp8>  <imm32>     mov   dword ptr [ebp+disp8], imm32
///     C7 44 24 <disp8> <imm32>   mov   dword ptr [esp+disp8], imm32
///     C7 85 <disp32> <imm32>     mov   dword ptr [ebp+disp32], imm32
///
/// The same unindexed disp8/disp32 memory encodings may use another base GPR.
/// This only observes constants; decoded instruction and frame proofs remain
/// responsible for authenticating their storage and registration lifetime.
std::vector<InstallOperand> collectInstallOperands(const Segment &Seg,
                                                   size_t Offset) {
  std::vector<InstallOperand> Operands;
  const uint8_t *Data = Seg.Data.data();
  const size_t Start =
      Offset > ::neverd::limits::loader::kCOFFRegistrationEHScanMaxInstallWindow
          ? Offset - ::neverd::limits::loader::
                         kCOFFRegistrationEHScanMaxInstallWindow
          : 0;
  for (size_t I = Start; I < Offset; ++I) {
    size_t ImmOffset = 0;
    if (Data[I] == 0x68) {
      ImmOffset = I + 1;
    } else if (Data[I] == 0xC7 && I + 1 < Offset) {
      const uint8_t Modrm = Data[I + 1];
      const unsigned Mod = Modrm >> 6;
      const unsigned Base = Modrm & 7;
      if ((Modrm & 0x38) || (Mod != 1 && Mod != 2))
        continue;
      size_t AddressBytes = Mod == 1 ? 3 : 6;
      if (Base == 4) {
        if (I + 2 >= Offset || (Data[I + 2] & 0xf8) != 0x20)
          continue;
        ++AddressBytes;
      }
      ImmOffset = I + AddressBytes;
    } else {
      continue;
    }
    if (ImmOffset + 4 > Seg.Data.size())
      continue;
    Operands.push_back({Seg.VA + I, readLE<uint32_t>(Data + ImmOffset)});
  }
  return Operands;
}

/// The initial try level a prologue seeds, which distinguishes the two SEH
/// scope-table dialects: `_except_handler3` starts at -1 and
/// `_except_handler4` at -2.
std::optional<int32_t> findSeededTryLevel(const Segment &Seg, size_t Offset) {
  const uint8_t *Data = Seg.Data.data();
  const size_t Start =
      Offset > ::neverd::limits::loader::kCOFFRegistrationEHScanMaxInstallWindow
          ? Offset - ::neverd::limits::loader::
                         kCOFFRegistrationEHScanMaxInstallWindow
          : 0;
  std::optional<int32_t> Level;
  for (size_t I = Start; I < Offset; ++I) {
    // push imm8
    if (Data[I] == 0x6A && I + 1 < Offset) {
      int32_t Value = static_cast<int8_t>(Data[I + 1]);
      if (Value == -1 || Value == -2)
        Level = Value;
      continue;
    }
    // mov dword ptr [ebp+disp8], imm32
    if (Data[I] == 0xC7 && I + 6 < Offset && Data[I + 1] == 0x45) {
      int32_t Value = static_cast<int32_t>(readLE<uint32_t>(Data + I + 3));
      if (Value == -1 || Value == -2)
        Level = Value;
    }
  }
  return Level;
}

/// A direct MSVC install can introduce a frame inside a range guessed from
/// symbols or padding. Observe its own entry before deduplicating by owner;
/// the layout and lifted-state proofs still authenticate the registration.
std::optional<va_t> directInstallEntry(const BinaryImage &Img,
                                       const InstallSite &Site) {
  const bool IsCxx = Site.Identity.CxxFuncInfoVA != 0;
  const unsigned Prefix = IsCxx ? 10 : 15;
  if (Site.InstallVA < Prefix || !Site.TryLevel)
    return std::nullopt;
  const va_t Entry = Site.InstallVA - Prefix;
  const auto *Code = Img.readVA(Entry, Prefix + 14);
  if (!Code || !isExecutableAddress(Img, Entry) || Code[0] != 0x55 ||
      !((Code[1] == 0x8b && Code[2] == 0xec) ||
        (Code[1] == 0x89 && Code[2] == 0xe5)) ||
      Code[3] != 0x6a || int8_t(Code[4]) != *Site.TryLevel)
    return std::nullopt;
  unsigned Cursor = 5;
  if (!IsCxx) {
    if (!Site.TableVA || Code[Cursor] != 0x68 ||
        readLE<uint32_t>(Code + Cursor + 1) != Site.TableVA)
      return std::nullopt;
    Cursor += 5;
  }
  if (Code[Cursor] != 0x68 ||
      readLE<uint32_t>(Code + Cursor + 1) != Site.HandlerVA ||
      Code[Prefix] != 0x64 || Code[Prefix + 1] != 0xa1 ||
      readLE<uint32_t>(Code + Prefix + 2) != 0 || Code[Prefix + 6] != 0x50)
    return std::nullopt;
  // A scope-table frame with a cookie goes on to allocate and encode before it
  // publishes the record; the seed, table and handler already name this frame.
  if (IsCxx &&
      (Code[Prefix + 7] != 0x64 || Code[Prefix + 8] != 0x89 ||
       Code[Prefix + 9] != 0x25 || readLE<uint32_t>(Code + Prefix + 10) != 0))
    return std::nullopt;
  return Entry;
}

/// clang-cl builds the record in words of a frame its prologue has already set
/// up, and pads between functions with no-ops, so only that prologue says where
/// the function starts:
///
///     push  ebp
///     mov   ebp, esp
///     ...
///     mov   dword ptr [ebp - 0x10], -1
///     mov   dword ptr [ebp - 0x14], <table>
///     lea   eax, [ebp - 0x1c]
///     mov   dword ptr [ebp - 0x18], <handler>
///     mov   ecx, fs:[0]
///
/// Everything from the nearest prologue on has to run straight to the chain
/// read at \p Offset, and the words have to be the record's: the handler, the
/// table above it and the try level above that.
std::optional<va_t> slotRecordEntry(const BinaryImage &Img, const Segment &Seg,
                                    size_t Offset, size_t &Work) {
  const uint8_t *Data = Seg.Data.data();
  const size_t Start =
      Offset > ::neverd::limits::loader::kCOFFRegistrationEHScanMaxInstallWindow
          ? Offset - ::neverd::limits::loader::
                         kCOFFRegistrationEHScanMaxInstallWindow
          : 0;
  // Each of the three words is a `mov dword ptr [ebp + disp8], imm32`.
  size_t Writes = 0;
  for (size_t I = Start; I + 1 < Offset; ++I)
    Writes += Data[I] == 0xC7 && Data[I + 1] == 0x45;
  if (Writes < 3)
    return std::nullopt;
  for (size_t K = Offset; K-- > Start;) {
    if (Data[K] != 0x55 || K + 3 > Offset ||
        !((Data[K + 1] == 0x89 && Data[K + 2] == 0xE5) ||
          (Data[K + 1] == 0x8B && Data[K + 2] == 0xEC)))
      continue;
    const auto Words = checkedRegistrationStraightPrologue(
        Img, Seg.VA + K, Seg.VA + Offset, Work);
    if (!Words)
      continue;
    auto At = [&](int64_t Slot) -> std::optional<uint32_t> {
      for (const RegistrationFrameWord &Word : *Words)
        if (Word.Slot == Slot)
          return Word.Value;
      return std::nullopt;
    };
    for (const RegistrationFrameWord &Handler : *Words) {
      const auto Table = At(int64_t(Handler.Slot) + 4);
      const auto Level = At(int64_t(Handler.Slot) + 8);
      if (Table && Level && isExecutableAddress(Img, Handler.Value) &&
          !isExecutableAddress(Img, *Table) && Img.readVA(*Table, 12) &&
          (int32_t(*Level) == -1 || int32_t(*Level) == -2))
        return Seg.VA + K;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

/// Observe the caller's exact `push allocation; push table; call helper`
/// prefix. This supplies an entry candidate, not a registration layout.
std::optional<va_t> helperCallerEntry(const Segment &Seg, size_t CallOffset,
                                      va_t TableVA) {
  if (CallOffset < 7 || Seg.Data[CallOffset - 5] != 0x68 ||
      readLE<uint32_t>(Seg.Data.data() + CallOffset - 4) != TableVA)
    return std::nullopt;
  size_t Begin = CallOffset - 7;
  int64_t Allocation = 0;
  if (Seg.Data[Begin] == 0x6a)
    Allocation = int8_t(Seg.Data[Begin + 1]);
  else if (CallOffset >= 10 && Seg.Data[CallOffset - 10] == 0x68) {
    Begin = CallOffset - 10;
    Allocation = readLE<int32_t>(Seg.Data.data() + Begin + 1);
  } else
    return std::nullopt;
  if (Allocation < 0 || Allocation > limits::kMaxSourceCallBorrowedBytes)
    return std::nullopt;
  return Seg.VA + Begin;
}

/// Resolve the frames served by a shared prologue helper.
///
/// At `/O1` and `/O2` MSVC stops inlining the SEH prologue and calls
/// `__SEH_prolog4` instead, a routine that pushes the handler, links the
/// record, and then masks with `__security_cookie` a scope table its *caller*
/// supplied:
///
///     push  <frame size>
///     push  offset <scope table>
///     call  __SEH_prolog4
///
/// The chain-head read therefore sits in the helper, where the table never
/// appears at all.  Attributing the record to the helper would describe one
/// frame the program does not have and lose every frame it does, so a site
/// that proved a handler but no table is resolved by finding the calls.
///
/// Returns the sites the callers establish, or nothing when \p Helper is not a
/// helper — an ordinary frame whose table simply could not be read has no
/// callers that push one.
std::vector<InstallSite>
resolvePrologueHelperCallers(const BinaryImage &Img,
                             const FunctionRangeMap &Functions,
                             const InstallSite &Helper) {
  std::vector<InstallSite> Callers;
  std::vector<va_t> Covered;
  std::set<va_t> ObservedEntries;
  size_t BoundaryWork = 0;

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 5)
      continue;
    const uint8_t *Data = Seg.Data.data();
    const size_t Size = Seg.Data.size();
    for (size_t I = 0; I + x86::kCallRel32Len <= Size; ++I) {
      const void *Call = std::memchr(Data + I, x86::kCallRel32,
                                     Size - x86::kCallRel32Len + 1 - I);
      if (!Call)
        break;
      I = static_cast<size_t>(static_cast<const uint8_t *>(Call) - Data);
      const va_t CallVA = Seg.VA + I;
      // The helper's own entry usually carries no symbol, so the range the
      // install was attributed to starts at whatever function precedes it.
      // What is certain is that a call establishing this frame has to land in
      // the code that runs before the link and close enough to it to be the
      // same prologue -- a call further back would be some other routine that
      // happens to share the range.
      std::optional<va_t> Target =
          addSignedOffset(CallVA + 5, readLE<int32_t>(Data + I + 1));
      if (!Target || *Target > Helper.InstallVA ||
          Helper.InstallVA - *Target >
              ::neverd::limits::loader::kCOFFRegistrationEHScanMaxInstallWindow)
        continue;
      if (!Helper.Range.contains(*Target))
        continue;

      std::optional<ExceptionAddressRange> Range = Functions.find(Img, CallVA);
      if (!Range || (CallVA >= *Target && CallVA <= Helper.InstallVA))
        continue;
      // Nothing may say where the helper starts: a caller just before it shares
      // its tentative range, and needs its own entry proved.
      const bool SharesHelperRange = Range->overlaps(Helper.Range);
      // Earlier callers may have proved a separate entry inside this same
      // tentative symbol range. Subsequent sites belong to that latest owner
      // until another closed body proves a new boundary.
      const auto Next = ObservedEntries.upper_bound(CallVA);
      if (Next != ObservedEntries.begin())
        Range->Begin = std::max(Range->Begin, *std::prev(Next));

      InstallSite Site;
      Site.InstallVA = CallVA;
      Site.HandlerVA = Helper.HandlerVA;
      Site.Identity = Helper.Identity;
      Site.TryLevel = Helper.TryLevel;
      Site.Range = *Range;
      for (const InstallOperand &Candidate : collectInstallOperands(Seg, I)) {
        if (Candidate.Value == 0 || Candidate.Value == Helper.HandlerVA)
          continue;
        if (isExecutableAddress(Img, Candidate.Value) ||
            !Img.readVA(Candidate.Value, 12))
          continue;
        Site.TableVA = Candidate.Value;
      }
      if (Site.TableVA == 0)
        continue;
      bool OwnEntry = false;
      if (const auto Entry = helperCallerEntry(Seg, I, Site.TableVA);
          Entry && Site.Range.contains(*Entry)) {
        OwnEntry = Site.Range.Begin == *Entry;
        // A prefix inside an instruction or reached by ordinary fallthrough
        // belongs to the current function. Only a completely decoded body
        // that closes before it can separate an otherwise unnamed caller, as
        // one that jumps away does: an image entry's `call; jmp` does.
        if (!OwnEntry && checkedRegistrationBodyClosesBefore(
                             Img, Site.Range.Begin, *Entry, BoundaryWork)) {
          Site.Range.Begin = *Entry;
          ObservedEntries.insert(*Entry);
          OwnEntry = true;
        }
      }
      if (SharesHelperRange && !OwnEntry)
        continue;
      // The helper is no part of the frame it builds, which a shared prologue
      // proof rests on.
      if (*Target > CallVA)
        Site.Range.End = std::min(Site.Range.End, *Target);
      if (std::find(Covered.begin(), Covered.end(), Site.Range.Begin) !=
          Covered.end())
        continue;

      Covered.push_back(Site.Range.Begin);
      Callers.push_back(std::move(Site));
    }
  }
  std::sort(Callers.begin(), Callers.end(),
            [](const InstallSite &A, const InstallSite &B) {
              return A.Range.Begin < B.Range.Begin;
            });
  for (size_t I = 1; I < Callers.size(); ++I)
    Callers[I - 1].Range.End =
        std::min(Callers[I - 1].Range.End, Callers[I].Range.Begin);
  return Callers;
}

} // namespace

/// Locate every prologue that links a registration record onto the `FS:[0]`
/// chain, and resolve the handler and table each one installs.
std::vector<InstallSite> findInstallSites(const BinaryImage &Img,
                                          const FunctionRangeMap &Functions,
                                          const SafeSEHTable &SafeSEH) {
  std::vector<InstallSite> Sites;
  std::vector<va_t> CoveredFunctions;
  size_t PrologueWork = 0;

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 8)
      continue;
    const uint8_t *Data = Seg.Data.data();
    const size_t Size = Seg.Data.size();
    for (size_t I = 0; I < Size; ++I) {
      const void *Prefix =
          std::memchr(Data + I, x86::kFSSegmentPrefix, Size - I);
      if (!Prefix)
        break;
      I = static_cast<size_t>(static_cast<const uint8_t *>(Prefix) - Data);
      if (chainHeadReadLength(Data + I, Size - I) == 0)
        continue;

      InstallSite Site;
      Site.InstallVA = Seg.VA + I;
      Site.TryLevel = findSeededTryLevel(Seg, I);

      // Choose the handler among the constants the prologue produced.  When
      // the image published a SafeSEH table the loader will only ever dispatch
      // to an address in it, which makes membership proof rather than
      // heuristic; otherwise the constant has to name code that classifies as
      // a handler on its own.
      std::optional<ExceptionAddressRange> Range =
          Functions.find(Img, Site.InstallVA);
      const auto SlotEntry = slotRecordEntry(Img, Seg, I, PrologueWork);
      if (SlotEntry)
        Range = Functions.findFrom(Img, *SlotEntry, Site.InstallVA);
      if (!Range)
        continue;

      std::vector<InstallOperand> Operands = collectInstallOperands(Seg, I);
      for (const InstallOperand &Candidate : Operands) {
        if (Candidate.Value == 0 || !isExecutableAddress(Img, Candidate.Value))
          continue;
        if (SafeSEH.isPresent() && !SafeSEH.contains(Candidate.Value))
          continue;
        HandlerIdentity Probe = identifyHandler(Img, Candidate.Value);
        bool IsWrapper = false;
        if (Probe.Personality == ExceptionPersonality::Unknown ||
            Probe.Personality == ExceptionPersonality::None) {
          if (!decodeCxxHandlerThunk(Img, Candidate.Value, Probe) &&
              isExceptHandler4Wrapper(Img, Candidate.Value)) {
            IsWrapper = true;
            Probe.Personality = ExceptionPersonality::ExceptHandler4;
            Probe.Name = getExceptionPersonalityName(
                ExceptionPersonality::ExceptHandler4);
          }
        }
        // Compiler-created C++ handlers may be private to this tentative
        // range. Their exact FuncInfo thunk proves the language identity, as
        // the wrapper's whole body does `_except_handler4`'s, which the CRT
        // links next to the shared prologue. An in-range generic/Delphi target
        // cannot inherit that exception.
        if (Range->contains(Candidate.Value) && !Probe.CxxFuncInfoVA &&
            !IsWrapper)
          continue;
        const bool Classified =
            Probe.Personality != ExceptionPersonality::Unknown &&
            Probe.Personality != ExceptionPersonality::None;
        if (!Classified && !SafeSEH.isPresent())
          continue;
        Site.HandlerVA = Candidate.Value;
        Site.Identity = Probe;
        if (Classified)
          break;
      }
      if (Site.HandlerVA == 0)
        continue;

      // The scope table is the remaining constant that decodes as one.  A C++
      // frame has none here because its table travels inside the handler.
      if (Site.Identity.CxxFuncInfoVA == 0)
        for (const InstallOperand &Candidate : Operands) {
          if (Candidate.Value == Site.HandlerVA || Candidate.Value == 0)
            continue;
          if (isExecutableAddress(Img, Candidate.Value) ||
              !Img.readVA(Candidate.Value, 12))
            continue;
          Site.TableVA = Candidate.Value;
        }

      // A second complete direct prologue owns a separate frame even when
      // entry discovery placed both installs in one tentative range.
      auto Entry = SlotEntry ? SlotEntry : directInstallEntry(Img, Site);
      if (!Entry)
        Entry = alignedCxxInstallEntry(Img, Site);
      if (!Entry) {
        Site.Range = *Range;
        if (const auto CookieEntry = directCxxCookieInstallEntry(Img, Site)) {
          Entry = CookieEntry->Address;
          Site.ReturningEntryCall = CookieEntry->ReturningCall;
        }
      }
      if (Entry && Range->contains(*Entry)) {
        // A hot-patchable function starts at the `mov edi, edi` before its
        // scope-table frame.
        if (!Site.Identity.CxxFuncInfoVA && *Entry >= Range->Begin + 2)
          if (const auto *Patch = Img.readVA(*Entry - 2, 2);
              Patch && Patch[0] == 0x8b && Patch[1] == 0xff)
            *Entry -= 2;
        Range->Begin = *Entry;
        Site.HasPhysicalEntry = true;
      }
      // Other hits inside one body describe the same re-established frame.
      if (std::find(CoveredFunctions.begin(), CoveredFunctions.end(),
                    Range->Begin) != CoveredFunctions.end())
        continue;
      CoveredFunctions.push_back(Range->Begin);
      Site.Range = *Range;
      Sites.push_back(std::move(Site));
    }
  }
  return Sites;
}

/// Replace every shared-prologue-helper site with the frames it serves.
void expandPrologueHelpers(const BinaryImage &Img,
                           const FunctionRangeMap &Functions,
                           std::vector<InstallSite> &Sites) {
  std::vector<InstallSite> Expanded;
  for (InstallSite &Site : Sites) {
    const bool LooksLikeHelper = Site.TableVA == 0 && Site.HandlerVA != 0 &&
                                 Site.Identity.CxxFuncInfoVA == 0;
    std::vector<InstallSite> Callers;
    if (LooksLikeHelper)
      Callers = resolvePrologueHelperCallers(Img, Functions, Site);
    if (Callers.empty()) {
      Expanded.push_back(std::move(Site));
      continue;
    }
    for (InstallSite &Caller : Callers)
      Expanded.push_back(std::move(Caller));
  }
  Sites = std::move(Expanded);
}

} // namespace neverd::coff_loader::registration_detail
