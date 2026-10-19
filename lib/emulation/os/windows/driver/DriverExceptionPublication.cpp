//===- DriverExceptionPublication.cpp - Native unwind ownership
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native unwind ownership.
///

#include "DriverExceptionPublication.h"

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
class ExceptionPublication final : public ModulePublication {
public:
  ExceptionPublication(DriverSessionState &State,
                       std::unique_ptr<ModulePublication> Child,
                       std::unique_ptr<X64SEH> Planner,
                       std::set<std::string> IDs)
      : State(State), Child(std::move(Child)), Planner(std::move(Planner)),
        IDs(std::move(IDs)) {}
  llvm::ArrayRef<BackingPatch> patches() const override {
    return Child->patches();
  }
  void commit() noexcept override {
    Child->commit();
    State.Exceptions.swap(Planner);
    State.ExceptionModuleIDs.swap(IDs);
  }

private:
  DriverSessionState &State;
  std::unique_ptr<ModulePublication> Child;
  std::unique_ptr<X64SEH> Planner;
  std::set<std::string> IDs;
};
} // namespace
llvm::Expected<std::unique_ptr<ModulePublication>>
prepareDriverExceptionPublication(DriverSessionState &State,
                                  llvm::ArrayRef<ModuleInstance> Preview,
                                  std::unique_ptr<ModulePublication> Child) {
  auto IDs = State.ExceptionModuleIDs;
  std::vector<X64SEH::Image> Added;
  for (const auto &I : Preview) {
    if (!I.mapped() || I.EnvironmentOwned || IDs.contains(I.Binding.ID))
      continue;
    if (!I.RuntimeData ||
        I.RuntimeData->typeID() != &KernelModuleRuntimeData::ID)
      return invalid("native module has no retained unwind ownership");
    auto Data =
        std::static_pointer_cast<const KernelModuleRuntimeData>(I.RuntimeData);
    if (!Data->Unwind.Metadata || Data->Base != I.Base ||
        Data->Size != I.Size || Data->SourceDigest != I.Source->imageDigest() ||
        Data->Unwind.PreferredBase != I.Source->image().Base)
      return invalid(
          "native unwind owner does not match its source and instance");
    Added.push_back(
        {Data->Unwind.Metadata.get(), Data->Unwind.PreferredBase, I.Base,
         I.Size,
         [S = &State, Data]() -> llvm::Expected<uint64_t> {
           if (!Data->Unwind.CookieAddress)
             return invalid(
                 "native GS dispatch requires its image security cookie");
           if (auto E = S->Kernel->validateGuestAccess(
                   Data->Unwind.CookieAddress, 8, false))
             return std::move(E);
           return S->CPU->readInteger(Data->Unwind.CookieAddress, 8);
         },
         [S = &State, Data]() -> llvm::Error {
           std::array<uint8_t, 4096> Bytes;
           for (const auto &W : Data->Witnesses) {
             size_t Offset = 0;
             while (Offset < W.Bytes.size()) {
               if (!S->Budget->remainingMicroseconds())
                 return invalid(
                     "native exception metadata validation timed out");
               const auto N = std::min(Bytes.size(), W.Bytes.size() - Offset);
               if (auto E = S->Kernel->validateGuestAccess(W.Address + Offset,
                                                           N, false))
                 return E;
               if (auto E =
                       S->CPU->read(W.Address + Offset,
                                    llvm::MutableArrayRef(Bytes).take_front(N)))
                 return E;
               if (!std::equal(Bytes.begin(), Bytes.begin() + N,
                               W.Bytes.begin() + Offset))
                 return invalid("native exception metadata or personality "
                                "binding changed");
               Offset += N;
             }
           }
           return llvm::Error::success();
         }});
    IDs.insert(I.Binding.ID);
  }
  if (Added.empty())
    return Child;
  auto Planner = State.Exceptions->withAdditionalImages(Added);
  if (!Planner)
    return Planner.takeError();
  return std::make_unique<ExceptionPublication>(
      State, std::move(Child), std::move(*Planner), std::move(IDs));
}
} // namespace neverd::emulation
