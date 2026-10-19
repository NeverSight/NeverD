//===- DriverBootstrapModules.cpp - Preexisting kernel module ownership
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preexisting kernel module ownership.
///

#include "DriverBootstrapModules.h"

#include "DriverSessionState.h"

#include "neverd/emulation/ModuleBinding.h"
#include "neverd/loader/COFF/PEProgramExports.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error invalid() {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "bootstrap module ownership changed");
}
ModuleInstance instance(DriverSessionState &S, bool Reference) {
  ModuleInstance I;
  I.Source = Reference ? S.Inputs.Kernel : S.Inputs.Image;
  I.Load = Reference ? S.Inputs.KernelLoad : S.Inputs.ImageLoad;
  I.Base = Reference ? S.Reference->base() : S.Image->Base;
  I.Size = Reference ? S.Reference->size() : S.Image->Size;
  // The reference is an observation, never an entry initialization request.
  I.Entry = Reference ? 0 : S.Image->Entry;
  const auto &ID = Reference ? S.Inputs.KernelID : S.Inputs.ImageID;
  I.Binding = {Reference ? KernelReferenceBootstrapID : DriverBootstrapID,
               ID.empty() ? (Reference ? "@kernel-reference" : "@driver-image")
                          : ID,
               Reference ? "ntoskrnl.exe" : S.Options.ServiceName + ".sys",
               "kernel",
               S.CPU->addressSpace(),
               I.Base,
               I.Size};
  I.EnvironmentOwned = true;
  I.Role = Reference ? "kernel-reference" : "driver";
  return I;
}
} // namespace
llvm::Error adoptDriverBootstrapModules(DriverSessionState &S,
                                        ModuleBindings &Modules) {
  if (!S.Inputs.Image || (S.Reference && !S.Inputs.Kernel))
    return invalid();
  std::vector<ModuleInstance> Roots{instance(S, false)};
  if (S.Reference) {
    auto I = instance(S, true);
    auto Exports = readPEProgramExports(I.Source->bytes());
    if (!Exports)
      return Exports.takeError();
    for (const auto &E : Exports->Entries) {
      if (E.Kind == PEExportKind::Hole)
        continue;
      for (const auto &Name : E.Names) {
        if (const auto Override = S.Options.KernelExports.find(Name);
            Override != S.Options.KernelExports.end() && !Override->second)
          continue;
        // This table belongs to the authenticated kernel image. Name-only
        // dynamic lookup can also see a distinct HAL export with this name.
        auto Address = S.Exports.findImport({0, I.Binding.GuestName, Name});
        if (!Address)
          return Address.takeError();
        if (!*Address)
          continue; // Explicit namespace absence remains absent.
        const auto *Bound = S.Exports.lookup(**Address);
        if (!Bound)
          return invalid();
        using K = KernelExportRegistry::ExportKind;
        const bool Function = Bound->Kind == K::ModuleExport ||
                              Bound->Kind == K::HalDispatchFunction;
        I.Exports.push_back(
            {Name, E.Ordinal, **Address,
             Function ? modules::SymbolKind::Function
                      : modules::SymbolKind::Data,
             **Address < I.Base || **Address - I.Base >= I.Size});
      }
    }
    Roots.push_back(std::move(I));
  }
  return Modules.adopt(
      Roots, [&S, &Roots](llvm::ArrayRef<ModuleInstance> Batch) -> llvm::Error {
        if (Batch.size() != (S.Reference ? 2u : 1u))
          return invalid();
        for (size_t N = 0; N < Batch.size(); ++N) {
          const auto &Expected = Roots[N];
          const auto &I = Batch[N];
          if (I.Binding != Expected.Binding || I.Source != Expected.Source ||
              I.Load != Expected.Load || I.Base != Expected.Base ||
              I.Size != Expected.Size || I.Entry != Expected.Entry ||
              !I.EnvironmentOwned || I.Role != Expected.Role ||
              !std::equal(I.Exports.begin(), I.Exports.end(),
                          Expected.Exports.begin(), Expected.Exports.end(),
                          [](const auto &A, const auto &B) {
                            return A.Name == B.Name && A.Ordinal == B.Ordinal &&
                                   A.Address == B.Address && A.Kind == B.Kind &&
                                   A.Forwarded == B.Forwarded;
                          }))
            return invalid();
        }
        return llvm::Error::success();
      });
}
} // namespace neverd::emulation
