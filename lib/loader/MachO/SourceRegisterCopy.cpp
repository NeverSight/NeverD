#include "neverd/loader/MachO/SourceRegisterCopy.h"

#include "SourceLocalCall.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCConstantStrings.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd {
namespace {
struct RegisterValue {
  enum Kind { EntryRegister, PageAddress, CompleteAddress } TheKind;
  uint64_t Value;
};

std::optional<SourceRegisterCopy> leaf(const BinaryImage &Image, va_t Entry) {
  if (Entry % 4 || Entry > UINT64_MAX - 68)
    return std::nullopt;
  SourceRegisterCopy Result;
  std::array<RegisterValue, 29> Sources;
  for (unsigned I = 0; I < Sources.size(); ++I)
    Sources[I] = {RegisterValue::EntryRegister, I * 8};
  for (unsigned I = 0; I <= 16; ++I) {
    const auto Bytes = readImmutableCodeBytes(Image, Entry + 4 * I, 4);
    if (!Bytes)
      return std::nullopt;
    const auto Word = llvm::support::endian::read32le(Bytes->data());
    Result.LeafWords.push_back(Word);
    if (Word == 0xd65f03c0) {
      if (!I || !sourceLocalLeafRange(Image, Entry, 4 * (I + 1)))
        return std::nullopt;
      for (unsigned R = 0; R < Sources.size(); ++R) {
        const auto &Source = Sources[R];
        if (Source.TheKind == RegisterValue::PageAddress)
          return std::nullopt;
        if (Source.TheKind == RegisterValue::EntryRegister) {
          if (Source.Value != R * 8)
            Result.Registers.emplace(R * 8, SourceEntryRegister{Source.Value});
          continue;
        }
        const auto String = readObjCConstantString(Image, Source.Value);
        if (!String)
          return std::nullopt;
        Result.Registers.emplace(
            R * 8, SourceConstantStringAddress{Source.Value, String->UTF16,
                                               String->Units,
                                               String->ContentsAddress});
      }
      // Identity-only helpers provide no useful first-stage projection.
      return Result.Registers.empty() ? std::nullopt : std::optional(Result);
    }
    // ORR Xd, XZR, Xm, LSL #0, the full-width MOV alias. Exclude the
    // platform, frame, link and zero registers in both operand positions.
    const unsigned Dst = Word & 31;
    auto Allowed = [](unsigned R) { return R <= 28 && R != 18; };
    if (I == 16 || !Allowed(Dst))
      return std::nullopt;
    if ((Word & 0xffffffe0) == 0xf90003e0) {
      // STR Xn,[SP,#0], with one fresh complete object value at this point.
      // The loader does not know whether the caller owns this stack slot.
      if (Result.StackStore ||
          Sources[Dst].TheKind != RegisterValue::CompleteAddress)
        return std::nullopt;
      const auto String = readObjCConstantString(Image, Sources[Dst].Value);
      if (!String)
        return std::nullopt;
      Result.StackStore =
          SourceStackConstantStore{I,
                                   Word,
                                   {Sources[Dst].Value, String->UTF16,
                                    String->Units, String->ContentsAddress}};
    } else if ((Word & 0xffe0ffe0) == 0xaa0003e0) {
      const unsigned Src = (Word >> 16) & 31;
      if (!Allowed(Src))
        return std::nullopt;
      Sources[Dst] = Sources[Src];
    } else if ((Word & 0x9f000000) == 0x90000000) {
      const uint64_t Page = (Entry + 4 * I) & ~uint64_t(0xfff);
      const uint32_t Imm = ((Word >> 29) & 3) | ((Word >> 3) & 0x1ffffc);
      const int64_t Delta =
          (int64_t(Imm) - ((Imm & 0x100000) ? 0x200000 : 0)) * 4096;
      if ((Delta < 0 && Page < uint64_t(-Delta)) ||
          (Delta >= 0 && Page > UINT64_MAX - uint64_t(Delta)))
        return std::nullopt;
      Sources[Dst] = {RegisterValue::PageAddress, Page + Delta};
    } else if ((Word & 0xffc00000) == 0x91000000) {
      // ADD Xd, Xn, #imm12, LSL #0. Entry-register arithmetic and ADD of
      // already completed addresses do not acquire object provenance.
      const unsigned Src = (Word >> 5) & 31;
      const uint64_t Offset = (Word >> 10) & 0xfff;
      if (!Allowed(Src) || Sources[Src].TheKind != RegisterValue::PageAddress ||
          Sources[Src].Value > UINT64_MAX - Offset)
        return std::nullopt;
      Sources[Dst] = {RegisterValue::CompleteAddress,
                      Sources[Src].Value + Offset};
    } else {
      return std::nullopt;
    }
  }
  return std::nullopt;
}
} // namespace

std::optional<SourceRegisterCopy>
sourceRegisterCopyLeafEffects(const BinaryImage &Image, va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable)
    return std::nullopt;
  return leaf(Image, Entry);
}

std::optional<SourceRegisterValues>
sourceRegisterCopyLeafRegisters(const BinaryImage &Image, va_t Entry) {
  const auto Proof = sourceRegisterCopyLeafEffects(Image, Entry);
  // A register-only query must not hide a caller-frame memory effect.
  return Proof && !Proof->StackStore ? std::optional(Proof->Registers)
                                     : std::nullopt;
}

SourceRegisterCopies sourceRegisterCopies(const BinaryImage &Image,
                                          const LowFunc &Caller) {
  SourceRegisterCopies Result;
  std::map<va_t, std::optional<SourceRegisterCopy>> Leaves;
  for (const auto &[Key, Word] : sourceLocalCalls(Image, Caller)) {
    auto [Found, Inserted] = Leaves.try_emplace(*Key.StaticTarget);
    if (Inserted)
      Found->second = leaf(Image, *Key.StaticTarget);
    if (!Found->second)
      continue;
    auto Proof = *Found->second;
    Proof.Caller = Caller.Entry;
    Proof.Site = Key;
    Proof.CallWord = Word;
    Result.emplace(Key, std::move(Proof));
  }
  return Result;
}

bool validateSourceRegisterCopies(const BinaryImage &Image,
                                  const LowFunc &Caller,
                                  const SourceRegisterCopies &Receipts) {
  if (Receipts.empty())
    return true;
  const auto Current = sourceRegisterCopies(Image, Caller);
  for (const auto &[Site, Receipt] : Receipts) {
    const auto Found = Current.find(Site);
    if (Found == Current.end() || Found->second != Receipt)
      return false;
  }
  return true;
}
} // namespace neverd
