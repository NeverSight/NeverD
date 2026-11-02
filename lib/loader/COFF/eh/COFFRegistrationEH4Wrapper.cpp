//===- COFFRegistrationEH4Wrapper.cpp - Retained PE32 EH4 wrappers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Authenticate retained EH4 forwarding code and its runtime dependencies.
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
#include <vector>

namespace neverd::coff_loader {
namespace {

bool isCommonHandlerSlot(const BinaryImage &Image, va_t Slot) {
  const Import *Runtime = nullptr;
  for (const auto &Import : Image.Imports)
    if (Import.IATAddr == Slot) {
      if (Runtime)
        return false;
      Runtime = &Import;
    }
  if (!Runtime || Runtime->IsDelayImport || Slot > UINT32_MAX - 3 ||
      !Image.readVA(Slot, 4) || Runtime->Name != "_except_handler4_common" ||
      !Image.isValidImportStorageSlot(Slot, Runtime->Name))
    return false;
  return isX86SEH4CommonProvider(Runtime->Module);
}

bool isCommonHandlerThunk(const BinaryImage &Image, va_t Address,
                          X86SEHPersonalityABI &Result);

std::optional<X86SEHPersonalityABI>
getDirectPersonality(const BinaryImage &Image, va_t HandlerVA) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Image.Base > UINT32_MAX - 3 ||
      !Image.DynInfo.SecurityCookieRVA ||
      Image.DynInfo.SecurityCookieRVA > UINT32_MAX - 3 - Image.Base ||
      HandlerVA > UINT32_MAX - 37)
    return std::nullopt;
  const va_t CookieVA = Image.Base + Image.DynInfo.SecurityCookieRVA;
  const auto First = readImmutableCodeBytes(Image, HandlerVA, 2);
  if (!First)
    return std::nullopt;
  const uint32_t Hotpatch = (*First)[0] == 0x8b && (*First)[1] == 0xff ? 2 : 0;
  const va_t Begin = HandlerVA + Hotpatch;
  const auto *Candidate = Image.readVA(Begin, 26);
  if (!Candidate || (Candidate[25] != 0xe8 && Candidate[25] != 0xff))
    return std::nullopt;
  const bool Direct = Candidate[25] == 0xe8;
  const uint32_t Tail = Direct ? 30 : 31;
  std::vector<va_t> Operands{Begin + 16, Begin + 21};
  if (!Direct)
    Operands.push_back(Begin + 27);
  const auto Bytes = readImmutablePE32CodeBytes(Image, HandlerVA,
                                                Hotpatch + Tail + 5, Operands);
  if (!Bytes || !Image.readVA(CookieVA, 4))
    return std::nullopt;
  const auto *Code = Bytes->data() + Hotpatch;
  if (Code[0] != 0x55 || !((Code[1] == 0x89 && Code[2] == 0xe5) ||
                           (Code[1] == 0x8b && Code[2] == 0xec)))
    return std::nullopt;
  for (unsigned Arg = 0; Arg != 4; ++Arg)
    if (Code[3 + Arg * 3] != 0xff || Code[4 + Arg * 3] != 0x75 ||
        Code[5 + Arg * 3] != 20 - Arg * 4)
      return std::nullopt;
  if (Code[15] != 0x68 || Code[20] != 0x68 ||
      readLE<uint32_t>(Code + 21) != CookieVA || Code[Tail] != 0x83 ||
      Code[Tail + 1] != 0xc4 || Code[Tail + 2] != 24 ||
      Code[Tail + 3] != 0x5d || Code[Tail + 4] != 0xc3)
    return std::nullopt;
  const va_t CheckVA = readLE<uint32_t>(Code + 16);
  if (!Image.isCodeAddress(CheckVA) || !Image.readVA(CheckVA, 1))
    return std::nullopt;
  X86SEHPersonalityABI Result;
  Result.SecurityCookieVA = CookieVA;
  Result.CookieCheckVA = CheckVA;
  Result.CodeRanges.push_back({HandlerVA, Begin + Tail + 5});
  Result.AbsoluteOperands.emplace(Begin + 16, CheckVA);
  Result.AbsoluteOperands.emplace(Begin + 21, CookieVA);
  if (Direct) {
    const va_t Target =
        uint32_t(Begin + 30 + int64_t(readLE<int32_t>(Code + 26)));
    if (!isCommonHandlerThunk(Image, Target, Result))
      return std::nullopt;
  } else if (Code[26] != 0x15 ||
             !isCommonHandlerSlot(Image, readLE<uint32_t>(Code + 27))) {
    return std::nullopt;
  } else {
    Result.IATVA = readLE<uint32_t>(Code + 27);
    Result.AbsoluteOperands.emplace(Begin + 27, Result.IATVA);
  }
  return Result;
}

// The retained normalizer and original common-handler wrapper run at their
// source addresses. Only their frame independence and forwarding ABI are
// proved here; no exception-code translation is synthesized by NeverD.
bool isCommonHandlerThunk(const BinaryImage &Image, va_t Address,
                          X86SEHPersonalityABI &Result) {
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
      Result.CodeRanges.push_back({Address, Address + 5});
      Address = va_t(Target);
      continue;
    }
    Code = readImmutablePE32CodeBytes(Image, Address, 6, {Address + 2});
    if (!Code || (*Code)[0] != 0xff || (*Code)[1] != 0x25)
      return false;
    const va_t Slot = readLE<uint32_t>(Code->data() + 2);
    if (!isCommonHandlerSlot(Image, Slot))
      return false;
    Result.CodeRanges.push_back({Address, Address + 6});
    Result.IATVA = Slot;
    Result.AbsoluteOperands.emplace(Address + 2, Slot);
    return true;
  }
  return false;
}

std::optional<X86SEHPersonalityABI>
getNormalizingPersonality(const BinaryImage &Image, va_t HandlerVA) {
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
      !Image.readVA(Checker, 1))
    return std::nullopt;
  const auto Scalar =
      getCheckedX86RegistrationScalarLeaf(Image, va_t(Normalizer));
  if (!Scalar)
    return std::nullopt;
  X86SEHPersonalityABI Result;
  Result.SecurityCookieVA = CookieVA;
  Result.CookieCheckVA = Checker;
  Result.AbsoluteOperands = Scalar->AbsoluteOperands;
  Result.AbsoluteOperands.emplace(HandlerVA + 27, Checker);
  Result.AbsoluteOperands.emplace(HandlerVA + 32, CookieVA);
  Result.CodeRanges.push_back({HandlerVA, HandlerVA + 47});
  Result.CodeRanges.insert(Result.CodeRanges.end(), Scalar->CodeRanges.begin(),
                           Scalar->CodeRanges.end());
  Result.ImageReads = Scalar->ImageReads;
  if (!isCommonHandlerThunk(Image, va_t(Common), Result))
    return std::nullopt;
  return Result;
}

std::optional<X86SEHPersonalityABI> getPersonality(const BinaryImage &Image,
                                                   va_t HandlerVA) {
  if (auto ABI = getDirectPersonality(Image, HandlerVA))
    return ABI;
  return getNormalizingPersonality(Image, HandlerVA);
}

} // namespace

std::optional<va_t> getCheckedX86EH4CookieCheck(const BinaryImage &Image,
                                                va_t HandlerVA) {
  const auto ABI = getPersonality(Image, HandlerVA);
  return ABI ? std::optional<va_t>(ABI->CookieCheckVA) : std::nullopt;
}

std::optional<X86SEHPersonalityABI>
getCheckedX86EH4PersonalityABI(const BinaryImage &Image, va_t HandlerVA) {
  auto ABI = getPersonality(Image, HandlerVA);
  if (!ABI || !isFileBackedPEImportSlot(Image, ABI->IATVA))
    return std::nullopt;
  return ABI;
}

} // namespace neverd::coff_loader
