//===- COFFRegistrationTableProof.h - Shared PE32 table ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_COFFREGISTRATIONTABLEPROOF_H
#define NEVERD_COFFREGISTRATIONTABLEPROOF_H

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Function.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/Support/Errc.h"

#include <limits>

namespace neverd::coff_registration {

inline llvm::Expected<uint64_t> ownerVA(const llvm::Function &Function,
                                        const CompiledImage &Compiled) {
  const llvm::mc_rewrite::RewriteSourceFunctionOwner *Found = nullptr;
  for (const auto &Owner : Compiled.SourceFunctionOwners)
    if (Owner.SourceFunction == Function.getName()) {
      if (Found ||
          Owner.Kind !=
              llvm::mc_rewrite::RewriteSourceFunctionOwnerKind::FunctionEntry ||
          Owner.IsPrivate != Function.hasLocalLinkage())
        return llvm::createStringError(
            llvm::errc::invalid_argument,
            "generated source function owner is ambiguous");
      Found = &Owner;
    }
  if (!Found || !Compiled.FunctionOwnerAddrs.count(Found->OwnerSymbol) ||
      Compiled.FunctionOwnerAddrs.at(Found->OwnerSymbol) != Found->OwnerVA)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "generated function has no exact compiler owner");
  return Found->OwnerVA;
}

inline const CompiledSection *
sectionAt(const CompiledImage &Compiled, uint64_t VA, uint64_t Size,
          llvm::mc_rewrite::RewriteSectionKind Kind) {
  const CompiledSection *Found = nullptr;
  if (!Size || VA > std::numeric_limits<uint64_t>::max() - Size)
    return nullptr;
  for (const auto &Section : Compiled.Sections) {
    if (!Section.IsAllocated || !Section.IsInImage || !Section.Size)
      continue;
    if (Section.VA > std::numeric_limits<uint64_t>::max() - Section.Size)
      return nullptr;
    if (VA >= Section.VA + Section.Size || Section.VA >= VA + Size)
      continue;
    if (VA < Section.VA ||
        !rangeInBounds(VA - Section.VA, Size, Section.Size) || Found ||
        Section.Kind != Kind || Section.VA < Compiled.BaseVA ||
        Section.Offset != Section.VA - Compiled.BaseVA ||
        !rangeInBounds(Section.Offset, Section.Size, Compiled.Bytes.size()))
      return nullptr;
    Found = &Section;
  }
  return Found;
}

inline bool absolutePointer(const CompiledFixupReference &Fixup) {
  return !Fixup.IsPCRel && Fixup.Kind == llvm::FK_Data_4 &&
         Fixup.BitWidth == 32 && Fixup.Specifier == 0 && Fixup.IsResolved &&
         !Fixup.Symbol.empty() && Fixup.SubtractSymbol.empty();
}

/// A literal table field must remain literal after PE rebasing as well.
inline bool hasNoFixup(const CompiledSection &Section, uint64_t VA,
                       uint64_t Size) {
  if (VA < Section.VA || !rangeInBounds(VA - Section.VA, Size, Section.Size))
    return false;
  for (const auto &Fixup : Section.FixupReferences) {
    const uint64_t Width =
        Fixup.BitWidth ? (uint64_t(Fixup.BitWidth) + 7) / 8 : 8;
    if (!rangeInBounds(Fixup.Offset, Width, Section.Size) ||
        (Fixup.Offset < VA - Section.VA + Size &&
         VA - Section.VA < Fixup.Offset + Width))
      return false;
  }
  return true;
}

inline bool exactPointerFixup(const CompiledSection &Section, uint64_t VA,
                              llvm::StringRef Symbol, uint64_t Target) {
  const CompiledFixupReference *Found = nullptr;
  if (VA < Section.VA || !rangeInBounds(VA - Section.VA, 4, Section.Size))
    return false;
  for (const auto &Fixup : Section.FixupReferences) {
    const uint64_t Width =
        Fixup.BitWidth ? (uint64_t(Fixup.BitWidth) + 7) / 8 : 8;
    if (!rangeInBounds(Fixup.Offset, Width, Section.Size))
      return false;
    if (Fixup.Offset < VA - Section.VA + 4 &&
        VA - Section.VA < Fixup.Offset + Width) {
      if (Fixup.Offset != VA - Section.VA || Found || !absolutePointer(Fixup) ||
          Fixup.Symbol != Symbol || Fixup.Addend ||
          Fixup.ResolvedValue != Target)
        return false;
      Found = &Fixup;
    }
  }
  return Found != nullptr;
}

} // namespace neverd::coff_registration
#endif
