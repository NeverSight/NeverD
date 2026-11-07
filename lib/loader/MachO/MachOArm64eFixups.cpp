//===- MachOArm64eFixups.cpp - Shared authenticated fixup words ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Modern and original arm64e chains share one signing and target decoder.
//===----------------------------------------------------------------------===//

#include "MachOArm64eFixups.h"

#include "MachODyldFixups.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::macho_loader::arm64e {

std::optional<PointerWord> decodePointerWord(uint64_t Raw,
                                             const ChainFormat &Format) {
  PointerWord Word;
  Word.Authenticated = kAuthentication.extract(Raw);
  Word.Bind = !Format.Segmented && kBind.extract(Raw);
  Word.Next = (Format.Segmented ? kSegmentedNext : kNext).extract(Raw);
  if (Word.Authenticated) {
    Word.Diversity = kDiversity.extract(Raw);
    Word.AddressDiversity = kAddressDiversity.extract(Raw);
    Word.Key = kKey.extract(Raw);
  }
  if (Format.Segmented) {
    if (!Word.Authenticated && kSegmentedReserved.extract(Raw))
      return std::nullopt;
    Word.SegmentIndex = kSegmentedTargetIndex.extract(Raw);
    Word.Target = kSegmentedTargetOffset.extract(Raw);
  } else if (Word.Bind) {
    Word.Target = kOrdinalStorage.extract(Raw);
    if (Word.Target >> Format.OrdinalBits)
      return std::nullopt;
    if (!Word.Authenticated)
      Word.Addend = kInlineAddend.signExtend(Raw);
  } else {
    Word.Target = Word.Authenticated ? kAuthenticatedTarget.extract(Raw)
                                     : kTarget.extract(Raw) |
                                           (kHigh8.extract(Raw) << kHigh8Shift);
  }
  return Word;
}

} // namespace neverd::macho_loader::arm64e

namespace neverd::macho_loader::detail {
void joinImportSlot(BinaryImage &Img, llvm::StringRef Name,
                    llvm::StringRef Module, va_t SlotVA) {
  for (Import &Imp : Img.Imports)
    if (Imp.Name == Name) {
      if (Imp.Module.empty() && !Module.empty())
        Imp.Module = Module.str();
      if (Imp.IATAddr == 0)
        Imp.IATAddr = SlotVA;
      return;
    }
}

std::optional<Arm64eFixup>
decodeArm64eFixup(uint64_t Raw, const arm64e::ChainFormat &Format, va_t Slot,
                  va_t Base, llvm::ArrayRef<Arm64eImportRecord> Imports,
                  const BinaryImage &Img) {
  const auto Word = arm64e::decodePointerWord(Raw, Format);
  if (!Word)
    return std::nullopt;
  const bool Auth = Word->Authenticated;
  Arm64eFixup F{Slot};
  F.Next = Word->Next;
  if (Auth) {
    F.Authentication.emplace();
    F.Authentication->EncodedValue = Raw;
    F.Authentication->Diversity = Word->Diversity;
    F.Authentication->AddressDiversity = Word->AddressDiversity;
    F.Authentication->Key = Word->Key;
  }
  if (Format.Segmented) {
    // Segment indices name load commands, not a sorted address view.
    // Linked Mach-O images retain those entries, including empty ones.
    const uint32_t Index = Word->SegmentIndex;
    const uint64_t Offset = Word->Target;
    if (Index >= Img.Segments.size())
      return std::nullopt;
    const Segment &Target = Img.Segments[Index];
    if (Offset > Target.Size || Offset > InvalidVA - Target.VA)
      return std::nullopt;
    F.Target = Target.VA + Offset;
    if (Auth)
      F.Authentication->Target = F.Target;
  } else if (Word->Bind) {
    const uint32_t Ordinal = Word->Target;
    if (Ordinal >= Imports.size() || !Imports[Ordinal].Valid)
      return std::nullopt;
    F.Import = &Imports[Ordinal];
    if (!Auth && !Img.isValidImportStorageSlot(Slot, F.Import->Name))
      return std::nullopt;
    if (llvm::AddOverflow(F.Import->Addend, Word->Addend, F.Addend))
      return std::nullopt;
    if (Auth) {
      F.Authentication->Symbol = F.Import->Name;
      F.Authentication->Module = F.Import->Module;
      F.Authentication->Addend = F.Addend;
      F.Authentication->WeakImport = F.Import->WeakImport;
      F.Authentication->LibraryOrdinal = F.Import->LibraryOrdinal;
    }
  } else {
    F.Target = Word->Target;
    if (Auth || Format.Offset) {
      if (F.Target > InvalidVA - Base)
        return std::nullopt;
      F.Target += Base;
    }
    if (Auth)
      F.Authentication->Target = F.Target;
  }
  return F;
}

void publishArm64eFixups(llvm::ArrayRef<Arm64eFixup> Fixups, BinaryImage &Img) {
  for (const auto &F : Fixups) {
    if (F.Authentication) {
      detail::clearLocalPointerClassification(Img, F.Slot);
      Img.MachOAuthenticatedPointers.emplace(F.Slot, *F.Authentication);
      // The recipe owns the symbolic binding too. Publishing an ordinary
      // import-storage proof would let clients replace signing with a raw
      // function or data address.
    } else if (F.Import) {
      if (Img.isValidImportStorageSlot(F.Slot, F.Import->Name)) {
        detail::clearLocalPointerClassification(Img, F.Slot);
        if (Img.recordDyldBindSlot(F.Slot, F.Import->Name, F.Addend,
                                   F.Import->Module, F.Import->WeakImport,
                                   F.Import->LibraryOrdinal))
          joinImportSlot(Img, F.Import->Name, F.Import->DisplayModule, F.Slot);
      }
    } else {
      detail::recordAbsolutePointerSlot(Img, F.Slot, F.Target);
      Img.patchPtr(F.Slot, F.Target);
      Img.MachOResolvedChainedPointerSlots.insert(F.Slot);
    }
  }
}

} // namespace neverd::macho_loader::detail
