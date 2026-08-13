//===- COFFRegistrationEHHandler.cpp - x86-32 handler identity -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LanguageRuntime.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace neverd::coff_loader::registration_detail {
namespace {

/// Bound on veneer/thunk chasing when naming a handler.
constexpr unsigned MaxHandlerThunkHops = 4;

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
  // The `mov`/`jmp` pair is the tail of the thunk, not necessarily its first
  // instruction: a /GS build prefixes a cookie check, and an /hotpatch or /Gy
  // build prefixes padding.  Scanning for the pair, and requiring the `jmp` to
  // sit immediately after the `mov`, keeps both variants readable without
  // accepting an unrelated `mov eax, imm32` somewhere in a real function.
  constexpr size_t MaxThunkBytes = 64;
  for (size_t Offset = 0; Offset < MaxThunkBytes; ++Offset) {
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
      auto Resolved = addSignedOffset(Cursor + 10, *Displacement);
      if (!Resolved || !isExecutableAddress(Img, *Resolved))
        continue;
      Target = *Resolved;
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
      ExceptionPersonality Resolved = classifyPersonalityName(SlotName);
      Identity.Personality = isCxxPersonality(Resolved)
                                 ? Resolved
                                 : ExceptionPersonality::CxxFrameHandlerX86;
      Identity.Name = std::move(SlotName);
      return true;
    } else {
      continue;
    }

    Identity.CxxFuncInfoVA = *FuncInfo;
    std::string TargetName = resolveRoutineName(Img, Target);
    if (TargetName.empty())
      TargetName = resolveVeneerTargetName(Img, Target);
    ExceptionPersonality Resolved = classifyPersonalityName(TargetName);
    if (isCxxPersonality(Resolved)) {
      Identity.Personality = Resolved;
      Identity.Name = TargetName;
    } else {
      // A statically linked personality can be stripped of its name.  The
      // thunk shape plus a `FuncInfo` that validates is still proof of the C++
      // model, so record the x86 spelling and let the table decode confirm it.
      Identity.Personality = ExceptionPersonality::CxxFrameHandlerX86;
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
  // Longest wrapper MSVC emits is well inside this; scanning further would
  // start reading whatever routine follows it.
  constexpr size_t MaxWrapperBytes = 96;
  const va_t CookieVA = Img.DynInfo.SecurityCookieRVA == 0
                            ? 0
                            : Img.Base + Img.DynInfo.SecurityCookieRVA;
  bool ForwardsToCommon = false;
  bool UsesSecurityCookie = false;

  for (size_t Offset = 0; Offset < MaxWrapperBytes; ++Offset) {
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
    if (*Opcode == 0xE8 || *Opcode == 0xE9) {
      auto Displacement = readScalar<int32_t>(Img, VA + 1);
      if (!Displacement)
        continue;
      auto Target = addSignedOffset(VA + 5, *Displacement);
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
  for (unsigned Hop = 0; Hop < MaxHandlerThunkHops; ++Hop) {
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
