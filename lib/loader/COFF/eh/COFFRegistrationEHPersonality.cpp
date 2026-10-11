//===- COFFRegistrationEHPersonality.cpp - Checked PE32 C++ dispatcher ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include <set>

namespace neverd::coff_loader {
std::optional<X86CxxPersonalityABI>
getCheckedX86CxxPersonalityABI(const BinaryImage &Img,
                               const ExceptionFunction &Function) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || !Function.Registration ||
      !Function.Cxx ||
      Function.Personality != ExceptionPersonality::CxxFrameHandler3 ||
      Function.Registration->HandlerVA != Function.PersonalityVA ||
      !getCheckedX86CxxMetadataRanges(Img, Function))
    return std::nullopt;
  registration_detail::HandlerIdentity Identity;
  if (!registration_detail::decodeCxxHandlerThunk(Img, Function.PersonalityVA,
                                                  Identity) ||
      Identity.CxxFuncInfoVA != Function.Cxx->NativeFuncInfoVA ||
      Identity.CxxThunkBodyVA < Function.PersonalityVA ||
      Identity.CxxThunkBodyVA - Function.PersonalityVA > 24 ||
      Identity.CxxThunkEndVA <= Identity.CxxThunkBodyVA)
    return std::nullopt;
  X86CxxPersonalityABI Result;
  auto ReadCode = [&](va_t VA, uint32_t Size,
                      const std::vector<va_t> &Operands = {})
      -> std::optional<std::vector<uint8_t>> {
    if (!VA || VA > UINT32_MAX || Size > uint64_t(UINT32_MAX) + 1 - VA ||
        !Img.hasExecutableCodeOwnerRange(VA, Size))
      return std::nullopt;
    auto Bytes = readImmutablePE32CodeBytes(Img, VA, Size, Operands);
    if (!Bytes)
      return std::nullopt;
    Result.CodeRanges.push_back({VA, VA + Size});
    return Bytes;
  };
  const uint32_t Prefix = Identity.CxxThunkBodyVA - Function.PersonalityVA;
  std::vector<va_t> Operands{Identity.CxxThunkBodyVA + 1};
  if (Identity.CxxDispatchIATVA)
    Operands.push_back(Identity.CxxThunkBodyVA + 7);
  const auto Thunk =
      ReadCode(Function.PersonalityVA,
               Identity.CxxThunkEndVA - Function.PersonalityVA, Operands);
  if (!Thunk)
    return std::nullopt;
  size_t Padding = 0;
  while (Padding < Prefix && Padding < 8 && (*Thunk)[Padding] == 0x90)
    ++Padding;
  if (Padding != Prefix) {
    // Clang -O0 reads the four handler arguments into EAX before loading
    // FuncInfo. These fixed stack reads preserve ESP, flags and every live
    // register; the immediately following MOV replaces the scratch value.
    if (Prefix - Padding != 16)
      return std::nullopt;
    for (unsigned Arg = 0; Arg != 4; ++Arg) {
      const auto At = Padding + Arg * 4;
      if ((*Thunk)[At] != 0x8b || (*Thunk)[At + 1] != 0x44 ||
          (*Thunk)[At + 2] != 0x24 || (*Thunk)[At + 3] != 16 - Arg * 4)
        return std::nullopt;
    }
  }
  Result.RuntimeVA = Identity.CxxDispatchIATVA ? Identity.CxxThunkBodyVA + 5
                                               : Identity.CxxDispatchVA;
  va_t Cursor = Result.RuntimeVA;
  std::set<va_t> Seen;
  for (unsigned Hop = 0; Hop != 4; ++Hop) {
    if (!Seen.insert(Cursor).second)
      return std::nullopt;
    auto Code = ReadCode(Cursor, 1);
    if (!Code)
      return std::nullopt;
    if ((*Code)[0] == 0xe9) {
      Code = ReadCode(Cursor, 5);
      if (!Code)
        return std::nullopt;
      Cursor =
          uint32_t(Cursor + 5 + uint32_t(readLE<int32_t>(Code->data() + 1)));
      continue;
    }
    Code = ReadCode(Cursor, 6, {Cursor + 2});
    if (!Code || (*Code)[0] != 0xff || (*Code)[1] != 0x25)
      return std::nullopt;
    Result.IATVA = readLE<uint32_t>(Code->data() + 2);
    const Import *Import = nullptr;
    for (const auto &Candidate : Img.Imports)
      if (Candidate.IATAddr == Result.IATVA) {
        if (Import)
          return std::nullopt;
        Import = &Candidate;
      }
    if (!Import || Import->Name != "__CxxFrameHandler3" ||
        (!llvm::StringRef(Import->Module)
              .equals_insensitive("vcruntime140.dll") &&
         !llvm::StringRef(Import->Module)
              .equals_insensitive("vcruntime140d.dll")) ||
        Result.IATVA > UINT32_MAX - 3 || !Img.readVA(Result.IATVA, 4))
      return std::nullopt;
    return Result;
  }
  return std::nullopt;
}

} // namespace neverd::coff_loader
