//===- COFFExceptionPersonality.cpp - PE personality resolution -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionDetail.h"

#include "neverd/Common.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::coff_loader::detail {

std::string directNameAt(const BinaryImage &Img, va_t Address) {
  if (const Import *Imp = Img.findImportAt(Address); Imp && !Imp->Name.empty())
    return Imp->Name;
  if (const Export *Exp = Img.findExportAt(Address); Exp && !Exp->Name.empty())
    return Exp->Name;
  // One address can carry both a name the image stated and a `sub_` placeholder
  // some earlier pass minted for the same code -- the unwind table alone names
  // every function it describes that way.  A placeholder is not a name: taking
  // the first match would let whichever pass ran first decide whether a
  // personality is `__gxx_personality_seh0` or nothing at all.
  const Symbol *Placeholder = nullptr;
  for (const Symbol &Sym : Img.Symbols) {
    if (Sym.Addr != Address || Sym.Name.empty())
      continue;
    if (llvm::StringRef(Sym.Name).starts_with(kAutoFuncPrefix)) {
      Placeholder = &Sym;
      continue;
    }
    return Sym.Name;
  }
  return Placeholder ? Placeholder->Name : std::string();
}

std::optional<va_t> addSignedOffset(va_t Base, int64_t Offset) {
  if (Offset < 0) {
    uint64_t Distance = static_cast<uint64_t>(-(Offset + 1)) + 1;
    if (Distance > Base)
      return std::nullopt;
    return Base - Distance;
  }
  if (static_cast<uint64_t>(Offset) > InvalidVA - Base)
    return std::nullopt;
  return Base + static_cast<uint64_t>(Offset);
}

namespace {

int64_t signExtend(uint64_t Value, unsigned Bits) {
  uint64_t Sign = uint64_t(1) << (Bits - 1);
  return static_cast<int64_t>((Value ^ Sign) - Sign);
}

std::optional<va_t> decodeAArch64Veneer(const BinaryImage &Img, va_t Current,
                                        std::string &ImportName) {
  const uint8_t *Bytes = Img.readVA(Current, 12);
  if (!Bytes)
    return std::nullopt;
  uint32_t First = readLE<uint32_t>(Bytes);

  // Direct B imm26 veneer.
  if ((First & 0xfc000000u) == 0x14000000u) {
    int64_t Disp = signExtend(uint64_t(First & 0x03ffffffu) << 2, 28);
    return addSignedOffset(Current, Disp);
  }

  uint32_t Second = readLE<uint32_t>(Bytes + 4);
  uint32_t Third = readLE<uint32_t>(Bytes + 8);
  if ((First & 0x9f000000u) == 0x90000000u) {
    unsigned Reg = First & 0x1fu;
    uint64_t Imm21 = ((First >> 29) & 3u) | ((First >> 3) & 0x1ffffcu);
    int64_t PageDisp = signExtend(Imm21, 21) << 12;
    auto Page = addSignedOffset(Current & ~va_t(0xfff), PageDisp);
    if (!Page)
      return std::nullopt;

    // adrp Xn; ldr Xt, [Xn, #imm12*8]; br Xt
    if ((Second & 0xffc00000u) == 0xf9400000u &&
        ((Second >> 5) & 0x1fu) == Reg &&
        (Third & 0xfffffc1fu) == 0xd61f0000u &&
        ((Third >> 5) & 0x1fu) == (Second & 0x1fu)) {
      uint64_t Offset = uint64_t((Second >> 10) & 0xfffu) * 8;
      if (Offset > InvalidVA - *Page)
        return std::nullopt;
      va_t Slot = *Page + Offset;
      ImportName = directNameAt(Img, Slot);
      if (!ImportName.empty())
        return Current;
      if (auto Target = readScalar<uint64_t>(Img, Slot))
        return *Target;
      return std::nullopt;
    }

    // adrp Xn; add Xn, Xn, #imm12{, lsl #12}; br Xn
    if ((Second & 0xff000000u) == 0x91000000u && (Second & 0x1fu) == Reg &&
        ((Second >> 5) & 0x1fu) == Reg &&
        (Third & 0xfffffc1fu) == 0xd61f0000u && ((Third >> 5) & 0x1fu) == Reg) {
      uint64_t Offset = (Second >> 10) & 0xfffu;
      if ((Second & (1u << 22)) != 0)
        Offset <<= 12;
      if (Offset <= InvalidVA - *Page)
        return *Page + Offset;
    }
  }

  // ldr Xt, literal; br Xt.  Import libraries sometimes name the literal IAT
  // slot, while static veneers contain an absolute target pointer there.
  if ((First & 0xff000000u) == 0x58000000u &&
      (Second & 0xfffffc1fu) == 0xd61f0000u &&
      ((Second >> 5) & 0x1fu) == (First & 0x1fu)) {
    int64_t Disp = signExtend(uint64_t((First >> 5) & 0x7ffffu) << 2, 21);
    auto Slot = addSignedOffset(Current, Disp);
    if (!Slot)
      return std::nullopt;
    ImportName = directNameAt(Img, *Slot);
    if (!ImportName.empty())
      return Current;
    if (auto Target = readScalar<uint64_t>(Img, *Slot))
      return *Target;
  }
  return std::nullopt;
}

std::optional<va_t> decodeARMBranchVeneer(const BinaryImage &Img,
                                          va_t Current) {
  bool Thumb = (Current & 1u) != 0;
  va_t CodeVA = Current & ~va_t(1);
  if (!Thumb) {
    const uint8_t *Code = Img.readVA(CodeVA, 4);
    if (!Code)
      return std::nullopt;
    uint32_t Insn = readLE<uint32_t>(Code);
    if ((Insn & 0x0f000000u) != 0x0a000000u)
      return std::nullopt;
    int64_t Disp = signExtend(uint64_t(Insn & 0x00ffffffu) << 2, 26);
    return addSignedOffset(CodeVA + 8, Disp);
  }

  const uint8_t *Code = Img.readVA(CodeVA, 4);
  if (!Code)
    return std::nullopt;
  uint16_t First = readLE<uint16_t>(Code);
  if ((First & 0xf800u) == 0xe000u) {
    int64_t Disp = signExtend(uint64_t(First & 0x7ffu) << 1, 12);
    auto Target = addSignedOffset(CodeVA + 4, Disp);
    return Target ? std::optional<va_t>(*Target | 1u) : std::nullopt;
  }
  uint16_t Second = readLE<uint16_t>(Code + 2);
  if ((First & 0xf800u) != 0xf000u || (Second & 0xd000u) != 0x9000u)
    return std::nullopt;
  uint32_t S = (First >> 10) & 1u;
  uint32_t J1 = (Second >> 13) & 1u;
  uint32_t J2 = (Second >> 11) & 1u;
  uint32_t I1 = !(J1 ^ S);
  uint32_t I2 = !(J2 ^ S);
  uint32_t Imm25 = (S << 24) | (I1 << 23) | (I2 << 22) |
                   ((First & 0x3ffu) << 12) | ((Second & 0x7ffu) << 1);
  auto Target = addSignedOffset(CodeVA + 4, signExtend(Imm25, 25));
  return Target ? std::optional<va_t>(*Target | 1u) : std::nullopt;
}

} // namespace

std::pair<va_t, std::string> resolvePersonality(const BinaryImage &Img,
                                                va_t Start) {
  va_t Current = Start;
  std::vector<va_t> Seen;
  for (unsigned Depth = 0; Depth < MaxPersonalityVeneers; ++Depth) {
    if (std::find(Seen.begin(), Seen.end(), Current) != Seen.end())
      break;
    Seen.push_back(Current);
    if (std::string Name = directNameAt(Img, Current); !Name.empty())
      return {Current, std::move(Name)};

    if (Img.Arch == Arch::AArch64) {
      std::string ImportName;
      auto Target = decodeAArch64Veneer(Img, Current, ImportName);
      if (!ImportName.empty())
        return {Current, std::move(ImportName)};
      if (!Target)
        break;
      Current = *Target;
      continue;
    }
    if (Img.Arch == Arch::ARM) {
      if (std::string Name = directNameAt(Img, Current & ~va_t(1));
          !Name.empty())
        return {Current & ~va_t(1), std::move(Name)};
      auto Target = decodeARMBranchVeneer(Img, Current);
      if (!Target)
        break;
      Current = *Target;
      continue;
    }
    if (Img.Arch != Arch::X64)
      break;
    const uint8_t *Code = Img.readVA(Current, 6);
    if (!Code || Current > InvalidVA - 6)
      break;
    if (Code[0] == 0xe9) {
      int32_t Rel = readLE<int32_t>(Code + 1);
      va_t NextIP = Current + 5;
      auto Target = addSignedOffset(NextIP, Rel);
      if (!Target)
        break;
      Current = *Target;
      continue;
    }
    if (Code[0] == 0xff && Code[1] == 0x25) {
      int32_t Disp = readLE<int32_t>(Code + 2);
      va_t NextIP = Current + 6;
      auto Slot = addSignedOffset(NextIP, Disp);
      if (!Slot)
        break;
      if (std::string Name = directNameAt(Img, *Slot); !Name.empty())
        return {Current, std::move(Name)};
    }
    break;
  }
  return {Current, {}};
}

ExceptionPersonality classifyPersonality(llvm::StringRef Name) {
  // Strip the spellings a PE personality can arrive in but a symbol table does
  // not use: a `module!symbol` qualification, the `__imp_` prefix of an import
  // thunk, Darwin's leading underscore, and the `@N` stdcall decoration.
  llvm::StringRef Bare = Name;
  if (size_t Bang = Bare.rfind('!'); Bang != llvm::StringRef::npos)
    Bare = Bare.drop_front(Bang + 1);
  while (Bare.consume_front("__imp_"))
    ;
  while (Bare.consume_front("_"))
    ;
  // A leading `@` is part of a Pascal-mangled name rather than a decoration,
  // so only a later one delimits an stdcall argument-byte suffix.
  if (size_t At = Bare.find('@', 1); At != llvm::StringRef::npos)
    Bare = Bare.take_front(At);

  if (Bare == "C_specific_handler")
    return ExceptionPersonality::CSpecificHandler;
  if (Bare == "CxxFrameHandler3")
    return ExceptionPersonality::CxxFrameHandler3;
  if (Bare == "CxxFrameHandler4")
    return ExceptionPersonality::CxxFrameHandler4;
  if (Bare == "GSHandlerCheck_SEH")
    return ExceptionPersonality::GSHandlerCheckSEH;
  if (Bare == "GSHandlerCheck_EH")
    return ExceptionPersonality::GSHandlerCheckEH;
  if (Bare == "GSHandlerCheck_EH4")
    return ExceptionPersonality::GSHandlerCheckEH4;

  // A PE is not only ever built by MSVC.  Delphi installs its own handler on
  // x64, MinGW installs the Itanium ones, Rust installs its own on the GNU
  // targets, and Go installs a trampoline on the one landing pad that can be
  // entered from C.  None of their language data is in a Windows dialect, so
  // none of it is parsed below -- but naming the personality is the difference
  // between reporting a frame's dispatch and reporting nothing about it.
  // The stripping above is for the MSVC names compared just above it, which a
  // PE spells without their underscores.  The shared table is keyed by the
  // canonical spelling instead, and for these routines the underscores are
  // part of it -- `__gxx_personality_seh0` is the name, not a decoration of
  // one -- so what was taken off is offered back rather than guessed at.  A
  // name that resolved to nothing still had a personality installed, so it is
  // unnamed rather than absent.
  const std::string Once = ("_" + Bare).str();
  const std::string Twice = ("__" + Bare).str();
  for (llvm::StringRef Candidate :
       {Bare, llvm::StringRef(Once), llvm::StringRef(Twice)})
    if (ExceptionPersonality P = classifyPersonalityName(Candidate);
        P != ExceptionPersonality::None)
      return P;
  return ExceptionPersonality::Unknown;
}

} // namespace neverd::coff_loader::detail
