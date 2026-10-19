//===- DriverImportPublication.cpp - Bootstrap IAT publication
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bootstrap IAT publication.
///

#include "DriverImportPublication.h"

#include "DriverSessionState.h"
#include "KernelModuleImage.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::emulation {
namespace {
class ImportPublication final : public ModulePublication {
public:
  ImportPublication(std::unique_ptr<ModulePublication> Model,
                    llvm::ArrayRef<KernelImportPatch> Imports, bool &Linked)
      : Model(std::move(Model)), Linked(Linked) {
    Storage.resize(Imports.size());
    Patches.reserve(this->Model->patches().size() + Imports.size());
    for (const auto &P : this->Model->patches())
      Patches.push_back(P);
    for (size_t N = 0; N < Imports.size(); ++N) {
      llvm::support::endian::write64le(Storage[N].data(), Imports[N].Address);
      Patches.push_back({Imports[N].Slot, Storage[N]});
    }
  }
  llvm::ArrayRef<BackingPatch> patches() const override { return Patches; }
  void commit() noexcept override {
    Model->commit();
    Linked = true;
  }

private:
  std::unique_ptr<ModulePublication> Model;
  bool &Linked;
  std::vector<std::array<uint8_t, 8>> Storage;
  std::vector<BackingPatch> Patches;
};
} // namespace
llvm::Expected<std::unique_ptr<ModulePublication>>
prepareDriverImportPublication(DriverSessionState &State,
                               llvm::ArrayRef<ModuleInstance> Preview) {
  auto Model = State.Kernel->prepareModulePublication(Preview);
  if (!Model)
    return Model.takeError();
  if (!State.AllowImportPublication || State.ModuleImportsLinked)
    return std::move(*Model);
  auto Imports = planKernelGuestImports(
      Preview, State.ModuleImports, State.Exports, State.CPU->addressSpace());
  if (!Imports)
    return Imports.takeError();
  return std::make_unique<ImportPublication>(std::move(*Model), *Imports,
                                             State.ModuleImportsLinked);
}
} // namespace neverd::emulation
