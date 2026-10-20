//===- COFFRegistrationEH.cpp - x86-32 registration-chain EH -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace neverd::coff_loader {
namespace {

using registration_detail::decodeEH4Header;
using registration_detail::decodeScopeRecords;
using registration_detail::decodeX86FuncInfo;
using registration_detail::diagnose;
using registration_detail::expandPrologueHelpers;
using registration_detail::findInstallSites;
using registration_detail::FunctionRangeMap;
using registration_detail::HandlerIdentity;
using registration_detail::InstallSite;
using registration_detail::proveRealignedCxxRegistrationLayout;
using registration_detail::recoverTryLevelStores;
using registration_detail::SafeSEHTable;

/// Address at which the table after \p TableVA begins, or zero when it is the
/// last one.  \p Sorted holds every table address the image was proven to
/// install, which is what bounds an otherwise unsized entry array.
va_t findNextTableAddress(const std::vector<va_t> &Sorted, va_t TableVA) {
  auto It = std::upper_bound(Sorted.begin(), Sorted.end(), TableVA);
  return It == Sorted.end() ? 0 : *It;
}

/// After the checked four pushes, EH4 can allocate locals and save registers
/// before publishing its node. Follow only that bounded, straight-line prefix;
/// the LowIR state owner still proves cookie values and registration lifetime.
std::optional<size_t> directEH4Publication(const BinaryImage &Img,
                                           const InstallSite &Site,
                                           const RegistrationChainInfo &Chain) {
  const size_t Size = std::min<uint64_t>(Site.Range.size(), 128);
  const uint8_t *P = Img.readVA(Site.Range.Begin, Size);
  if (Img.Base > UINT32_MAX ||
      Img.DynInfo.SecurityCookieRVA > UINT32_MAX - Img.Base)
    return std::nullopt;
  const va_t Cookie = Img.DynInfo.SecurityCookieRVA
                          ? Img.Base + Img.DynInfo.SecurityCookieRVA
                          : 0;
  if (!P || Size < 25 || !Cookie || Cookie > UINT32_MAX ||
      Site.Identity.Personality != ExceptionPersonality::ExceptHandler4)
    return std::nullopt;
  size_t Cursor = 22;
  int64_t Allocation = 0;
  const bool Subtract = P[Cursor + 1] == 0xec;
  if (!Subtract && P[Cursor + 1] != 0xc4)
    return std::nullopt;
  if (P[Cursor] == 0x83) {
    Allocation = int8_t(P[Cursor + 2]);
    Cursor += 3;
  } else if (P[Cursor] == 0x81 && Size >= Cursor + 6) {
    Allocation = readLE<int32_t>(P + Cursor + 2);
    Cursor += 6;
  } else
    return std::nullopt;
  if (!Subtract)
    Allocation = -Allocation;
  if (Allocation < 8 || Allocation > limits::kMaxSourceCallBorrowedBytes)
    return std::nullopt;
  int64_t SP = -16 - Allocation;
  unsigned Saved = 0;
  std::vector<int64_t> SavedSlots;
  while (Cursor < Size &&
         (P[Cursor] == 0x53 || P[Cursor] == 0x56 || P[Cursor] == 0x57)) {
    const unsigned Bit = 1u << (P[Cursor++] - 0x50);
    if (Saved & Bit)
      return std::nullopt;
    Saved |= Bit;
    SP -= 4;
    SavedSlots.push_back(SP);
  }
  if (Size - Cursor < 10 || P[Cursor] != 0xa1 ||
      readLE<uint32_t>(P + Cursor + 1) != Cookie || P[Cursor + 5] != 0x31 ||
      P[Cursor + 6] != 0x45 || P[Cursor + 7] != 0xf8 ||
      !((P[Cursor + 8] == 0x31 && P[Cursor + 9] == 0xe8) ||
        (P[Cursor + 8] == 0x33 && P[Cursor + 9] == 0xc5)))
    return std::nullopt;
  Cursor += 10;
  std::set<int32_t> Cookies{Chain.EHCookieOffset};
  if (Chain.GSCookieOffset != -2) {
    if (int64_t(Chain.GSCookieOffset) < int64_t(Chain.EHCookieOffset) + 4 &&
        int64_t(Chain.EHCookieOffset) < int64_t(Chain.GSCookieOffset) + 4)
      return std::nullopt;
    Cookies.insert(Chain.GSCookieOffset);
  }
  while (!Cookies.empty()) {
    int64_t Slot = 0;
    if (Cursor < Size && P[Cursor] == 0x50) {
      Slot = SP -= 4;
      ++Cursor;
    } else if (Size - Cursor >= 3 && P[Cursor] == 0x89 &&
               P[Cursor + 1] == 0x45) {
      Slot = int8_t(P[Cursor + 2]);
      Cursor += 3;
    } else if (Size - Cursor >= 6 && P[Cursor] == 0x89 &&
               P[Cursor + 1] == 0x85) {
      Slot = readLE<int32_t>(P + Cursor + 2);
      Cursor += 6;
    } else
      return std::nullopt;
    // EBP is node+16 in this exact prologue. Do not admit unallocated storage
    // or overwrite SavedESP, exception pointers, or any byte of the node.
    if (Slot < SP || Slot + 4 > -24 ||
        llvm::any_of(SavedSlots,
                     [&](int64_t Saved) {
                       return Slot < Saved + 4 && Saved < Slot + 4;
                     }) ||
        !Cookies.erase(int32_t(Slot)))
      return std::nullopt;
  }
  if (Size - Cursor < 3 || P[Cursor] != 0x8d || P[Cursor + 1] != 0x45 ||
      P[Cursor + 2] != 0xf0)
    return std::nullopt;
  Cursor += 3;
  const auto Store =
      getX86RegistrationChainStoreSize(Img, Site.Range.Begin + Cursor);
  if (!Store || *Store > Size - Cursor ||
      (*Store == 7 && P[Cursor + 2] != 0x05))
    return std::nullopt;
  return Cursor;
}

/// After the checked `push -1; push <handler>; mov eax, fs:[0]; push eax`
/// preamble a /GS C++ frame allocates locals, saves registers, then pushes a
/// stack guard (`mov eax, __security_cookie; xor eax, ebp; push eax`) before
/// publishing its node with `lea eax, [ebp-0xC]; mov fs:[0], eax`.  The guard
/// is an ordinary pushed word the state solver replays; it is no part of the
/// registration chain and does not move the three-word node, so the frame's
/// coordinates stay those of an unguarded fixed C++ frame (HasSecurityCookies
/// stays false).  Returns the publication store offset, or nothing when the
/// tail is not exactly that shape.
std::optional<size_t> directCxxCookiePublication(const BinaryImage &Img,
                                                 const InstallSite &Site,
                                                 size_t Cursor) {
  const size_t Size = std::min<uint64_t>(Site.Range.size(), 128);
  const uint8_t *P = Img.readVA(Site.Range.Begin, Size);
  if (Img.Base > UINT32_MAX ||
      Img.DynInfo.SecurityCookieRVA > UINT32_MAX - Img.Base)
    return std::nullopt;
  const va_t Cookie = Img.DynInfo.SecurityCookieRVA
                          ? Img.Base + Img.DynInfo.SecurityCookieRVA
                          : 0;
  if (!P || !Cookie || Cookie > UINT32_MAX || Site.Identity.CxxFuncInfoVA == 0)
    return std::nullopt;
  // Optional local allocation.
  if (Cursor + 3 <= Size && P[Cursor] == 0x83 && P[Cursor + 1] == 0xec)
    Cursor += 3;
  else if (Cursor + 6 <= Size && P[Cursor] == 0x81 && P[Cursor + 1] == 0xec)
    Cursor += 6;
  // Callee-saved pushes (EBX/ESI/EDI).
  unsigned Saved = 0;
  while (Cursor < Size &&
         (P[Cursor] == 0x53 || P[Cursor] == 0x56 || P[Cursor] == 0x57)) {
    const unsigned Bit = 1u << (P[Cursor] - 0x50);
    if (Saved & Bit)
      return std::nullopt;
    Saved |= Bit;
    ++Cursor;
  }
  // mov eax, [__security_cookie]; xor eax, ebp; push eax.
  if (Cursor + 8 > Size || P[Cursor] != 0xa1 ||
      readLE<uint32_t>(P + Cursor + 1) != Cookie ||
      !((P[Cursor + 5] == 0x31 && P[Cursor + 6] == 0xe8) ||
        (P[Cursor + 5] == 0x33 && P[Cursor + 6] == 0xc5)) ||
      P[Cursor + 7] != 0x50)
    return std::nullopt;
  Cursor += 8;
  // lea eax, [ebp-0xC]  (the C++ node base).
  if (Cursor + 3 > Size || P[Cursor] != 0x8d || P[Cursor + 1] != 0x45 ||
      P[Cursor + 2] != 0xf4)
    return std::nullopt;
  Cursor += 3;
  const auto Store =
      getX86RegistrationChainStoreSize(Img, Site.Range.Begin + Cursor);
  if (!Store || *Store > Size - Cursor ||
      (*Store == 7 && P[Cursor + 2] != 0x05))
    return std::nullopt;
  return Cursor;
}

/// Authenticate the direct MSVC frame from its exact entry instruction
/// sequence. A chain-head read alone does not establish a registration, and
/// locals containing -1/0 are not evidence of the runtime's state field.
void proveDirectRegistrationLayout(const BinaryImage &Img,
                                   const InstallSite &Site,
                                   RegistrationChainInfo &Chain) {
  const bool IsCxx = Site.Identity.CxxFuncInfoVA != 0;
  if (IsCxx &&
      (proveRealignedCxxRegistrationLayout(Img, Site, Chain) ||
       registration_detail::proveFixedCxxRegistrationLayout(Img, Site, Chain)))
    return;
  const size_t Size = IsCxx ? 24 : 29;
  const uint8_t *P = Img.readVA(Site.Range.Begin, Size);
  if (!P || Site.Range.size() < Size || P[0] != 0x55 ||
      !((P[1] == 0x8B && P[2] == 0xEC) || (P[1] == 0x89 && P[2] == 0xE5)) ||
      P[3] != 0x6A || !Chain.SeededTryLevel ||
      static_cast<int8_t>(P[4]) != *Chain.SeededTryLevel)
    return;
  size_t Cursor = 5;
  if (!IsCxx) {
    if (P[Cursor] != 0x68 || readLE<uint32_t>(P + Cursor + 1) != Site.TableVA)
      return;
    Cursor += 5;
  }
  if (P[Cursor] != 0x68 || readLE<uint32_t>(P + Cursor + 1) != Site.HandlerVA)
    return;
  Cursor += 5;
  if (Site.InstallVA != Site.Range.Begin + Cursor || P[Cursor] != 0x64 ||
      P[Cursor + 1] != 0xA1 || readLE<uint32_t>(P + Cursor + 2) != 0 ||
      P[Cursor + 6] != 0x50)
    return;
  size_t Installation = Cursor + 7;
  if (P[Cursor + 7] != 0x64 || P[Cursor + 8] != 0x89 || P[Cursor + 9] != 0x25 ||
      readLE<uint32_t>(P + Cursor + 10) != 0) {
    const auto Publication =
        IsCxx ? directCxxCookiePublication(Img, Site, Cursor + 7)
              : directEH4Publication(Img, Site, Chain);
    if (!Publication)
      return;
    Installation = *Publication;
  }
  Chain.RegistrationOffset = IsCxx ? -12 : -16;
  Chain.TryLevelOffset = -4;
  Chain.ChainInstallVA = Site.Range.Begin + Installation;
}

} // namespace

void parseX86RegistrationExceptions(BinaryImage &Img) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF)
    return;

  const FunctionRangeMap Functions(Img);
  const SafeSEHTable SafeSEH(Img);
  if (SafeSEH.isMalformed()) {
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Malformed);
    Img.ExceptionMetadata.Diagnostics.push_back("malformed x86 SafeSEH table");
    return;
  }
  std::vector<InstallSite> Sites = findInstallSites(Img, Functions, SafeSEH);
  expandPrologueHelpers(Img, Functions, Sites);
  if (Sites.empty())
    return;

  // Every table address the image installs, so each entry array can be capped
  // at the next one.  Both the scope tables and the C++ `FuncInfo` records
  // take part: the compiler emits them into the same read-only region, so a
  // `FuncInfo` is just as much a boundary for the scope table before it.
  std::vector<va_t> TableAddresses;
  for (const InstallSite &Site : Sites) {
    if (Site.Identity.CxxFuncInfoVA != 0)
      TableAddresses.push_back(Site.Identity.CxxFuncInfoVA);
    if (Site.TableVA != 0)
      TableAddresses.push_back(Site.TableVA);
  }
  std::sort(TableAddresses.begin(), TableAddresses.end());
  TableAddresses.erase(
      std::unique(TableAddresses.begin(), TableAddresses.end()),
      TableAddresses.end());

  std::vector<ExceptionFunction> Recovered;
  Recovered.reserve(Sites.size());
  for (const InstallSite &Site : Sites) {
    const HandlerIdentity &Identity = Site.Identity;
    ExceptionFunction F;
    F.CodeRange = Site.Range;
    F.Kind = RuntimeFunctionKind::Primary;
    F.PersonalityVA = Site.HandlerVA;
    F.PersonalityName = Identity.Name;
    F.Personality = Identity.Personality;
    if (F.Personality == ExceptionPersonality::Unknown)
      diagnose(F, ExceptionParseStatus::Partial,
               "x86 registration handler identity is not proven");

    RegistrationChainInfo Chain;
    Chain.HandlerVA = Site.HandlerVA;
    Chain.ChainInstallVA = Site.InstallVA;
    Chain.ScopeTableVA = Site.TableVA;

    if (Identity.CxxFuncInfoVA != 0) {
      F.Encoding = ExceptionEncoding::X86CxxFuncInfo;
      F.HandlerDataVA = Identity.CxxFuncInfoVA;
      Chain.ScopeTableVA = Identity.CxxFuncInfoVA;
      decodeX86FuncInfo(F, Img, Identity.CxxFuncInfoVA);
      Chain.SeededTryLevel = -1;
    } else if (Site.TableVA != 0 && Img.readVA(Site.TableVA, 12)) {
      // `_except_handler4` seeds -2 as the initial try level and prefixes
      // its table with cookie displacements; `_except_handler3` seeds -1
      // and starts at the entry array.  When the handler kept its name that
      // is authoritative. For an unknown handler the sentinel supplies only
      // an inspectable layout observation, never runtime semantics.
      bool IsEH4 = F.Personality == ExceptionPersonality::ExceptHandler4 ||
                   (F.Personality != ExceptionPersonality::ExceptHandler3 &&
                    Site.TryLevel && *Site.TryLevel == -2);
      F.HandlerDataVA = Site.TableVA;
      va_t ArrayVA = Site.TableVA;
      if (IsEH4) {
        if (!decodeEH4Header(Img, Site.TableVA, Chain)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated _except_handler4 scope-table header");
        } else {
          ArrayVA = Site.TableVA + 16;
        }
        F.Encoding = ExceptionEncoding::X86ScopeTableEH4;
      } else {
        F.Encoding = ExceptionEncoding::X86ScopeTableEH3;
      }
      // Both sentinels mean "no scope is current"; which one this frame uses
      // follows from the handler it installed.
      Chain.SeededTryLevel = IsEH4 ? -2 : -1;
      const va_t Limit = findNextTableAddress(TableAddresses, Site.TableVA);
      bool ScopeBudgetExhausted = false;
      if (decodeScopeRecords(Img, ArrayVA, Limit, IsEH4, Chain.Scopes,
                             ScopeBudgetExhausted) == 0)
        diagnose(F, ExceptionParseStatus::Partial,
                 "x86 scope table at 0x" + llvm::utohexstr(ArrayVA) +
                     " declares no usable entry");
      if (ScopeBudgetExhausted)
        diagnose(
            F, ExceptionParseStatus::Partial,
            "x86 scope-table decode budget exhausted before a proven boundary");
    } else {
      F.Encoding = ExceptionEncoding::X86ScopeTableEH3;
      diagnose(F, ExceptionParseStatus::Partial,
               "x86 registration record installs a handler with no "
               "recoverable table");
    }

    proveDirectRegistrationLayout(Img, Site, Chain);
    registration_detail::recoverRegistrationCallbackRanges(F, Img, Functions,
                                                           Chain);
    if (Chain.SeededTryLevel)
      recoverTryLevelStores(Img, F.CodeRange, *Chain.SeededTryLevel,
                            F.Cxx ? F.Cxx->MaxState : Chain.Scopes.size(),
                            Chain);
    if (Chain.SeededTryLevel)
      for (const auto &Fragment : F.FragmentRanges)
        recoverTryLevelStores(Img, Fragment, *Chain.SeededTryLevel,
                              F.Cxx ? F.Cxx->MaxState : Chain.Scopes.size(),
                              Chain);

    F.Registration = std::move(Chain);
    // Registration personalities and language tables were resolved together
    // above. The generic table-driven resolver must not reinterpret this
    // absolute-pointer FuncInfo as a trailing x64 language-data record.
    F.LanguageTablesResolved = true;
    if (F.Personality == ExceptionPersonality::Unknown)
      diagnose(F, ExceptionParseStatus::Partial,
               "unknown x86 registration handler");

    Recovered.push_back(std::move(F));
  }

  for (ExceptionFunction &F : Recovered) {
    // An untyped PE export alone is not a callable entry. The exact decoded
    // direct registration prologue supplies that missing instruction evidence.
    const auto InstallSize = F.Registration
                                 ? getX86RegistrationChainStoreSize(
                                       Img, F.Registration->ChainInstallVA)
                                 : std::nullopt;
    if (F.ParseStatus == ExceptionParseStatus::Complete && F.Registration &&
        InstallSize && F.Registration->RegistrationOffset &&
        F.Registration->TryLevelOffset &&
        F.CodeRange.contains(F.Registration->ChainInstallVA) &&
        Img.hasExecutableCodeOwnerRange(F.CodeRange.Begin,
                                        F.Registration->ChainInstallVA -
                                            F.CodeRange.Begin + *InstallSize))
      Img.VerifiedFunctionEntries.insert(F.CodeRange.Begin);
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, F.ParseStatus);
    Img.ExceptionMetadata.addModel(F.model());
    Img.ExceptionMetadata.Functions.push_back(std::move(F));
  }
  Img.ExceptionMetadata.rebuildIndex();

  // The data-pointer scan ran before this parse and took each relocated
  // scope-table pointer for a function.  The thunks are code of the function
  // that installed the record, as FuncDetector treats them: a guess goes, and
  // a stated symbol stays as a label.
  const std::set<va_t> Thunks = Img.ExceptionMetadata.registrationScopeThunks();
  const auto PointerRoles = getCheckedX86RegistrationPointerRoles(Img);
  llvm::erase_if(Img.Symbols, [&](const Symbol &Sym) {
    return Sym.IsFunc && Sym.Origin == NameOrigin::Synthesized &&
           (Thunks.count(Sym.Addr) ||
            (PointerRoles &&
             PointerRoles->RuntimeOnlyPointerTargets.count(Sym.Addr)));
  });
  for (Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && Thunks.count(Sym.Addr))
      Sym.IsFunc = false;
}

} // namespace neverd::coff_loader
