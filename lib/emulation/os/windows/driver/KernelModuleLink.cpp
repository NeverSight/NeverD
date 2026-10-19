//===- KernelModuleLink.cpp - Pure stopped PE module batch linking --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pure stopped PE module batch linking.
///

#include "../exception/X64SEH.h"
#include "../exception/X64SEHMetadata.h"
#include "../kernel/KernelExportRegistry.h"
#include "KernelModuleImage.h"

#include "neverd/Common.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "kernel module linker: " + Text);
}
struct Target {
  uint64_t Address;
  modules::SymbolKind Kind;
  unsigned Hops = 0;
};
struct Provider {
  const ModuleBinding *Binding;
  const ModuleInstance *Existing = nullptr;
  const ModuleMappingCandidate *Candidate = nullptr;
  const KernelModulePreparation *Input = nullptr;
  struct ExportRef {
    const PEProgramExport *Raw = nullptr;
    const ModuleExport *Bound = nullptr;
  };
  std::map<std::string, ExportRef> ByName;
  std::map<uint32_t, ExportRef> ByOrdinal;
};
class Linker {
public:
  explicit Linker(const KernelExportRegistry &Registry)
      : Registry(Registry), ModelProviders(Registry.importProviders()),
        Names(ModelProviders) {}
  llvm::Error add(Provider P) {
    auto Name =
        KernelExportRegistry::canonicalImportModule(P.Binding->GuestName);
    if (!Name || (!P.Binding->Scope.empty() && P.Binding->Scope != "kernel"))
      return invalid("invalid kernel provider identity");
    if (P.Input) {
      for (const auto &E : P.Input->Exports.Entries) {
        if (++Work > 1000000)
          return invalid("export index work limit exhausted");
        P.ByOrdinal.emplace(E.Ordinal, Provider::ExportRef{&E, nullptr});
        for (const auto &Name : E.Names) {
          if (++Work > 1000000)
            return invalid("export index work limit exhausted");
          P.ByName.emplace(Name, Provider::ExportRef{&E, nullptr});
        }
      }
      for (const auto &E : P.Candidate->Plan.Exports)
        if (E.Ordinal)
          Direct.emplace(std::pair{P.Binding->ID, *E.Ordinal},
                         Target{E.Address, E.Kind});
    } else if (P.Existing && P.Existing->mapped()) {
      for (const auto &E : P.Existing->Exports) {
        if (++Work > 1000000)
          return invalid("export index work limit exhausted");
        if (!E.Name.empty())
          P.ByName.emplace(E.Name, Provider::ExportRef{nullptr, &E});
        if (E.Ordinal)
          P.ByOrdinal.emplace(*E.Ordinal, Provider::ExportRef{nullptr, &E});
      }
    }
    if (!Providers.emplace(*Name, std::move(P)).second)
      return invalid("guest module name aliases another provider: " + *Name);
    Names.insert(*Name);
    return llvm::Error::success();
  }
  llvm::Expected<Target> find(llvm::StringRef Module, llvm::StringRef Symbol,
                              std::optional<uint32_t> Ordinal = {},
                              unsigned Depth = 0) {
    if (Depth >= 64 || ++Work > 1000000)
      return invalid("forwarder depth/work limit exhausted");
    auto Name = moduleName(Module);
    if (!Name)
      return Name.takeError();
    const std::string Spelling =
        Ordinal ? (neverd::kOrdinalPrefix + llvm::Twine(*Ordinal)).str()
                : Symbol.str();
    auto Model = modelTarget(*Name, Spelling, Ordinal);
    if (!Model)
      return Model.takeError();
    if (*Model)
      return **Model;
    const auto P = Providers.find(*Name);
    if (P == Providers.end())
      return llvm::make_error<ModuleDependencyError>(Module.str());
    if (P->second.Existing) {
      const auto &I = *P->second.Existing;
      if (!I.mapped())
        return llvm::make_error<ModuleDependencyError>(I.Binding.GuestName,
                                                       I.Binding.ID);
    }
    const auto &PRef = P->second;
    const Provider::ExportRef *E = nullptr;
    if (Ordinal) {
      auto I = PRef.ByOrdinal.find(*Ordinal);
      if (I != PRef.ByOrdinal.end())
        E = &I->second;
    } else {
      auto I = PRef.ByName.find(Symbol.str());
      if (I != PRef.ByName.end())
        E = &I->second;
    }
    if (E && E->Bound)
      return Target{E->Bound->Address, E->Bound->Kind};
    if (E && E->Raw)
      return resolve(PRef, *E->Raw, Depth);
    return invalid("export is absent from provided module: " + Module + "!" +
                   Spelling);
  }
  llvm::Expected<Target> resolve(const Provider &P, const PEProgramExport &E,
                                 unsigned Depth = 0) {
    const auto Key = std::pair{P.Binding->ID, E.Ordinal};
    if (const auto I = Resolved.find(Key); I != Resolved.end()) {
      if (Depth + I->second.Hops >= 64)
        return invalid("forwarder depth/work limit exhausted");
      return I->second;
    }
    if (!Active.insert(Key).second)
      return invalid("export forwarder cycle");
    if (E.Kind == PEExportKind::Hole)
      return invalid("forwarder/import targets an empty export ordinal");
    std::optional<Target> Model;
    for (const auto &Name : E.Names) {
      if (++Work > 1000000)
        return invalid("export resolution work limit exhausted");
      auto Bound = modelTarget(P.Binding->GuestName, Name);
      if (!Bound)
        return Bound.takeError();
      if (*Bound) {
        if (Model && (Model->Address != (**Bound).Address ||
                      Model->Kind != (**Bound).Kind))
          return invalid(
              "export ordinal aliases incompatible model identities");
        Model = **Bound;
      }
    }
    Target Result{};
    if (Model) {
      Result = *Model;
    } else if (E.Kind == PEExportKind::Address) {
      const auto I = Direct.find(Key);
      if (I == Direct.end())
        return invalid("validated direct export lost its mapped target");
      Result = I->second;
    } else {
      const auto [Module, Symbol] = llvm::StringRef(E.Forwarder).rsplit('.');
      if (Module.empty() || Symbol.empty() ||
          Symbol.size() > profile::MaxKernelExportNameSize ||
          !std::all_of(Symbol.begin(), Symbol.end(),
                       [](unsigned char C) { return C >= '!' && C <= '~'; }))
        return invalid("invalid PE export forwarder identity");
      std::optional<uint32_t> Ordinal;
      if (Symbol.starts_with("#")) {
        const auto Digits = Symbol.drop_front();
        uint16_t Number = 0;
        if (Digits.empty() || !llvm::all_of(Digits, llvm::isDigit) ||
            Digits.getAsInteger(10, Number))
          return invalid("invalid forwarder ordinal");
        Ordinal = Number;
      }
      auto T = find(Module, Symbol, Ordinal, Depth + 1);
      if (!T)
        return T.takeError();
      Result = *T;
      if (++Result.Hops >= 64)
        return invalid("forwarder depth/work limit exhausted");
    }
    Active.erase(Key);
    Resolved.emplace(Key, Result);
    return Result;
  }

private:
  llvm::Expected<std::optional<Target>>
  modelTarget(llvm::StringRef Module, llvm::StringRef Name,
              std::optional<uint32_t> Ordinal = {}) const {
    const auto Provider = KernelExportRegistry::canonicalImportModule(Module);
    if (!Provider || !ModelProviders.contains(*Provider))
      return std::optional<Target>();
    auto Bound = Registry.findImport({0, Module.str(), Name.str(), Ordinal});
    if (!Bound)
      return Bound.takeError();
    if (!*Bound)
      return std::optional<Target>();
    const auto *E = Registry.lookup(**Bound);
    if (!E)
      return invalid("existing binding lost its model identity");
    using K = KernelExportRegistry::ExportKind;
    const bool Function =
        E->Kind == K::ModuleExport || E->Kind == K::FrameworkFunction ||
        E->Kind == K::DMAFunction || E->Kind == K::ProviderFunction ||
        E->Kind == K::HalDispatchFunction;
    return std::optional(Target{**Bound, Function
                                             ? modules::SymbolKind::Function
                                             : modules::SymbolKind::Data});
  }
  llvm::Expected<std::string> moduleName(llvm::StringRef Module) const {
    if (auto Name = KernelExportRegistry::canonicalImportModule(Module))
      return *Name;
    if (Module.empty() || Module.size() > profile::MaxKernelExportNameSize ||
        Module.front() == '.' || Module.contains("..") ||
        !std::all_of(Module.begin(), Module.end(), [](unsigned char C) {
          return llvm::isAlnum(C) || C == '_' || C == '-' || C == '.';
        }))
      return invalid("invalid forwarder provider identity");
    std::set<std::string> Matches;
    for (const auto &Name : Names)
      if (KernelExportRegistry::matchesImportProvider(Module, Name))
        Matches.insert(Name);
    if (Matches.size() > 1)
      return invalid("ambiguous extensionless forwarder provider: " + Module);
    if (!Matches.empty())
      return *Matches.begin();
    // An opaque request identity, never a guessed file path or host search.
    return Module.str();
  }
  const KernelExportRegistry &Registry;
  const std::set<std::string> ModelProviders;
  std::set<std::string> Names;
  std::map<std::string, Provider> Providers;
  std::set<std::pair<std::string, uint32_t>> Active;
  std::map<std::pair<std::string, uint32_t>, Target> Resolved;
  std::map<std::pair<std::string, uint32_t>, Target> Direct;
  uint64_t Work = 0;
};
} // namespace

llvm::Expected<std::vector<KernelImportPatch>>
planKernelGuestImports(llvm::ArrayRef<ModuleInstance> Instances,
                       llvm::ArrayRef<DriverImport> Imports,
                       const KernelExportRegistry &Registry,
                       const std::shared_ptr<AddressSpace> &Space) {
  Linker L(Registry);
  for (const auto &I : Instances)
    if (I.Binding.Space == Space)
      if (auto E = L.add({&I.Binding, &I}))
        return std::move(E);
  std::vector<KernelImportPatch> Patches;
  for (const auto &I : Imports) {
    auto T = L.find(I.Module, I.Name, I.Ordinal);
    if (!T)
      return T.takeError();
    Patches.push_back({I.Slot, T->Address});
  }
  return Patches;
}

llvm::Error
finalizeKernelGuestModules(llvm::ArrayRef<ModuleInstance> Existing,
                           llvm::ArrayRef<ModuleMappingCandidate> Candidates,
                           const KernelExportRegistry &Registry) {
  if (Candidates.empty())
    return invalid("requires a nonempty mapping batch");
  Linker L(Registry);
  std::set<std::string> IDs;
  std::vector<Provider> Pending;
  for (const auto &C : Candidates) {
    if (!C.Plan.Preparation ||
        C.Plan.Preparation->typeID() != &KernelModulePreparation::ID)
      return invalid("missing native module preparation");
    auto *Input =
        static_cast<const KernelModulePreparation *>(C.Plan.Preparation.get());
    Provider P{&C.Binding, nullptr, &C, Input};
    if (auto E = L.add(P))
      return E;
    Pending.push_back(P);
    IDs.insert(C.Binding.ID);
  }
  for (const auto &I : Existing)
    if (!IDs.contains(I.Binding.ID) &&
        I.Binding.Space == Candidates.front().Binding.Space)
      if (auto E = L.add({&I.Binding, &I}))
        return E;
  for (const auto &P : Pending) {
    auto &Plan = P.Candidate->Plan;
    Plan.Exports.clear();
    for (const auto &E : P.Input->Exports.Entries) {
      if (E.Kind == PEExportKind::Hole)
        continue;
      auto T = L.resolve(P, E);
      if (!T)
        return T.takeError();
      const bool Forwarded = E.Kind == PEExportKind::Forwarder ||
                             T->Address < Plan.Image.Base ||
                             T->Address - Plan.Image.Base >= Plan.Size;
      if (E.Names.empty())
        Plan.Exports.push_back({{}, E.Ordinal, T->Address, T->Kind, Forwarded});
      else
        for (const auto &Name : E.Names)
          Plan.Exports.push_back(
              {Name, E.Ordinal, T->Address, T->Kind, Forwarded});
    }
    for (const auto &I : P.Input->Imports) {
      auto T = L.find(I.Module, I.Name, I.Ordinal);
      if (!T)
        return T.takeError();
      bool Patched = false;
      for (auto &R : Plan.Image.Regions)
        if (I.Slot >= R.Address && R.Bytes.size() >= 8 &&
            I.Slot - R.Address <= R.Bytes.size() - 8) {
          llvm::support::endian::write64le(R.Bytes.data() + I.Slot - R.Address,
                                           T->Address);
          Patched = true;
          break;
        }
      if (!Patched)
        return invalid("validated import slot disappeared from its image");
    }
    auto Runtime = std::make_shared<KernelModuleRuntimeData>();
    Runtime->Unwind = P.Input->Unwind;
    Runtime->Base = Plan.Image.Base;
    Runtime->Size = Plan.Size;
    Runtime->SourceDigest = P.Candidate->Source.imageDigest();
    Runtime->Guard = P.Input->Guard;
    if (!Runtime->Guard.Enabled)
      for (const auto &E : Plan.Exports)
        if (!E.Forwarded && E.Kind == modules::SymbolKind::Function)
          Runtime->CompatibilityExports.push_back(E.Address);
    if (!Runtime->Unwind.Metadata)
      return invalid("missing native unwind preparation");
    std::vector<PEMetadataRange> Code;
    for (const auto &R : Plan.Image.Regions)
      if (R.Permissions & Execute)
        Code.push_back({R.Address - Plan.Image.Base, R.Bytes.size()});
    auto Ranges = x64SEHMetadataRanges(
        *Runtime->Unwind.Metadata, Runtime->Unwind.PreferredBase, Runtime->Size,
        Runtime->Unwind.HeaderSize, Code, UINT64_MAX);
    if (!Ranges)
      return Ranges.takeError();
    for (const auto &I : P.Input->Imports)
      if (llvm::any_of(x64SEHPersonalityExports(),
                       [&](auto Name) { return I.Name == Name; })) {
        const bool Kernel =
            KernelExportRegistry::canonicalImportModule(I.Module) ==
            std::optional<std::string>("ntoskrnl.exe");
        const bool Used = llvm::any_of(
            Runtime->Unwind.Metadata->Functions, [&](const auto &F) {
              return F.PersonalityName == I.Name || (Kernel && F.GSCookie);
            });
        if (!Used)
          continue;
        auto Address = Registry.findImport(I);
        if (!Address)
          return Address.takeError();
        if (!*Address)
          return invalid("SEH personality has no authoritative kernel binding");
        Ranges->Ranges.push_back({I.Slot - Plan.Image.Base, 8});
      }
    auto Witness =
        [&](uint64_t RVA, uint64_t N,
            std::vector<KernelModuleRuntimeData::Witness> &Out) -> llvm::Error {
      uint64_t A = Plan.Image.Base + RVA;
      while (N) {
        const auto R = llvm::find_if(Plan.Image.Regions, [&](const auto &R) {
          return A >= R.Address && A - R.Address < R.Bytes.size();
        });
        if (R == Plan.Image.Regions.end())
          return invalid("native metadata witness has no initialized mapping");
        const auto Offset = A - R->Address;
        const auto Count = std::min(N, R->Bytes.size() - Offset);
        Out.push_back(
            {A,
             {R->Bytes.begin() + Offset, R->Bytes.begin() + Offset + Count}});
        A += Count;
        N -= Count;
      }
      return llvm::Error::success();
    };
    for (const auto &Range : Ranges->Ranges)
      if (auto E = Witness(Range.RVA, Range.Size, Runtime->Witnesses))
        return E;
    for (const auto &Range : Runtime->Guard.MetadataRanges) {
      if (auto E = Witness(Range.RVA, Range.Size, Runtime->GuardWitnesses))
        return E;
    }
    Plan.RuntimeData = std::move(Runtime);
  }
  for (const auto &C : Candidates)
    C.Plan.Preparation.reset();
  return llvm::Error::success();
}
} // namespace neverd::emulation
