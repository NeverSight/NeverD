//===- WindowsProcessLoader.cpp - Stopped-boundary module operations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessLoader.h"

#include "neverd/emulation/CPU.h"

#include <functional>
#include <set>

namespace neverd::emulation::windows_process {
using namespace value;
llvm::Error Loader::publish(const ModuleLink &Linked) {
  for (auto Ref : Linked.Added) {
    if (!current(P, Ref))
      return failure(text::Lifetime);
    const auto &M = P.Modules[Ref.Index];
    for (const auto &R : M.Loaded.Regions) {
      if (auto E = Memory.map(R.Address, R.Bytes.size(),
                              Read | Write | UserAccessible))
        return E;
      if (auto E = Memory.write(R.Address, R.Bytes))
        return E;
    }
  }
  for (auto Ref : Linked.Added)
    for (const auto &I : P.Modules[Ref.Index].Loaded.Imports)
      if (auto E = Memory.writeInteger(I.Slot, I.Gate, PointerSize))
        return E;
  if (auto E = updateEnvironment(Memory, P, Env, Budget))
    return E;
  for (auto Ref : Linked.Added) {
    auto &M = P.Modules[Ref.Index];
    for (const auto &R : M.Loaded.Regions)
      if (auto E = Memory.protect(R.Address, R.Bytes.size(), R.Permissions))
        return E;
    M.State = ModuleState::Linked;
  }
  return llvm::Error::success();
}
llvm::Expected<std::vector<ModuleRef>> Loader::unreferenced() const {
  std::set<size_t> Retained;
  std::vector<size_t> Work;
  for (size_t I = 0; I < P.Modules.size(); ++I) {
    const auto &M = P.Modules[I];
    if (resident(M) && (M.References || M.Pinned))
      Work.push_back(I);
  }
  while (!Work.empty()) {
    const size_t I = Work.back();
    Work.pop_back();
    if (!Retained.insert(I).second)
      continue;
    for (auto Ref : P.Modules[I].Dependencies) {
      if (!current(P, Ref))
        return failure(text::Lifetime);
      Work.push_back(Ref.Index);
    }
  }
  // Unload owners before their now-unreferenced dependencies. Loader-list
  // order alone is insufficient when a runtime forwarder acquired a target.
  std::set<size_t> Ordered, Visiting;
  std::vector<ModuleRef> Result;
  std::function<llvm::Error(size_t)> Visit = [&](size_t Index) -> llvm::Error {
    if (Retained.contains(Index))
      return llvm::Error::success();
    if (Visiting.contains(Index))
      return failure(text::ModuleCycle + P.Identities[Index].Name);
    if (!Ordered.insert(Index).second)
      return llvm::Error::success();
    Visiting.insert(Index);
    for (auto Ref : P.Modules[Index].Dependencies) {
      if (!current(P, Ref))
        return failure(text::Lifetime);
      if (auto E = Visit(Ref.Index))
        return E;
    }
    Visiting.erase(Index);
    Result.push_back(moduleRef(P, Index));
    return llvm::Error::success();
  };
  for (size_t I : P.LoaderInitializationOrder)
    if (auto E = Visit(I))
      return std::move(E);
  std::reverse(Result.begin(), Result.end());
  return Result;
}
llvm::Expected<Loader::Operation> Loader::begin(const LoaderRequest &Request) {
  auto Result = start(Request);
  if (Result)
    return Result;
  uint32_t Code = 0;
  auto E = llvm::handleErrors(
      Result.takeError(),
      [&](const ModuleLoadError &Failure) { Code = Failure.Code; });
  if (E)
    return std::move(E);
  Operation Out{Request};
  Out.Error = Code;
  return Out;
}
llvm::Expected<Loader::Operation> Loader::start(const LoaderRequest &Request) {
  Operation Out{Request};
  if (Request.Operation == LoaderRequest::Kind::Load) {
    if (findProvider(Request.Name))
      return failure(text::LoaderProvider);
    if (auto I = findModule(P, Request.Name)) {
      auto &M = P.Modules[*I];
      if (M.State != ModuleState::Ready)
        return failure(text::LoaderReentrant);
      if (M.References == UINT64_MAX)
        return failure(text::ModuleBudget);
      if (!M.Pinned)
        ++M.References;
      Out.Value = M.Loaded.Base;
      return Out;
    }
    auto Linked = linkModule(P, Request.Name, Virtual, Budget, &CPU);
    if (!Linked)
      return Linked.takeError();
    if (auto E = publish(*Linked))
      return std::move(E);
    Out.Root = moduleRef(P, Linked->Root);
    Out.Value = P.Modules[Linked->Root].Loaded.Base;
    ++P.Modules[Linked->Root].References;
    Out.Acquired = Out.Initializing = true;
    Out.Edges = std::move(Linked->Edges);
    Out.Created = Linked->Added;
    Out.Notifications =
        std::make_unique<Lifetime>(P, Lifetime::Mode::Load, Linked->Attach);
    return Out;
  }
  auto Found = llvm::find_if(P.Modules, [&](const auto &M) {
    return resident(M) && M.Loaded.Base == Request.Module;
  });
  if (Found == P.Modules.end()) {
    Out.Error = ErrorModuleNotFound;
    return Out;
  }
  const size_t Index = size_t(Found - P.Modules.begin());
  Out.Root = moduleRef(P, Index);
  if (Request.Operation == LoaderRequest::Kind::Free) {
    if (Found->Pinned) {
      Out.Value = 1;
      return Out;
    }
    if (!Index || Found->State != ModuleState::Ready || !Found->References)
      return failure(text::LoaderReentrant);
    --Found->References;
    auto Retired = unreferenced();
    if (!Retired)
      return Retired.takeError();
    for (auto Ref : *Retired)
      if (P.Modules[Ref.Index].State != ModuleState::Ready)
        return failure(text::LoaderReentrant);
    Out.Value = 1;
    Out.Unload = std::move(*Retired);
    if (!Out.Unload.empty()) {
      Out.Notifications =
          std::make_unique<Lifetime>(P, Lifetime::Mode::Unload, Out.Unload);
      if (auto E = updateEnvironment(Memory, P, Env, Budget))
        return std::move(E);
    }
    return Out;
  }
  std::vector<ModuleRef> Attach;
  auto Forward = [&](size_t Owner,
                     llvm::StringRef Name) -> llvm::Expected<size_t> {
    auto Target = findModule(P, Name);
    if (!Target) {
      auto Linked = linkModule(P, Name, Virtual, Budget, &CPU);
      if (!Linked)
        return Linked.takeError();
      if (auto E = publish(*Linked))
        return std::move(E);
      Attach.insert(Attach.end(), Linked->Attach.begin(), Linked->Attach.end());
      Out.Created.insert(Out.Created.end(), Linked->Added.begin(),
                         Linked->Added.end());
      Out.Edges.insert(Out.Edges.end(), Linked->Edges.begin(),
                       Linked->Edges.end());
      Target = Linked->Root;
    }
    auto Ref = moduleRef(P, *Target);
    if (P.Modules[*Target].State != ModuleState::Ready &&
        !llvm::is_contained(Out.Created, Ref))
      return failure(text::LoaderReentrant);
    if (Owner != *Target &&
        !llvm::is_contained(P.Modules[Owner].Dependencies, Ref)) {
      P.Modules[Owner].Dependencies.push_back(Ref);
      Out.Edges.emplace_back(moduleRef(P, Owner), Ref);
    }
    return *Target;
  };
  auto Resolved = resolveExport(P, Index, Request.Name, Request.Ordinal, Budget,
                                &CPU, Forward);
  if (!Resolved) {
    auto E =
        llvm::handleErrors(Resolved.takeError(), [&](const ModuleLoadError &F) {
          Out.Error = F.Code;
        });
    if (E)
      return std::move(E);
  } else {
    Out.Value = Resolved->Address.value_or(0);
    Out.Error = Resolved->Error;
  }
  if (!Attach.empty()) {
    Out.Initializing = true;
    Out.Notifications =
        std::make_unique<Lifetime>(P, Lifetime::Mode::Load, Attach);
  }
  return Out;
}
llvm::Error Loader::finishUnload(Operation &Op) {
  if (auto E = releaseModuleEnvironment(Memory, P, Env, Op.Unload, Budget))
    return E;
  for (auto Ref : Op.Unload)
    if (auto E = retireModule(P, Ref, Virtual))
      return E;
  return updateEnvironment(Memory, P, Env, Budget);
}
llvm::Error Loader::complete(Operation &Op) {
  auto Failed = Op.Notifications->failedModule();
  Op.Notifications.reset();
  if (!Op.Initializing)
    return finishUnload(Op);
  Op.Initializing = false;
  if (Failed) {
    if (Op.Acquired && Op.Root && current(P, *Op.Root))
      --P.Modules[Op.Root->Index].References;
    for (const auto &[Owner, Target] : Op.Edges)
      if (current(P, Owner) && !llvm::is_contained(Op.Created, Owner))
        std::erase(P.Modules[Owner.Index].Dependencies, Target);
    auto Retired = unreferenced();
    if (!Retired)
      return Retired.takeError();
    Op.Value = 0;
    Op.Error = uint32_t(Op.Request.Operation == LoaderRequest::Kind::Export
                            ? ErrorProcedureNotFound
                            : ErrorDLLInitFailed);
    Op.Unload = std::move(*Retired);
    Op.Notifications = std::make_unique<Lifetime>(P, Lifetime::Mode::Rollback,
                                                  Op.Unload, Failed);
    return updateEnvironment(Memory, P, Env, Budget);
  }
  if (Op.Request.Operation == LoaderRequest::Kind::Export) {
    if (!Op.Root || !current(P, *Op.Root))
      return failure(text::Lifetime);
    auto Resident = [&](size_t,
                        llvm::StringRef Name) -> llvm::Expected<size_t> {
      auto I = findModule(P, Name);
      if (!I)
        return llvm::make_error<ModuleLoadError>(uint32_t(ErrorModuleNotFound));
      return *I;
    };
    auto Resolved = resolveExport(P, Op.Root->Index, Op.Request.Name,
                                  Op.Request.Ordinal, Budget, &CPU, Resident);
    if (!Resolved) {
      auto E = llvm::handleErrors(
          Resolved.takeError(),
          [&](const ModuleLoadError &F) { Op.Error = F.Code; });
      if (E)
        return E;
      Op.Value = 0;
    } else {
      Op.Value = Resolved->Address.value_or(0);
      Op.Error = Resolved->Error;
    }
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation::windows_process
