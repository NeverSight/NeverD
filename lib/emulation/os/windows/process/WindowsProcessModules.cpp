//===- WindowsProcessModules.cpp - Explicit acyclic PE module linking ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessModules.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <functional>

namespace neverd::emulation::windows_process {
namespace {
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
llvm::Error relocate(Image &Loaded, uint64_t Base) {
  const uint64_t Bias = Base - Loaded.Base;
  if (!Bias)
    return llvm::Error::success();
  for (uint64_t RVA : Loaded.Relocations) {
    bool Found = false;
    for (auto &R : Loaded.Regions) {
      const uint64_t Start = R.Address - Loaded.Base;
      if (RVA < Start || RVA - Start >= R.Bytes.size() ||
          PointerSize > R.Bytes.size() - (RVA - Start))
        continue;
      auto *Slot = R.Bytes.data() + RVA - Start;
      llvm::support::endian::write64le(
          Slot, llvm::support::endian::read64le(Slot) + Bias);
      Found = true;
      break;
    }
    if (!Found)
      return failure(text::Metadata);
  }
  for (auto &R : Loaded.Regions)
    R.Address += Bias;
  for (auto &I : Loaded.Imports)
    I.Slot += Bias;
  // Only no-entry, no-TLS DLLs are movable in this startup contract.
  Loaded.Base = Base;
  return llvm::Error::success();
}
} // namespace
llvm::Expected<Program> loadProgram(const std::filesystem::path &Path,
                                    const ProcessOptions &Options,
                                    const ExecutionBudget &Budget,
                                    VirtualMemory &Memory) {
  auto CheckTime = [&]() -> llvm::Error {
    return Budget.remainingMicroseconds() ? llvm::Error::success()
                                          : failure(text::ModuleTimeout);
  };
  if (auto E = CheckTime())
    return std::move(E);
  std::map<std::string, std::filesystem::path> Catalogue;
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
          findProvider(*Name) || !Catalogue.emplace(*Name, Input.Path).second)
        return failure(text::ModuleName + Input.Name);
    }
  }
  const uint64_t RuntimeBytes =
      EnvironmentEnd - TEB + GateSize + Options.StackSize;
  if (RuntimeBytes >= Options.MemoryLimit)
    return failure(text::ModuleBudget);
  ImageReadBudget Reads{Options.MemoryLimit,
                        Options.MemoryLimit - RuntimeBytes};
  Program Out;
  std::map<std::string, size_t> LoadedNames;
  std::vector<bool> Loading;
  std::function<llvm::Error(const std::filesystem::path &, std::string, bool)>
      Visit;
  Visit = [&](const std::filesystem::path &File, std::string Name,
              bool DLL) -> llvm::Error {
    if (auto E = CheckTime())
      return E;
    if (auto I = LoadedNames.find(Name); I != LoadedNames.end())
      return Loading[I->second] ? failure(text::ModuleCycle + Name)
                                : llvm::Error::success();
    if (Out.Modules.size() > windows_process_limits::Modules)
      return failure(text::ModuleBudget);
    auto Image = loadProgramImage(File, Reads, DLL);
    if (!Image)
      return Image.takeError();
    if (auto E = CheckTime())
      return E;
    if (DLL && Image->Architecture != Out.Modules.front().Loaded.Architecture)
      return failure(text::ModuleISA);
    auto Base = Memory.reserveImage(Image->Base, Image->Size,
                                    DLL && Image->Relocatable);
    if (!Base)
      return Base.takeError();
    if (auto E = relocate(*Image, *Base))
      return E;
    const size_t Index = Out.Modules.size();
    LoadedNames.emplace(Name, Index);
    Loading.push_back(true);
    Out.Identities.push_back({Name, Image->Base, Image->Size, Image->Entry});
    Out.Modules.push_back({std::move(*Image), {}, {}});
    // A recursive append may move the module vector; retain indices, not
    // borrows.
    const auto Dependencies = Out.Modules[Index].Loaded.Dependencies;
    for (const auto &Dependency : Dependencies) {
      if (findProvider(Dependency))
        continue;
      auto Key = moduleName(Dependency);
      if (!Key)
        return Key.takeError();
      auto Input = Catalogue.find(*Key);
      if (Input == Catalogue.end())
        return failure(text::ModuleMissing + Dependency);
      if (auto E = Visit(Input->second, *Key, true))
        return E;
    }
    Loading[Index] = false;
    if (DLL)
      Out.InitializationOrder.push_back(Index);
    return llvm::Error::success();
  };
  const auto FileName = Path.filename().u8string();
  const std::string MainName(reinterpret_cast<const char *>(FileName.data()),
                             FileName.size());
  if (Catalogue.contains(llvm::StringRef(MainName).lower()))
    return failure(text::ModuleName + MainName);
  if (auto E = Visit(Path, MainName, false))
    return std::move(E);
  for (auto &M : Out.Modules) {
    for (const auto &E : M.Loaded.Exports.Entries) {
      if (E.Kind != PEExportKind::Address)
        continue;
      const uint64_t Address = M.Loaded.Base + E.RVA;
      M.Ordinals.emplace(E.Ordinal, Address);
      for (const auto &Name : E.Names)
        M.Names.emplace(Name, Address);
    }
  }
  std::map<std::pair<std::string, std::string>, uint64_t> Gates;
  for (auto &M : Out.Modules) {
    if (auto E = CheckTime())
      return std::move(E);
    for (auto &I : M.Loaded.Imports) {
      if (I.Target) {
        auto [Gate, New] = Gates.emplace(std::pair{I.Module, I.Name}, 0);
        if (New) {
          if (Out.Gates.size() == MaxImports)
            return failure(text::ModuleBudget);
          Gate->second =
              GateBase + (FirstImportGate + Out.Gates.size()) * GateStride;
          I.Gate = Gate->second;
          Out.Gates.push_back(I);
        } else
          I.Gate = Gate->second;
      } else {
        auto Provider = LoadedNames.find(I.Module);
        if (Provider == LoadedNames.end())
          return failure(text::ModuleMissing + I.Module);
        const auto &Dependency = Out.Modules[Provider->second];
        if (I.Ordinal) {
          auto Symbol = Dependency.Ordinals.find(*I.Ordinal);
          if (Symbol != Dependency.Ordinals.end())
            I.Gate = Symbol->second;
        } else {
          auto Symbol = Dependency.Names.find(I.Name);
          if (Symbol != Dependency.Names.end())
            I.Gate = Symbol->second;
        }
        if (!I.Gate)
          return failure(
              text::ModuleExport + I.Module +
              llvm::Twine(text::ImportSeparator) +
              (I.Ordinal ? llvm::Twine(*I.Ordinal) : llvm::Twine(I.Name)));
      }
    }
  }
  if (auto E = CheckTime())
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation::windows_process
