//===- COFFRegistrationEHScan.cpp - x86-32 chain install scanning --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::coff_loader::registration_detail {
namespace {

/// How far back from an install site the prologue may be searched for the
/// `push` operands.  A frame installs at most a try level, a table pointer,
/// and a handler.
constexpr unsigned MaxInstallPushes = 4;

/// How far back from a chain-head read the operands of the install may be
/// found.  The record is built immediately before the link, so a window this
/// size covers the widest prologue any of the three shapes below emits while
/// staying far short of the previous function.
constexpr size_t MaxInstallWindow = 64;

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
std::vector<InstallOperand> collectInstallOperands(const Segment &Seg,
                                                   size_t Offset) {
  std::vector<InstallOperand> Operands;
  const uint8_t *Data = Seg.Data.data();
  const size_t Start =
      Offset > MaxInstallWindow ? Offset - MaxInstallWindow : 0;
  for (size_t I = Start; I < Offset; ++I) {
    size_t ImmOffset = 0;
    if (Data[I] == 0x68) {
      ImmOffset = I + 1;
    } else if (Data[I] == 0xC7 && I + 1 < Offset) {
      const uint8_t Modrm = Data[I + 1];
      if (Modrm == 0x45)
        ImmOffset = I + 3;
      else if (Modrm == 0x44 && I + 2 < Offset && Data[I + 2] == 0x24)
        ImmOffset = I + 4;
      else if (Modrm == 0x85)
        ImmOffset = I + 6;
      else
        continue;
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
      Offset > MaxInstallWindow ? Offset - MaxInstallWindow : 0;
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

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 5)
      continue;
    const uint8_t *Data = Seg.Data.data();
    const size_t Size = Seg.Data.size();
    for (size_t I = 0; I + 5 <= Size; ++I) {
      if (Data[I] != 0xE8)
        continue;
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
          Helper.InstallVA - *Target > MaxInstallWindow)
        continue;
      if (!Helper.Range.contains(*Target))
        continue;

      std::optional<ExceptionAddressRange> Range = Functions.find(Img, CallVA);
      if (!Range || Range->overlaps(Helper.Range))
        continue;
      if (std::find(Covered.begin(), Covered.end(), Range->Begin) !=
          Covered.end())
        continue;

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

      Covered.push_back(Range->Begin);
      Callers.push_back(std::move(Site));
    }
  }
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

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 8)
      continue;
    const uint8_t *Data = Seg.Data.data();
    const size_t Size = Seg.Data.size();
    for (size_t I = 0; I < Size; ++I) {
      if (Data[I] != 0x64)
        continue;
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
      if (!Range)
        continue;

      std::vector<InstallOperand> Operands = collectInstallOperands(Seg, I);
      for (const InstallOperand &Candidate : Operands) {
        if (Candidate.Value == 0 || !isExecutableAddress(Img, Candidate.Value))
          continue;
        // A handler inside the installing function is a Delphi `TExcDesc`,
        // which the Delphi decoder has already claimed.  Every Windows
        // dialect installs a routine the whole image shares, so none of them
        // can produce a handler here.
        if (Range->contains(Candidate.Value))
          continue;
        if (SafeSEH.isPresent() && !SafeSEH.contains(Candidate.Value))
          continue;
        HandlerIdentity Probe = identifyHandler(Img, Candidate.Value);
        if (Probe.Personality == ExceptionPersonality::Unknown ||
            Probe.Personality == ExceptionPersonality::None) {
          if (!decodeCxxHandlerThunk(Img, Candidate.Value, Probe) &&
              isExceptHandler4Wrapper(Img, Candidate.Value)) {
            Probe.Personality = ExceptionPersonality::ExceptHandler4;
            Probe.Name = getExceptionPersonalityName(
                ExceptionPersonality::ExceptHandler4);
          }
        }
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

      // A function installs one record; a second hit inside the same body is
      // the same frame being re-established, not a new one.
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
