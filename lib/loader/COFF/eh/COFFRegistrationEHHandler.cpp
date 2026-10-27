//===- COFFRegistrationEHHandler.cpp - x86-32 handler identity -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Authenticate registration handler identities and retained code dependencies.
//===----------------------------------------------------------------------===//

#include "../../ReadOnlyBytesIndex.h"
#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/ISAEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::coff_loader {

std::optional<X86CookieCheckSuccessPath>
getCheckedX86CookieCheckSuccessPath(const BinaryImage &Img, va_t CheckVA) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF ||
      Img.Bits != Bitness::Bits32 || Img.Base > UINT32_MAX ||
      CheckVA > UINT32_MAX - 14 || !Img.DynInfo.SecurityCookieRVA ||
      Img.DynInfo.SecurityCookieRVA > UINT32_MAX - Img.Base)
    return std::nullopt;
  const va_t Cookie = Img.Base + Img.DynInfo.SecurityCookieRVA;
  const auto *Code = Img.readVA(CheckVA, 9);
  if (Cookie > UINT32_MAX - 3 || !Img.readVA(Cookie, 4) || !Code ||
      Code[0] != 0x3b || Code[1] != 0x0d ||
      readLE<uint32_t>(Code + 2) != Cookie)
    return std::nullopt;
  size_t Success = 0;
  int64_t Failure = 0;
  if (Code[6] == 0x75) {
    Success = 8;
    Failure = int64_t(CheckVA) + Success + int8_t(Code[7]);
  } else {
    Code = Img.readVA(CheckVA, 13);
    if (!Code || Code[6] != 0x0f || Code[7] != 0x85)
      return std::nullopt;
    Success = 12;
    Failure = int64_t(CheckVA) + Success + readLE<int32_t>(Code + 8);
  }
  size_t Extent = Success + 1;
  if (Code[Success] == 0xf3) {
    ++Extent;
    Code = Img.readVA(CheckVA, Extent);
    if (!Code || Code[Success + 1] != 0xc3)
      return std::nullopt;
  } else if (Code[Success] != 0xc3)
    return std::nullopt;
  // Eliding this call relies on these instructions remaining unchanged after
  // loading. Only CMP's absolute cookie operand can carry a PE32 base fixup;
  // opcode, branch and return fixups cannot certify this success path.
  if (!readImmutablePE32CodeBytes(Img, CheckVA, Extent, {CheckVA + 2}) ||
      !Img.hasExecutableCodeOwnerRange(CheckVA, Extent) || Failure <= 0 ||
      Failure > UINT32_MAX || !Img.isCodeAddress(va_t(Failure)) ||
      !Img.readVA(va_t(Failure), 1) ||
      (uint64_t(Failure) >= CheckVA && uint64_t(Failure) < CheckVA + Extent))
    return std::nullopt;
  return X86CookieCheckSuccessPath{Cookie,
                                   va_t(Failure),
                                   {CheckVA, CheckVA + Extent},
                                   {{CheckVA + 2, Cookie}}};
}

bool hasCheckedX86CookieCheckSuccessPath(const BinaryImage &Img, va_t CheckVA) {
  return getCheckedX86CookieCheckSuccessPath(Img, CheckVA).has_value();
}

namespace {
std::optional<X86SEHPersonalityABI>
getSEH3Personality(const BinaryImage &Img, va_t HandlerVA, bool Immutable) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF)
    return std::nullopt;
  if (Immutable && (Img.Bits != Bitness::Bits32 || Img.IsRelocatable ||
                    Img.Base > UINT32_MAX))
    return std::nullopt;
  X86SEHPersonalityABI Result;
  std::set<va_t> Seen;
  for (unsigned Hop = 0;
       Hop != ::neverd::limits::loader::kMaxRegistrationHandlerHops; ++Hop) {
    if (!Seen.insert(HandlerVA).second ||
        (Immutable && (!HandlerVA || HandlerVA > UINT32_MAX - 6)))
      return std::nullopt;
    const auto *Code = Img.readVA(HandlerVA, 5);
    if (!Code || !Img.hasExecutableCodeOwnerRange(HandlerVA, 5))
      return std::nullopt;
    if (Code[0] == 0xe9) {
      if (Immutable && !readImmutableCodeBytes(Img, HandlerVA, 5))
        return std::nullopt;
      const int64_t Target = int64_t(HandlerVA) + 5 + readLE<int32_t>(Code + 1);
      if (Target <= 0 || Target > UINT32_MAX)
        return std::nullopt;
      Result.CodeRanges.push_back({HandlerVA, HandlerVA + 5});
      HandlerVA = va_t(Target);
      continue;
    }
    Code = Img.readVA(HandlerVA, 6);
    if (!Code || !Img.hasExecutableCodeOwnerRange(HandlerVA, 6) ||
        Code[0] != 0xff || Code[1] != 0x25)
      return std::nullopt;
    const va_t Slot = readLE<uint32_t>(Code + 2);
    const auto *Import = Img.findImportAt(Slot);
    if (!Import || Import->Name != "_except_handler3" ||
        !Img.readVA(Import->IATAddr, 4) ||
        !(llvm::StringRef(Import->Module).equals_insensitive("msvcrt.dll") ||
          llvm::StringRef(Import->Module).equals_insensitive("ucrtbase.dll") ||
          llvm::StringRef(Import->Module)
              .equals_insensitive("vcruntime140.dll") ||
          llvm::StringRef(Import->Module)
              .equals_insensitive("vcruntime140d.dll")))
      return std::nullopt;
    if (Immutable) {
      if (Import->IATAddr != Slot || !isFileBackedPEImportSlot(Img, Slot) ||
          !readImmutablePE32CodeBytes(Img, HandlerVA, 6, {HandlerVA + 2}))
        return std::nullopt;
      for (const auto &Other : Img.Imports)
        if (&Other != Import && Other.IATAddr == Import->IATAddr)
          return std::nullopt;
    }
    Result.CodeRanges.push_back({HandlerVA, HandlerVA + 6});
    Result.AbsoluteOperands.emplace(HandlerVA + 2, Slot);
    Result.IATVA = Import->IATAddr;
    return Result;
  }
  return std::nullopt;
}
} // namespace

bool isCheckedX86SEH3Personality(const BinaryImage &Img, va_t HandlerVA) {
  return getSEH3Personality(Img, HandlerVA, false).has_value();
}

std::optional<X86SEHPersonalityABI>
getCheckedX86SEH3PersonalityABI(const BinaryImage &Img, va_t HandlerVA) {
  return getSEH3Personality(Img, HandlerVA, true);
}

std::optional<X86SEHRuntimeCode>
getCheckedX86SEHRuntimeCode(const BinaryImage &Img,
                            const ExceptionFunction &Function, size_t &Work) {
  const auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork -
                     std::min(Work, limits::kMaxRegistrationEHStateWork)) {
      Work = limits::kMaxRegistrationEHStateWork;
      return false;
    }
    Work += Amount;
    return true;
  };
  if (!Charge(1) || Function.Cxx || !Function.Registration ||
      Function.Registration->HandlerVA != Function.PersonalityVA ||
      Function.ParseStatus != ExceptionParseStatus::Complete)
    return std::nullopt;
  const bool EH4 = Function.Personality == ExceptionPersonality::ExceptHandler4;
  if (!EH4 && Function.Personality != ExceptionPersonality::ExceptHandler3)
    return std::nullopt;
  const auto ABI =
      EH4 ? getCheckedX86EH4PersonalityABI(Img, Function.PersonalityVA)
          : getCheckedX86SEH3PersonalityABI(Img, Function.PersonalityVA);
  if (!ABI || !ABI->ImageReads.empty() || !Charge(Img.Imports.size()))
    return std::nullopt;
  const auto *Import = Img.findImportAt(ABI->IATVA);
  if (!Import || Import->IATAddr != ABI->IATVA)
    return std::nullopt;
  X86SEHRuntimeCode Result;
  Result.IATVA = ABI->IATVA;
  Result.ImportModule = Import->Module;
  Result.ImportName = Import->Name;
  Result.SecurityCookieVA = ABI->SecurityCookieVA;
  Result.CookieCheckVA = ABI->CookieCheckVA;
  if (!Charge(ABI->AbsoluteOperands.size()) || !Charge(ABI->CodeRanges.size()))
    return std::nullopt;
  Result.AbsoluteOperands = ABI->AbsoluteOperands;
  auto Ranges = ABI->CodeRanges;
  if (EH4) {
    const auto Cookie =
        getCheckedX86CookieCheckSuccessPath(Img, ABI->CookieCheckVA);
    if (!Cookie || Cookie->SecurityCookieVA != ABI->SecurityCookieVA ||
        !Charge(Cookie->AbsoluteOperands.size() + 1))
      return std::nullopt;
    Result.CookieFailureVA = Cookie->FailureVA;
    Ranges.push_back(Cookie->CodeRange);
    for (const auto &[Address, Target] : Cookie->AbsoluteOperands)
      if (!Result.AbsoluteOperands.emplace(Address, Target).second)
        return std::nullopt;
  }
  size_t Covered = 0;
  for (const auto &Range : Ranges) {
    if (!Range.isValid() || Range.size() > UINT32_MAX ||
        !Charge(Result.AbsoluteOperands.size() + Result.Code.size() + 1))
      return std::nullopt;
    for (const auto &[Begin, Bytes] : Result.Code)
      if (Range.overlaps({Begin, Begin + Bytes.size()}))
        return std::nullopt;
    std::vector<va_t> Operands;
    for (const auto &[Address, Target] : Result.AbsoluteOperands)
      if (Range.contains(Address))
        Operands.push_back(Address);
    auto Bytes =
        readRelocatedPE32CodeBytes(Img, Range.Begin, Range.size(), Operands,
                                   Work, limits::kMaxRegistrationEHStateWork);
    if (!Bytes)
      return std::nullopt;
    for (va_t Address : Operands) {
      if (readLE<uint32_t>(Bytes->data() + (Address - Range.Begin)) !=
          Result.AbsoluteOperands.at(Address))
        return std::nullopt;
      ++Covered;
    }
    Result.Code.emplace(Range.Begin, std::move(*Bytes));
  }
  if (Covered != Result.AbsoluteOperands.size() || Result.Code.empty())
    return std::nullopt;
  return Result;
}

} // namespace neverd::coff_loader

namespace neverd::coff_loader::registration_detail {
namespace {

/// Follow one `jmp rel32` / `jmp [iat]` veneer and name what it reaches.
/// Returns an empty string when \p Address is not a veneer or its destination
/// has no name.
std::string resolveVeneerTargetName(const BinaryImage &Img, va_t Address) {
  auto Opcode = readScalar<uint8_t>(Img, Address);
  if (!Opcode)
    return {};
  if (*Opcode == 0xE9) {
    auto Displacement = readScalar<int32_t>(Img, Address + 1);
    if (!Displacement)
      return {};
    if (auto Target = addSignedOffset(Address + 5, *Displacement))
      return resolveRoutineName(Img, *Target);
    return {};
  }
  if (*Opcode != 0xFF)
    return {};
  auto Modrm = readScalar<uint8_t>(Img, Address + 1);
  if (!Modrm || *Modrm != 0x25)
    return {};
  auto Slot = readScalar<uint32_t>(Img, Address + 2);
  if (!Slot)
    return {};
  std::string Name = resolveRoutineName(Img, 0, *Slot);
  if (Name.empty())
    if (auto Bound = readScalar<uint32_t>(Img, *Slot))
      Name = resolveRoutineName(Img, *Bound, *Slot);
  return Name;
}

} // namespace

/// Decode the `__ehhandler$` thunk MSVC emits for each x86-32 C++ frame:
///
///     [nop; nop]
///     mov eax, offset __ehfuncinfo$<mangled>
///     jmp __CxxFrameHandler3
///
/// The thunk is how a per-function `FuncInfo` reaches a shared personality
/// that takes it in `EAX`, so the `mov` immediate is the only place the table
/// address appears.
bool decodeCxxHandlerThunk(const BinaryImage &Img, va_t HandlerVA,
                           HandlerIdentity &Identity) {
  for (size_t Offset = 0;
       Offset <
       ::neverd::limits::loader::kCOFFRegistrationEHHandlerMaxThunkBytes;
       ++Offset) {
    const va_t Cursor = HandlerVA + Offset;
    auto Opcode = readScalar<uint8_t>(Img, Cursor);
    if (!Opcode)
      return false;
    if (*Opcode != 0xB8)
      continue;
    auto FuncInfo = readScalar<uint32_t>(Img, Cursor + 1);
    if (!FuncInfo || *FuncInfo == 0 || !Img.readVA(*FuncInfo, 4))
      continue;

    va_t Target = 0;
    auto Jump = readScalar<uint8_t>(Img, Cursor + 5);
    if (!Jump)
      return false;
    if (*Jump == 0xE9) {
      auto Displacement = readScalar<int32_t>(Img, Cursor + 6);
      if (!Displacement)
        continue;
      const va_t Resolved = uint32_t(Cursor + 10 + uint32_t(*Displacement));
      if (!isExecutableAddress(Img, Resolved))
        continue;
      Target = Resolved;
    } else if (*Jump == 0xFF) {
      // `jmp dword ptr [__imp___CxxFrameHandler3]`, which is what an import
      // of the personality looks like when the linker did not build a veneer.
      auto Modrm = readScalar<uint8_t>(Img, Cursor + 6);
      if (!Modrm || *Modrm != 0x25)
        continue;
      auto Slot = readScalar<uint32_t>(Img, Cursor + 7);
      if (!Slot)
        continue;
      std::string SlotName = resolveRoutineName(Img, 0, *Slot);
      if (SlotName.empty())
        if (auto Bound = readScalar<uint32_t>(Img, *Slot))
          SlotName = resolveRoutineName(Img, *Bound, *Slot);
      Identity.CxxFuncInfoVA = *FuncInfo;
      Identity.CxxThunkBodyVA = Cursor;
      Identity.CxxThunkEndVA = Cursor + 11;
      Identity.CxxDispatchIATVA = *Slot;
      ExceptionPersonality Resolved = classifyPersonalityName(SlotName);
      Identity.Personality =
          isCxxPersonality(Resolved) ? Resolved : ExceptionPersonality::Unknown;
      Identity.Name = std::move(SlotName);
      return true;
    } else {
      continue;
    }

    Identity.CxxFuncInfoVA = *FuncInfo;
    Identity.CxxThunkBodyVA = Cursor;
    Identity.CxxThunkEndVA = Cursor + 10;
    Identity.CxxDispatchVA = Target;
    std::string TargetName = resolveRoutineName(Img, Target);
    if (TargetName.empty())
      TargetName = resolveVeneerTargetName(Img, Target);
    ExceptionPersonality Resolved = classifyPersonalityName(TargetName);
    if (isCxxPersonality(Resolved)) {
      Identity.Personality = Resolved;
      Identity.Name = TargetName;
    } else {
      // Table-shaped data passed to an unknown target does not prove what
      // that target will execute. Retain the observation without promoting it.
      Identity.Personality = ExceptionPersonality::Unknown;
      Identity.Name = TargetName;
    }
    return true;
  }
  return false;
}

/// Recognize the per-image `_except_handler4` wrapper.
///
/// `_except_handler4` cannot be imported: it has to read *this* image's
/// `__security_cookie` in order to unmask the scope table, so the CRT supplies
/// only the cookie-agnostic `_except_handler4_common` and the compiler emits a
/// local wrapper that supplies the cookie and forwards to it:
///
///     push  ebp / mov ebp, esp / push esi
///     mov   esi, [ebp+8]
///     push  [esi]  /  call  <decode>  /  mov [esi], eax
///     push  [ebp+14h] ... push esi
///     push  offset <cookie check thunk>
///     push  offset __security_cookie
///     call  _except_handler4_common
///
/// A GS-enabled image therefore installs an address that resolves to no known
/// name, which is why a name lookup alone reports the frame as having an
/// unknown personality.  The wrapper is identified here by what it forwards
/// to, corroborated by its use of the cookie the load configuration names —
/// both facts the image states about itself rather than shapes guessed from
/// bytes.
bool isExceptHandler4Wrapper(const BinaryImage &Img, va_t HandlerVA) {
  const va_t CookieVA = Img.DynInfo.SecurityCookieRVA == 0
                            ? 0
                            : Img.Base + Img.DynInfo.SecurityCookieRVA;
  bool ForwardsToCommon = false;
  bool UsesSecurityCookie = false;

  for (size_t Offset = 0;
       Offset <
       ::neverd::limits::loader::kCOFFRegistrationEHHandlerMaxWrapperBytes;
       ++Offset) {
    const va_t VA = HandlerVA + Offset;
    auto Opcode = readScalar<uint8_t>(Img, VA);
    if (!Opcode)
      break;
    // `push offset __security_cookie`
    if (*Opcode == 0x68 && CookieVA != 0) {
      if (auto Immediate = readScalar<uint32_t>(Img, VA + 1))
        UsesSecurityCookie |= *Immediate == CookieVA;
      continue;
    }
    std::string TargetName;
    if (*Opcode == x86::kCallRel32 || *Opcode == x86::kJmpRel32) {
      auto Displacement = readScalar<int32_t>(Img, VA + x86::kRel32DispOffset);
      if (!Displacement)
        continue;
      auto Target = addSignedOffset(VA + x86::kCallRel32Len, *Displacement);
      if (!Target || !isExecutableAddress(Img, *Target))
        continue;
      TargetName = resolveRoutineName(Img, *Target);
      if (TargetName.empty())
        TargetName = resolveVeneerTargetName(Img, *Target);
    } else if (*Opcode == 0xFF) {
      auto Modrm = readScalar<uint8_t>(Img, VA + 1);
      // `call [mem32]` (reg=2) and `jmp [mem32]` (reg=4), absolute form.
      if (!Modrm || (*Modrm != 0x15 && *Modrm != 0x25))
        continue;
      if (auto Slot = readScalar<uint32_t>(Img, VA + 2)) {
        TargetName = resolveRoutineName(Img, 0, *Slot);
        if (TargetName.empty())
          if (auto Bound = readScalar<uint32_t>(Img, *Slot))
            TargetName = resolveRoutineName(Img, *Bound, *Slot);
      }
    } else {
      continue;
    }
    if (classifyPersonalityName(TargetName) ==
        ExceptionPersonality::ExceptHandler4)
      ForwardsToCommon = true;
  }
  return ForwardsToCommon && (UsesSecurityCookie || CookieVA == 0);
}

/// Name a handler, following the `jmp [iat]` and `jmp rel32` veneers a linker
/// may interpose between the pushed address and the routine itself.
HandlerIdentity identifyHandler(const BinaryImage &Img, va_t HandlerVA) {
  HandlerIdentity Identity;
  va_t Cursor = HandlerVA;
  std::vector<va_t> Seen;
  for (unsigned Hop = 0;
       Hop <
       ::neverd::limits::loader::kCOFFRegistrationEHHandlerMaxHandlerThunkHops;
       ++Hop) {
    if (std::find(Seen.begin(), Seen.end(), Cursor) != Seen.end())
      break;
    Seen.push_back(Cursor);

    std::string Name = resolveRoutineName(Img, Cursor);
    ExceptionPersonality Resolved = classifyPersonalityName(Name);
    if (Resolved != ExceptionPersonality::Unknown &&
        Resolved != ExceptionPersonality::None) {
      Identity.Personality = Resolved;
      Identity.Name = std::move(Name);
      return Identity;
    }
    if (Identity.Name.empty())
      Identity.Name = Name;

    auto Opcode = readScalar<uint8_t>(Img, Cursor);
    if (!Opcode)
      break;
    if (*Opcode == 0xE9) {
      auto Displacement = readScalar<int32_t>(Img, Cursor + 1);
      if (!Displacement)
        break;
      auto Target = addSignedOffset(Cursor + 5, *Displacement);
      if (!Target)
        break;
      Cursor = *Target;
      continue;
    }
    if (*Opcode == 0xFF) {
      auto Modrm = readScalar<uint8_t>(Img, Cursor + 1);
      if (!Modrm || *Modrm != 0x25)
        break;
      auto Slot = readScalar<uint32_t>(Img, Cursor + 2);
      if (!Slot)
        break;
      std::string SlotName = resolveRoutineName(Img, 0, *Slot);
      if (SlotName.empty())
        if (auto Bound = readScalar<uint32_t>(Img, *Slot))
          SlotName = resolveRoutineName(Img, *Bound, *Slot);
      ExceptionPersonality SlotPersonality = classifyPersonalityName(SlotName);
      if (SlotPersonality != ExceptionPersonality::Unknown &&
          SlotPersonality != ExceptionPersonality::None) {
        Identity.Personality = SlotPersonality;
        Identity.Name = std::move(SlotName);
      }
      break;
    }
    break;
  }
  return Identity;
}

} // namespace neverd::coff_loader::registration_detail
