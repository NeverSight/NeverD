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
  const bool Auth = arm64e::kAuthentication.extract(Raw);
  const bool Bind = !Format.Segmented && arm64e::kBind.extract(Raw);
  const uint32_t Next =
      (Format.Segmented ? arm64e::kSegmentedNext : arm64e::kNext).extract(Raw);
  Arm64eFixup F{Slot};
  F.Next = Next;
  if (Auth) {
    F.Authentication.emplace();
    F.Authentication->EncodedValue = Raw;
    F.Authentication->Diversity = arm64e::kDiversity.extract(Raw);
    F.Authentication->AddressDiversity = arm64e::kAddressDiversity.extract(Raw);
    F.Authentication->Key = arm64e::kKey.extract(Raw);
  }
  if (Format.Segmented) {
    // Segment indices name load commands, not a sorted address view.
    // Linked Mach-O images retain those entries, including empty ones.
    const uint32_t Index = arm64e::kSegmentedTargetIndex.extract(Raw);
    const uint64_t Offset = arm64e::kSegmentedTargetOffset.extract(Raw);
    if ((!Auth && arm64e::kSegmentedReserved.extract(Raw)) ||
        Index >= Img.Segments.size())
      return std::nullopt;
    const Segment &Target = Img.Segments[Index];
    if (Offset > Target.Size || Offset > InvalidVA - Target.VA)
      return std::nullopt;
    F.Target = Target.VA + Offset;
    if (Auth)
      F.Authentication->Target = F.Target;
  } else if (Bind) {
    const uint32_t Ordinal =
        Raw & llvm::maskTrailingOnes<uint32_t>(Format.OrdinalBits);
    if ((arm64e::kOrdinalStorage.extract(Raw) >> Format.OrdinalBits) != 0 ||
        Ordinal >= Imports.size() || !Imports[Ordinal].Valid)
      return std::nullopt;
    F.Import = &Imports[Ordinal];
    if (!Auth && !Img.isValidImportStorageSlot(Slot, F.Import->Name))
      return std::nullopt;
    const int64_t InlineAddend =
        Auth ? 0 : arm64e::kInlineAddend.signExtend(Raw);
    if (llvm::AddOverflow(F.Import->Addend, InlineAddend, F.Addend))
      return std::nullopt;
    if (Auth) {
      F.Authentication->Symbol = F.Import->Name;
      F.Authentication->Module = F.Import->Module;
      F.Authentication->Addend = F.Addend;
      F.Authentication->WeakImport = F.Import->WeakImport;
      F.Authentication->LibraryOrdinal = F.Import->LibraryOrdinal;
    }
  } else {
    F.Target = Auth ? arm64e::kAuthenticatedTarget.extract(Raw)
                    : arm64e::kTarget.extract(Raw) |
                          (arm64e::kHigh8.extract(Raw) << arm64e::kHigh8Shift);
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
