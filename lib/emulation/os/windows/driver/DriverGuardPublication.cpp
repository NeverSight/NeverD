//===- DriverGuardPublication.cpp - Transactional Native CFG policy
//--------===//
///
/// \file
/// Transactional Native CFG policy.
///

#include "DriverGuardPublication.h"

#include "DriverSessionState.h"
#include "KernelModuleImage.h"

#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
namespace neverd::emulation {
namespace {
llvm::Error invalid(llvm::StringRef Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
class GuardPublication final : public ModulePublication {
public:
  GuardPublication(DriverSessionState &State,
                   std::unique_ptr<ModulePublication> Child,
                   std::unique_ptr<GuardControlFlow> Policy,
                   std::set<std::string> IDs)
      : State(State), Child(std::move(Child)), Policy(std::move(Policy)),
        IDs(std::move(IDs)) {}
  llvm::ArrayRef<BackingPatch> patches() const override {
    return Child->patches();
  }
  void commit() noexcept override {
    Child->commit();
    State.Guard->swap(*Policy);
    State.GuardModuleIDs.swap(IDs);
  }

private:
  DriverSessionState &State;
  std::unique_ptr<ModulePublication> Child;
  std::unique_ptr<GuardControlFlow> Policy;
  std::set<std::string> IDs;
};
} // namespace
llvm::Expected<std::unique_ptr<ModulePublication>>
prepareDriverGuardPublication(DriverSessionState &State,
                              llvm::ArrayRef<ModuleInstance> Preview,
                              std::unique_ptr<ModulePublication> Child) {
  auto IDs = State.GuardModuleIDs;
  std::unique_ptr<GuardControlFlow> Policy;
  for (const auto &I : Preview) {
    if (!I.mapped() || I.EnvironmentOwned || IDs.contains(I.Binding.ID))
      continue;
    if (!I.RuntimeData ||
        I.RuntimeData->typeID() != &KernelModuleRuntimeData::ID)
      return invalid("native module has no retained CFG ownership");
    auto Data =
        std::static_pointer_cast<const KernelModuleRuntimeData>(I.RuntimeData);
    if (Data->Base != I.Base || Data->Size != I.Size ||
        Data->SourceDigest != I.Source->imageDigest())
      return invalid("native CFG owner does not match its source and instance");
    if (!Policy)
      Policy = std::make_unique<GuardControlFlow>(*State.Guard);
    if (auto E = Policy->addImage(
            Data->Guard, Data->CompatibilityExports,
            [S = &State, Data]() -> llvm::Error {
              std::array<uint8_t, 4096> Bytes;
              for (const auto &W : Data->GuardWitnesses) {
                size_t Offset = 0;
                while (Offset < W.Bytes.size()) {
                  if (!S->Budget->remainingMicroseconds())
                    return invalid("native CFG metadata validation timed out");
                  const auto N =
                      std::min(Bytes.size(), W.Bytes.size() - Offset);
                  if (auto E = S->Kernel->validateGuestAccess(
                          W.Address + Offset, N, false))
                    return E;
                  if (auto E = S->CPU->read(
                          W.Address + Offset,
                          llvm::MutableArrayRef(Bytes).take_front(N)))
                    return E;
                  if (!std::equal(Bytes.begin(), Bytes.begin() + N,
                                  W.Bytes.begin() + Offset))
                    return invalid(
                        "native CFG metadata or helper binding changed");
                  Offset += N;
                }
              }
              return llvm::Error::success();
            }))
      return std::move(E);
    IDs.insert(I.Binding.ID);
  }
  if (!Policy)
    return Child;
  return std::make_unique<GuardPublication>(State, std::move(Child),
                                            std::move(Policy), std::move(IDs));
}
} // namespace neverd::emulation
