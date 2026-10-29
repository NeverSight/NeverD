//===- COFFRegistrationEH4Wrapper.cpp - Retained PE32 EH4 wrappers
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationScalarLeaf.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/RuntimeProviders.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/ISAEncoding.h"

#include <algorithm>
#include <set>

namespace neverd::coff_loader {
namespace {

std::optional<va_t> getDirectCookieCheck(const BinaryImage &Img,
                                         va_t HandlerVA) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF ||
      Img.Base > UINT32_MAX || !Img.DynInfo.SecurityCookieRVA)
    return std::nullopt;
  const va_t CookieVA = Img.Base + Img.DynInfo.SecurityCookieRVA;
  // Executable, immutable storage whose only absolute operands are the pushed
  // checker and cookie and the runtime's IAT slot, as for the normalizer.
  const auto Bytes = readImmutablePE32CodeBytes(
      Img, HandlerVA, 36, {HandlerVA + 16, HandlerVA + 21, HandlerVA + 27});
  if (!Bytes || CookieVA > UINT32_MAX - 3 || !Img.readVA(CookieVA, 4))
    return std::nullopt;
  const auto *Code = Bytes->data();
  if (Code[0] != 0x55 || !((Code[1] == 0x89 && Code[2] == 0xe5) ||
                           (Code[1] == 0x8b && Code[2] == 0xec)))
    return std::nullopt;
  for (unsigned Arg = 0; Arg != 4; ++Arg)
    if (Code[3 + Arg * 3] != 0xff || Code[4 + Arg * 3] != 0x75 ||
        Code[5 + Arg * 3] != 20 - Arg * 4)
      return std::nullopt;
  if (Code[15] != 0x68 || Code[20] != 0x68 ||
      readLE<uint32_t>(Code + 21) != CookieVA || Code[25] != 0xff ||
      Code[26] != 0x15 || Code[31] != 0x83 || Code[32] != 0xc4 ||
      Code[33] != 24 || Code[34] != 0x5d || Code[35] != 0xc3)
    return std::nullopt;
  const va_t CheckVA = readLE<uint32_t>(Code + 16);
  const auto *Import = Img.findImportAt(readLE<uint32_t>(Code + 27));
  if (!Img.isCodeAddress(CheckVA) || !Img.readVA(CheckVA, 1) || !Import ||
      !Img.readVA(Import->IATAddr, 4) ||
      Import->Name != "_except_handler4_common" ||
      !isX86SEHHandlerProvider(Import->Module))
    return std::nullopt;
  return CheckVA;
}

// The retained normalizer and original common-handler wrapper run at their
// source addresses. Only their frame independence and forwarding ABI are
// proved here; no exception-code translation is synthesized by NeverD.
bool isCommonHandlerThunk(const BinaryImage &Image, va_t Address) {
  std::set<va_t> Seen;
  for (unsigned Hop = 0;
       Hop != ::neverd::limits::loader::kMaxRegistrationEH4WrapperHops; ++Hop) {
    if (!Address || Address > UINT32_MAX - 6 || !Seen.insert(Address).second)
      return false;
    auto Code = readImmutableCodeBytes(Image, Address, 5);
    if (Code && (*Code)[0] == 0xe9) {
      const int64_t Target =
          int64_t(Address) + 5 + readLE<int32_t>(Code->data() + 1);
      if (Target <= 0 || Target > UINT32_MAX)
        return false;
      Address = va_t(Target);
      continue;
    }
    Code = readImmutablePE32CodeBytes(Image, Address, 6, {Address + 2});
    if (!Code || (*Code)[0] != 0xff || (*Code)[1] != 0x25)
      return false;
    const va_t Slot = readLE<uint32_t>(Code->data() + 2);
    const Import *Runtime = nullptr;
    for (const auto &Import : Image.Imports)
      if (Import.IATAddr == Slot) {
        if (Runtime)
          return false;
        Runtime = &Import;
      }
    return Runtime && Slot <= UINT32_MAX - 3 && Image.readVA(Slot, 4) &&
           Runtime->Name == "_except_handler4_common" &&
           isX86SEHHandlerProvider(Runtime->Module);
  }
  return false;
}

std::optional<va_t> getNormalizingCookieCheck(const BinaryImage &Image,
                                              va_t HandlerVA) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Image.Base > UINT32_MAX ||
      !Image.DynInfo.SecurityCookieRVA || !HandlerVA ||
      HandlerVA > UINT32_MAX - 47)
    return std::nullopt;
  const va_t CookieVA = Image.Base + Image.DynInfo.SecurityCookieRVA;
  const auto Bytes = readImmutablePE32CodeBytes(
      Image, HandlerVA, 47, {HandlerVA + 27, HandlerVA + 32});
  if (!Bytes || CookieVA > UINT32_MAX - 3 || !Image.readVA(CookieVA, 4))
    return std::nullopt;
  const auto *Code = Bytes->data();
  const uint8_t Prefix[] = {0x55, 0x8b, 0xec, 0x56, 0x8b,
                            0x75, 0x08, 0xff, 0x36, 0xe8};
  const uint8_t Arguments[] = {0xff, 0x75, 0x14, 0x89, 0x06, 0xff, 0x75,
                               0x10, 0xff, 0x75, 0x0c, 0x56, 0x68};
  const uint8_t Epilogue[] = {0x83, 0xc4, 0x1c, 0x5e, 0x5d, 0xc3};
  if (!std::equal(std::begin(Prefix), std::end(Prefix), Code) ||
      !std::equal(std::begin(Arguments), std::end(Arguments), Code + 14) ||
      !std::equal(std::begin(Epilogue), std::end(Epilogue), Code + 41) ||
      Code[31] != 0x68 || readLE<uint32_t>(Code + 32) != CookieVA ||
      Code[36] != 0xe8)
    return std::nullopt;
  const int64_t Normalizer =
      int64_t(HandlerVA) + 14 + readLE<int32_t>(Code + 10);
  const int64_t Common = int64_t(HandlerVA) + 41 + readLE<int32_t>(Code + 37);
  const va_t Checker = readLE<uint32_t>(Code + 27);
  if (Normalizer <= 0 || Normalizer > UINT32_MAX || Common <= 0 ||
      Common > UINT32_MAX || !Image.isCodeAddress(Checker) ||
      !Image.readVA(Checker, 1) ||
      !isCheckedX86RegistrationScalarLeaf(Image, va_t(Normalizer)) ||
      !isCommonHandlerThunk(Image, va_t(Common)))
    return std::nullopt;
  return Checker;
}

} // namespace

std::optional<va_t> getCheckedX86EH4CookieCheck(const BinaryImage &Image,
                                                va_t HandlerVA) {
  if (auto Check = getDirectCookieCheck(Image, HandlerVA))
    return Check;
  return getNormalizingCookieCheck(Image, HandlerVA);
}

} // namespace neverd::coff_loader
