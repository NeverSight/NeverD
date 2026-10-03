//===- WindowsProcessModules.cpp - Explicit acyclic PE module linking ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessModules.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <set>

namespace neverd::emulation::windows_process {
using namespace value;
llvm::Expected<std::string> moduleName(llvm::StringRef Name) {
  if (Name.size() >= windows_process_limits::NameBytes ||
      Name.size() <= sizeof(text::DLLExtension) - 1 ||
      !Name.ends_with_insensitive(text::DLLExtension) ||
      !llvm::all_of(Name,
                    [](char C) {
                      return llvm::isAlnum(C) || C == '_' || C == '-' ||
                             C == '.' || C == ' ';
                    }) ||
      Name.front() == ' ' || Name.front() == '.')
    return failure(text::ModuleName + Name);
  return Name.lower();
}

char ModuleLoadError::ID;
void ModuleLoadError::log(llvm::raw_ostream &OS) const {
  OS << text::ModuleLoadFailed
     << (Code == ErrorModuleNotFound ? text::ModuleMissing : text::ModuleExport)
     << Code;
}
std::error_code ModuleLoadError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}
bool resident(const Module &M) { return M.State != ModuleState::Retired; }
bool current(const Program &P, ModuleRef Ref) {
  return Ref.Index < P.Modules.size() && resident(P.Modules[Ref.Index]) &&
         P.Modules[Ref.Index].Generation == Ref.Generation;
}
ModuleRef moduleRef(const Program &P, size_t Index) {
  return {Index, P.Modules[Index].Generation};
}
std::optional<size_t> findModule(const Program &P, llvm::StringRef Name) {
  auto I = P.Slots.find(Name.lower());
  if (I == P.Slots.end() || !resident(P.Modules[I->second]))
    return std::nullopt;
  return I->second;
}
llvm::Error retireModule(Program &P, ModuleRef Ref, VirtualMemory &Memory) {
  if (!current(P, Ref) || !Ref.Index)
    return failure(text::Lifetime);
  auto &M = P.Modules[Ref.Index];
  if (auto E = Memory.releaseImage(M.Loaded.Base))
    return E;
  M = Module{};
  auto &Identity = P.Identities[Ref.Index];
  Identity.Base = Identity.Size = Identity.Entry = 0;
  std::erase(P.LoaderInitializationOrder, Ref.Index);
  return llvm::Error::success();
}
namespace {
class Linker final {
public:
  Linker(Program &P, VirtualMemory &Memory, const ExecutionBudget &Budget,
         ExecutionBackend *CPU = nullptr)
      : P(P), Memory(Memory), Budget(Budget), CPU(CPU) {}
  llvm::Expected<ModuleLink> run(llvm::StringRef Name,
                                 const std::filesystem::path *Main = nullptr) {
    auto Result = link(Name, Main);
    if (Result)
      return Result;
    auto E = Result.takeError();
    for (const auto &[Owner, Target] : Change.Edges)
      if (current(P, Owner))
        std::erase(P.Modules[Owner.Index].Dependencies, Target);
    for (auto I = Change.Added.rbegin(); I != Change.Added.rend(); ++I) {
      // Main-image preparation errors terminate construction. The virtual
      // owner and its address space are destroyed with the failed process.
      if (!I->Index)
        continue;
      E = llvm::joinErrors(std::move(E), retireModule(P, *I, Memory));
    }
    return std::move(E);
  }

private:
  llvm::Error checkTime() const {
    return Budget.remainingMicroseconds() ? llvm::Error::success()
                                          : failure(text::ModuleTimeout);
  }
  llvm::Expected<size_t> visit(llvm::StringRef Name,
                               const std::filesystem::path *Main = nullptr) {
    if (auto E = checkTime())
      return std::move(E);
    if (auto I = findModule(P, Name)) {
      if (Loading.contains(*I))
        return failure(text::ModuleCycle + Name);
      if (P.Modules[*I].State != ModuleState::Ready &&
          !llvm::is_contained(Change.Added, moduleRef(P, *I)))
        return failure(text::LoaderReentrant);
      return *I;
    }
    const bool DLL = Main == nullptr;
    const auto Input = P.Catalogue.find(Name.str());
    if (DLL && Input == P.Catalogue.end())
      return llvm::make_error<ModuleLoadError>(uint32_t(ErrorModuleNotFound));
    const auto &File = DLL ? Input->second : *Main;
    auto Image = loadProgramImage(File, P.Reads, DLL);
    if (!Image)
      return Image.takeError();
    if (auto E = checkTime())
      return std::move(E);
    if (DLL && Image->Architecture != P.Modules.front().Loaded.Architecture)
      return failure(text::ModuleISA);
    auto Base = Memory.reserveImage(Image->Base, Image->Size,
                                    DLL && Image->Relocatable);
    if (!Base)
      return Base.takeError();
    if (auto E = relocateImage(*Image, *Base, P.Reads))
      return llvm::joinErrors(std::move(E), Memory.releaseImage(*Base));
    auto Slot = P.Slots.find(Name.lower());
    size_t Index;
    if (Slot == P.Slots.end()) {
      if (P.Modules.size() > windows_process_limits::Modules)
        return llvm::joinErrors(failure(text::ModuleBudget),
                                Memory.releaseImage(*Base));
      Index = P.Modules.size();
      P.Slots.emplace(Name.lower(), Index);
      P.Modules.emplace_back();
      P.Identities.push_back({Name.str(), 0, 0, 0});
    } else
      Index = Slot->second;
    auto &M = P.Modules[Index];
    M.Loaded = std::move(*Image);
    M.Generation = P.NextGeneration++;
    M.State = ModuleState::Prepared;
    P.Identities[Index] = {Name.str(), M.Loaded.Base, M.Loaded.Size,
                           M.Loaded.Entry};
    Change.Added.push_back(moduleRef(P, Index));
    Loading.insert(Index);
    for (size_t I = 0; I < M.Loaded.Exports.Entries.size(); ++I) {
      const auto &E = M.Loaded.Exports.Entries[I];
      M.Ordinals.emplace(E.Ordinal, I);
      for (const auto &ExportName : E.Names)
        M.Names.emplace(ExportName, I);
    }
    auto Ranges = M.Loaded.Exports.Metadata;
    for (const auto &R : M.Loaded.Regions)
      if (R.Address == M.Loaded.Base)
        Ranges.push_back({0, R.ContentSize});
    std::sort(Ranges.begin(), Ranges.end(),
              [](const auto &A, const auto &B) { return A.RVA < B.RVA; });
    for (const auto &R : Ranges) {
      if (!M.ExportMetadata.empty() &&
          R.RVA <= M.ExportMetadata.back().RVA + M.ExportMetadata.back().Size) {
        auto &Last = M.ExportMetadata.back();
        Last.Size = std::max(Last.Size, R.RVA + R.Size - Last.RVA);
      } else
        M.ExportMetadata.push_back(R);
    }
    for (const auto &Dependency : M.Loaded.Dependencies) {
      if (findProvider(Dependency))
        continue;
      auto Key = moduleName(Dependency);
      if (!Key)
        return Key.takeError();
      auto Target = visit(*Key);
      if (!Target)
        return Target.takeError();
      addEdge(Index, *Target);
    }
    Loading.erase(Index);
    if (DLL)
      P.LoaderInitializationOrder.push_back(Index);
    return Index;
  }
  void addEdge(size_t Owner, size_t Target) {
    if (Owner == Target)
      return;
    auto Ref = moduleRef(P, Target);
    auto &Edges = P.Modules[Owner].Dependencies;
    if (!llvm::is_contained(Edges, Ref)) {
      Edges.push_back(Ref);
      Change.Edges.emplace_back(moduleRef(P, Owner), Ref);
    }
  }
  llvm::Expected<ModuleLink> link(llvm::StringRef Name,
                                  const std::filesystem::path *Main) {
    auto Root = visit(Name, Main);
    if (!Root)
      return Root.takeError();
    Change.Root = *Root;
    auto Forward = [&](size_t Owner,
                       llvm::StringRef Name) -> llvm::Expected<size_t> {
      auto Target = visit(Name);
      if (!Target)
        return Target.takeError();
      addEdge(Owner, *Target);
      return *Target;
    };
    for (size_t M = 0; M < Change.Added.size(); ++M) {
      const size_t Index = Change.Added[M].Index;
      for (auto &I : P.Modules[Index].Loaded.Imports) {
        if (I.Target) {
          I.Gate = P.ServiceGates.at({I.Module, I.Name});
          continue;
        }
        auto Provider = findModule(P, I.Module);
        if (!Provider)
          return failure(text::ModuleMissing + I.Module);
        auto Target = resolveExport(P, *Provider, I.Name, I.Ordinal, Budget,
                                    CPU, Forward);
        if (!Target)
          return Target.takeError();
        if (!Target->Address)
          return llvm::make_error<ModuleLoadError>(Target->Error);
        I.Gate = *Target->Address;
      }
    }
    std::set<size_t> Ordered, Visiting;
    std::function<llvm::Error(size_t)> Order =
        [&](size_t Index) -> llvm::Error {
      if (auto E = checkTime())
        return E;
      if (Visiting.contains(Index))
        return failure(text::ModuleCycle + P.Identities[Index].Name);
      if (!Ordered.insert(Index).second)
        return llvm::Error::success();
      Visiting.insert(Index);
      for (auto Dependency : P.Modules[Index].Dependencies) {
        if (!current(P, Dependency))
          return failure(text::Lifetime);
        if (auto E = Order(Dependency.Index))
          return E;
      }
      Visiting.erase(Index);
      if (Index && P.Modules[Index].State == ModuleState::Prepared)
        Change.Attach.push_back(moduleRef(P, Index));
      return llvm::Error::success();
    };
    if (auto E = Order(*Root))
      return std::move(E);
    return Change;
  }
  Program &P;
  VirtualMemory &Memory;
  const ExecutionBudget &Budget;
  ExecutionBackend *CPU;
  ModuleLink Change;
  std::set<size_t> Loading;
};
} // namespace
llvm::Expected<ModuleLink> linkModule(Program &P, llvm::StringRef Name,
                                      VirtualMemory &Memory,
                                      const ExecutionBudget &Budget,
                                      ExecutionBackend *CPU) {
  return Linker(P, Memory, Budget, CPU).run(Name);
}
llvm::Expected<Program> loadProgram(const std::filesystem::path &Path,
                                    const ProcessOptions &Options,
                                    const ExecutionBudget &Budget,
                                    VirtualMemory &Memory) {
  Program Out;
  if (Options.Windows) {
    if (Options.Windows->Modules.size() > windows_process_limits::Modules)
      return failure(text::ModuleBudget);
    for (const auto &Input : Options.Windows->Modules) {
      auto Name = moduleName(Input.Name);
      if (!Name)
        return Name.takeError();
      if (Input.Path.empty() ||
          Input.Path.native().find(typename std::filesystem::path::value_type(
              0)) != std::filesystem::path::string_type::npos ||
          findProvider(*Name) ||
          !Out.Catalogue.emplace(*Name, Input.Path).second)
        return failure(text::ModuleName + Input.Name);
    }
  }
  const uint64_t RuntimeBytes =
      EnvironmentEnd - TEB + GateSize + Options.StackSize;
  if (RuntimeBytes >= Options.MemoryLimit)
    return failure(text::ModuleBudget);
  Out.Reads = {Options.MemoryLimit, Options.MemoryLimit - RuntimeBytes};
  const auto FileName = Path.filename().u8string();
  const std::string MainName(reinterpret_cast<const char *>(FileName.data()),
                             FileName.size());
  if (Out.Catalogue.contains(llvm::StringRef(MainName).lower()))
    return failure(text::ModuleName + MainName);
  for (const char *Provider : {text::Kernel32, text::KernelBase, text::NTDLL})
    for (const auto &S : services()) {
      if (findProvider(Provider) != S.Provider)
        continue;
      if (Out.Gates.size() == MaxImports)
        return failure(text::ModuleBudget);
      const uint64_t Gate =
          GateBase + (FirstImportGate + Out.Gates.size()) * GateStride;
      Out.Gates.push_back({0, &S, Provider, Gate, S.Name, std::nullopt});
      Out.ServiceGates.emplace(std::pair{Provider, S.Name}, Gate);
    }
  auto Linked = Linker(Out, Memory, Budget).run(MainName, &Path);
  if (!Linked)
    return Linked.takeError();
  for (auto Ref : Linked->Attach)
    Out.AttachOrder.push_back(Ref.Index);
  for (auto &M : Out.Modules)
    M.Pinned = true;
  Out.Modules.front().References = 1;
  return Out;
}
} // namespace neverd::emulation::windows_process
