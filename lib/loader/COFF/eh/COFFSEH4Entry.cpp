//===- COFFSEH4Entry.cpp - Physical shared SEH4 calls ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind complete entry/exit helper protocols to their immutable source calls.
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFSEH4Entry.h"

#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <iterator>

namespace neverd::coff_loader {

std::optional<X86SEH4PrologueCall>
getCheckedX86SEH4PrologueCall(const BinaryImage &Image,
                              const ExceptionFunction &Function) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF ||
      Function.ParseStatus != ExceptionParseStatus::Complete ||
      Function.Encoding != ExceptionEncoding::X86ScopeTableEH4 ||
      Function.Personality != ExceptionPersonality::ExceptHandler4 ||
      Function.Cxx || !Function.Registration || !Function.CodeRange.isValid())
    return std::nullopt;
  const auto &Chain = *Function.Registration;
  const va_t Entry = Function.CodeRange.Begin;
  if (Chain.RealignedFrame || Chain.SeededTryLevel != -2 ||
      Chain.HandlerVA != Function.PersonalityVA ||
      Image.Base > UINT32_MAX - 3 || !Image.DynInfo.SecurityCookieRVA ||
      Image.DynInfo.SecurityCookieRVA > UINT32_MAX - 3 - Image.Base ||
      Entry > UINT32_MAX - 14)
    return std::nullopt;
  const auto *Bytes = Image.readVA(Entry, 12);
  if (!Bytes || (Bytes[0] != 0x6a && Bytes[0] != 0x68))
    return std::nullopt;
  const uint32_t Push = Bytes[0] == 0x6a ? 2 : 5;
  const uint32_t Size = Push + 10;
  Bytes = Image.readVA(Entry, Size);
  if (!Bytes || Bytes[Push] != 0x68 || Bytes[Push + 5] != 0xe8 ||
      Entry + Size > Function.CodeRange.End ||
      Chain.ChainInstallVA != Entry + Push + 5 ||
      readLE<uint32_t>(Bytes + Push + 1) != Chain.ScopeTableVA ||
      !readImmutablePE32CodeBytes(Image, Entry, Size, {Entry + Push + 1}))
    return std::nullopt;
  const int32_t Allocation =
      Push == 2 ? int8_t(Bytes[1]) : readLE<int32_t>(Bytes + 1);
  if (Allocation < 0 || Allocation > limits::kMaxSourceCallBorrowedBytes)
    return std::nullopt;
  const va_t Target =
      uint32_t(Entry + Size + int64_t(readLE<int32_t>(Bytes + Push + 6)));
  // The two absolute operands are checked separately. Every other byte owns
  // a required instruction, including PC movement and publication ordering.
  static constexpr uint8_t Body[] = {
      0x68, 0,    0,    0,    0,    0x64, 0xff, 0x35, 0,    0,    0,    0,
      0x8b, 0x44, 0x24, 0x10, 0x89, 0x6c, 0x24, 0x10, 0x8d, 0x6c, 0x24, 0x10,
      0x2b, 0xe0, 0x53, 0x56, 0x57, 0xa1, 0,    0,    0,    0,    0x31, 0x45,
      0xfc, 0x33, 0xc5, 0x50, 0x89, 0x65, 0xe8, 0xff, 0x75, 0xf8, 0x8b, 0x45,
      0xfc, 0xc7, 0x45, 0xfc, 0xfe, 0xff, 0xff, 0xff, 0x89, 0x45, 0xf8, 0x8d,
      0x45, 0xf0, 0x64, 0xa3, 0,    0,    0,    0,    0xc3};
  if (Target > UINT32_MAX - (sizeof(Body) - 1) ||
      Function.CodeRange.overlaps({Target, Target + sizeof(Body)}))
    return std::nullopt;
  const auto Code = readImmutablePE32CodeBytes(Image, Target, sizeof(Body),
                                               {Target + 1, Target + 30});
  if (!Code)
    return std::nullopt;
  for (size_t I = 0; I != sizeof(Body); ++I)
    if (!((I >= 1 && I < 5) || (I >= 30 && I < 34)) && (*Code)[I] != Body[I])
      return std::nullopt;
  const va_t Cookie = Image.Base + Image.DynInfo.SecurityCookieRVA;
  if (readLE<uint32_t>(Code->data() + 1) != Chain.HandlerVA ||
      readLE<uint32_t>(Code->data() + 30) != Cookie ||
      !Image.readVA(Cookie, 4) ||
      !getCheckedX86EH4CookieCheck(Image, Chain.HandlerVA))
    return std::nullopt;
  const registration_detail::SafeSEHTable SafeSEH(Image);
  if (SafeSEH.isMalformed() ||
      (SafeSEH.isPresent() &&
       !isCheckedX86SafeSEHHandlerReference(Image, Chain.HandlerVA)))
    return std::nullopt;

  X86SEH4PrologueCall Result{{Entry, Entry + Size},
                             {Target, Target + sizeof(Body)},
                             Entry + Push + 5,
                             Chain.HandlerVA,
                             Chain.ScopeTableVA,
                             Cookie,
                             uint32_t(Allocation),
                             -32 - Allocation,
                             {},
                             {}};
  Result.AbsoluteOperands = {{Entry + Push + 1, Chain.ScopeTableVA},
                             {Target + 1, Chain.HandlerVA},
                             {Target + 30, Cookie}};
  using Value = X86SEH4PrologueWrite::Value;
  Result.Writes = {{-8, Value::ReturnPC},
                   {-12, Value::Handler},
                   {-16, Value::PreviousChain},
                   {0, Value::PreviousEBP},
                   {-20 - Allocation, Value::EBX},
                   {-24 - Allocation, Value::ESI},
                   {-28 - Allocation, Value::EDI},
                   {-4, Value::EncodedTable},
                   {Result.PostCallStackOffset, Value::EncodedCookie},
                   {-24, Value::StackPointer},
                   {Result.PostCallStackOffset - 4, Value::ReturnPC},
                   {-4, Value::InitialState},
                   {-8, Value::EncodedTable}};
  return Result;
}

std::optional<X86SEH4EpilogueCall>
getCheckedX86SEH4EpilogueCall(const BinaryImage &Image,
                              const ExceptionFunction &Function, va_t CallVA) {
  const auto Entry = getCheckedX86SEH4PrologueCall(Image, Function);
  if (!Entry || CallVA < Entry->Caller.End || CallVA > UINT32_MAX - 5 ||
      CallVA + 5 > Function.CodeRange.End)
    return std::nullopt;
  const auto Call = readImmutablePE32CodeBytes(Image, CallVA, 5, {});
  if (!Call || (*Call)[0] != 0xe8)
    return std::nullopt;
  const va_t Target =
      uint32_t(CallVA + 5 + int64_t(readLE<int32_t>(Call->data() + 1)));
  static constexpr uint8_t Body[] = {0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0,
                                     0,    0,    0,    0x59, 0x5f, 0x5f, 0x5e,
                                     0x5b, 0x8b, 0xe5, 0x5d, 0x51, 0xc3};
  if (Target > UINT32_MAX - (sizeof(Body) - 1) ||
      Function.CodeRange.overlaps({Target, Target + sizeof(Body)}) ||
      Entry->Helper.overlaps({Target, Target + sizeof(Body)}))
    return std::nullopt;
  const auto Code = readImmutablePE32CodeBytes(Image, Target, sizeof(Body), {});
  if (!Code || !std::equal(std::begin(Body), std::end(Body), Code->begin()))
    return std::nullopt;
  return X86SEH4EpilogueCall{{CallVA, CallVA + 5},
                             {Target, Target + sizeof(Body)}};
}

} // namespace neverd::coff_loader
