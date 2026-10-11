//===- COFFRegistrationEHPointers.cpp - PE32 runtime pointer roles ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separate table-owned pointers from independent ordinary code references.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/COFF/COFFRegistrationEH.h"

namespace neverd::coff_loader {

std::optional<std::pair<va_t, va_t>>
getCheckedX86SafeSEHTablePointer(const BinaryImage &Img) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF)
    return std::nullopt;
  return registration_detail::SafeSEHTable(Img).tablePointer();
}

std::optional<X86RegistrationPointerRoles>
getCheckedX86RegistrationPointerRoles(const BinaryImage &Img) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF ||
      Img.Bits != Bitness::Bits32)
    return std::nullopt;
  size_t Work = Img.ExceptionMetadata.Functions.size();
  if (Work > limits::kMaxRegistrationEHStateWork)
    return std::nullopt;
  X86RegistrationPointerRoles Roles;
  auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork - Work)
      return false;
    Work += Amount;
    return true;
  };
  for (const auto &Function : Img.ExceptionMetadata.Functions) {
    if (Function.Cxx && Function.Registration) {
      const auto &Cxx = *Function.Cxx;
      if (!Charge(Cxx.UnwindMap.size()) || !Charge(Cxx.TryBlocks.size()) ||
          !Charge(Cxx.IPMap.size()) || !Charge(Cxx.ExceptionSpecTypes.size()))
        return std::nullopt;
      for (const auto &Try : Cxx.TryBlocks)
        if (!Charge(Try.Handlers.size()))
          return std::nullopt;
    }
    const auto Sources = getCheckedX86CxxCallbackPointerSources(Img, Function);
    if (!Sources)
      continue;
    if (Sources->size() > limits::kMaxRegistrationEHStateWork - Work)
      return std::nullopt;
    Work += Sources->size();
    for (const auto &[Slot, Target] : *Sources) {
      const auto [It, New] = Roles.Sources.emplace(Slot, Target);
      if (!New && It->second != Target)
        return std::nullopt;
      Roles.RuntimeOnlyPointerTargets.insert(Target);
    }
  }
  // SafeSEH entries are RVAs. Its relocated load-config pointer names the
  // table allocation even when a rewriter places that allocation in RX bytes.
  // Treating this source as a code pointer creates a spurious ordinary root
  // inside the generated parent and contaminates every callback state.
  if (const auto Pointer = getCheckedX86SafeSEHTablePointer(Img)) {
    if (!Charge(1))
      return std::nullopt;
    const auto [Slot, Target] = *Pointer;
    const auto [It, New] = Roles.Sources.emplace(Slot, Target);
    if (!New && It->second != Target)
      return std::nullopt;
    Roles.RuntimeOnlyPointerTargets.insert(Target);
  }
  if (Img.CodePtrRelocSlots.size() > limits::kMaxRegistrationEHStateWork - Work)
    return std::nullopt;
  for (va_t Slot : Img.CodePtrRelocSlots) {
    const uint8_t *Pointer = Img.readVA(Slot, 4);
    if (!Pointer)
      return std::nullopt;
    const va_t Target = readLE<uint32_t>(Pointer);
    const auto Source = Roles.Sources.find(Slot);
    if (Source == Roles.Sources.end() || Source->second != Target)
      Roles.RuntimeOnlyPointerTargets.erase(Target);
  }
  return Roles;
}

} // namespace neverd::coff_loader
