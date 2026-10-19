//===- DriverExecutionFactory.cpp - Persistent public WDM configuration
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Persistent public WDM configuration.
///

#include "neverd/emulation/DriverExecutionFactory.h"

#include "../../../runtime/ExecutionJSONParsing.h"
#include "DriverExecutionRun.h"
#include "DriverResultView.h"

#include "neverd/support/FilePath.h"

#include "llvm/Support/FormatVariadic.h"

#include <tuple>

namespace neverd::emulation {
namespace {
namespace j = llvm::json;
using namespace execution_json;
struct Input {
  DriverOptions Options;
  std::filesystem::path Image;
  std::string ImageModule, KernelModule;
  DriverResultView Report;
  bool PrimaryModelImports = false;
};
llvm::Expected<Input> decode(const ExecutionCreationInput &Creation,
                             const std::filesystem::path &SourceOrigin) {
  auto V = parse(Creation.EnvironmentJSON);
  if (!V)
    return V.takeError();
  const auto *O = V->getAsObject();
  if (!O)
    return invalid("driver environment must be an object");
  if (auto E =
          keys(*O, {"kind", "image", "image_module", "kernel_module",
                    "base_directory", "backend", "contract", "memory_limit",
                    "scenario", "report", "primary_import_policy"}))
    return std::move(E);
  if (Creation.EnvironmentKind != "windows-driver" ||
      O->getString("kind") != "windows-driver")
    return invalid("driver factory requires kind=windows-driver");
  Input R;
  if (const auto *Policy = O->get("primary_import_policy")) {
    auto Name = Policy->getAsString();
    if (!Name || (*Name != "modules" && *Name != "model-traps"))
      return invalid("primary_import_policy must be modules or model-traps");
    R.PrimaryModelImports = *Name == "model-traps";
  }
  if (const auto *Value = O->get("report")) {
    const auto *Report = Value->getAsObject();
    if (!Report)
      return invalid("driver report must be an object");
    if (auto E = keys(*Report, {"call_tail", "write_tail"}))
      return std::move(E);
    if (auto E = positive(*Report, "call_tail", R.Report.CallTail, 1000000))
      return std::move(E);
    if (auto E = positive(*Report, "write_tail", R.Report.WriteTail, 1000000))
      return std::move(E);
    R.Report.Enabled = true;
  }
  const auto &L = Creation.Options.Limits;
  R.Options.InstructionLimit = L.Instructions;
  R.Options.EventLimit = L.Events;
  // Legacy DriverOptions reports milliseconds. The common run retains and
  // enforces the exact microsecond limit, including a fractional millisecond.
  R.Options.TimeoutMilliseconds =
      L.TimeoutMicroseconds / 1000 + (L.TimeoutMicroseconds % 1000 != 0);
  if (const auto *S = O->get("scenario")) {
    if (!S->getAsObject())
      return invalid("driver scenario must be an object");
    auto Options = driverOptionsFromScenarioJSON(llvm::formatv("{0}", *S).str(),
                                                 std::move(R.Options));
    if (!Options)
      return Options.takeError();
    R.Options = std::move(*Options);
  }
  if (auto E = positive(*O, "memory_limit", R.Options.MemoryLimit,
                        profile::MaxMemoryLimit))
    return std::move(E);
  for (const auto Name : {"backend", "contract"}) {
    if (!O->get(Name))
      continue;
    auto S = text(*O, Name, execution_limit::NameBytes);
    if (!S)
      return S.takeError();
    if (llvm::StringRef(Name) == "backend") {
      auto B = parseExecutionBackend(*S);
      if (!B)
        return B.takeError();
      R.Options.Backend = *B;
    } else {
      auto C = parseExecutionContract(*S);
      if (!C)
        return C.takeError();
      R.Options.Contract = *C;
    }
  }
  auto Origin = SourceOrigin;
  if (O->get("base_directory")) {
    auto B = text(*O, "base_directory", modules::input_limit::PathBytes);
    if (!B)
      return B.takeError();
    auto P = modules::normalizeInputPath(*B);
    if (!P)
      return P.takeError();
    Origin = std::move(*P);
  }
  if (bool(O->get("image")) == bool(O->get("image_module")))
    return invalid("driver requires exactly one of image or image_module");
  if (O->get("image")) {
    auto Name = text(*O, "image", modules::input_limit::PathBytes);
    if (!Name)
      return Name.takeError();
    auto Image = modules::normalizeInputPath(*Name, Origin);
    if (!Image)
      return Image.takeError();
    R.Image = std::move(*Image);
  } else {
    auto ID = text(*O, "image_module", modules::input_limit::IDBytes);
    if (!ID)
      return ID.takeError();
    R.ImageModule = std::move(*ID);
  }
  if (O->get("kernel_module")) {
    if (!R.Options.KernelImage.empty() || !R.Options.KernelPDB.empty())
      return invalid(
          "kernel_module conflicts with legacy kernel_image/kernel_pdb");
    auto ID = text(*O, "kernel_module", modules::input_limit::IDBytes);
    if (!ID)
      return ID.takeError();
    R.KernelModule = std::move(*ID);
  }
  auto Normalize = [&](std::filesystem::path &Path) -> llvm::Error {
    if (Path.empty())
      return llvm::Error::success();
    auto P = modules::normalizeInputPath(pathToUTF8(Path), Origin);
    if (!P)
      return P.takeError();
    Path = std::move(*P);
    return llvm::Error::success();
  };
  if (auto E = Normalize(R.Options.KernelImage))
    return std::move(E);
  if (auto E = Normalize(R.Options.KernelPDB))
    return std::move(E);
  if (R.Options.ServiceImages)
    for (auto &Service : *R.Options.ServiceImages)
      if (auto E = Normalize(Service.Source))
        return std::move(E);
  return R;
}
} // namespace
llvm::Expected<ExecutionRunFactoryResult>
createDriverExecution(ExecutionCreationInput Creation,
                      std::shared_ptr<modules::ModuleSet> Sources,
                      const std::filesystem::path &SourceOrigin) {
  if (!Sources)
    return invalid("driver factory requires explicit module sources");
  auto Input = decode(Creation, SourceOrigin);
  if (!Input)
    return Input.takeError();
  DriverSessionState::ResourceInputs Resources;
  if (!Input->ImageModule.empty()) {
    auto Image = Sources->prepare(Input->ImageModule);
    if (!Image)
      return Image.takeError();
    Resources.Image = std::move(*Image);
    Resources.ImageID = Input->ImageModule;
  }
  if (!Input->KernelModule.empty()) {
    auto Kernel = Sources->prepare(Input->KernelModule);
    if (!Kernel)
      return Kernel.takeError();
    Resources.Kernel = std::move(*Kernel);
    Resources.KernelID = Input->KernelModule;
    Input->Options.KernelImage = Resources.Kernel->sourcePath();
    Input->Options.KernelPDB = Resources.Kernel->debugSourcePath();
  }
  for (const auto &I : Sources->inspect()) {
    if (I.Input.ID == Resources.ImageID)
      Resources.ImageLoad = I.Load;
    if (I.Input.ID == Resources.KernelID)
      Resources.KernelLoad = I.Load;
  }
  const auto TransferLimit = Creation.Options.MemoryTransferLimit;
  const bool ResourcePausesEnabled = Creation.Options.EnableResourcePauses;
  auto Prepared = createDriverExecutionRun(
      Input->Image, Input->Options, std::move(Creation.Options),
      std::move(Sources), std::move(Resources), Input->PrimaryModelImports);
  if (!Prepared)
    return Prepared.takeError();
  ExecutionRunFactoryResult Result;
  Result.MemoryTransferLimit = TransferLimit;
  const auto Space = Prepared->State->CPU->addressSpace();
  Result.ResolveSpace = [Space](llvm::StringRef Name)
      -> llvm::Expected<std::shared_ptr<AddressSpace>> {
    if (Name.empty() || Name == "kernel")
      return Space;
    return invalid("unknown driver address space: " + Name);
  };
  Result.IdentifySpace = [Space](const std::shared_ptr<AddressSpace> &S)
      -> llvm::Expected<std::string> {
    if (S != Space)
      return invalid("foreign driver address space");
    return std::string("kernel");
  };
  auto Bindings =
      resolveExecutionBindings(Creation.Bindings, Result.ResolveSpace);
  if (!Bindings)
    return Bindings.takeError();
  for (auto &Binding : *Bindings)
    if (auto E = Prepared->Run->declareModule(std::move(Binding)))
      return std::move(E);
  auto *State = Prepared->State;
  auto *Run = Prepared->Run.get();
  Result.InspectEnvironment =
      [State, Run, ResourcePausesEnabled, Report = Input->Report,
       DriverID = Input->ImageModule,
       KernelID = Input->KernelModule]() -> llvm::Expected<std::string> {
    // A stopped snapshot observes the same canonical kernel objects. It does
    // not drain callbacks, restore a failed call, or resume a coroutine.
    if (auto E = State->Kernel->snapshot())
      return std::move(E);
    auto V = j::parse(driverResultJSONView(State->Result, Report));
    if (!V)
      return V.takeError();
    auto &O = *V->getAsObject();
    if (Run->state() != ExecutionRunState::Completed) {
      O.erase("stop_reason");
      O.erase("scenario_success");
    }
    O["observation_only"] = Run->state() != ExecutionRunState::Completed;
    j::Array Inputs;
    for (const auto &[Role, ID, Source] :
         {std::tuple{"driver", DriverID, State->Inputs.Image},
          std::tuple{"kernel-reference", KernelID, State->Inputs.Kernel}})
      if (Source && !ID.empty())
        Inputs.push_back(j::Object{{"role", Role},
                                   {"module_id", ID},
                                   {"image_sha256", Source->imageDigest()},
                                   {"debug_sha256", Source->debugDigest()}});
    O["input_modules"] = std::move(Inputs);
    O["primary_imports_linked"] = State->ModuleImportsLinked;
    O["primary_import_policy"] =
        State->Inputs.LinkModuleImports ? "modules" : "model-traps";
    O["capabilities"] =
        j::Object{{"module_publication", true},
                  {"resource_pause", State->CPU->supportsResourcePause()},
                  {"resource_pause_enabled", ResourcePausesEnabled},
                  {"module_dependencies", true},
                  {"module_x64_c_seh", true},
                  {"module_cfg", true},
                  {"primary_module_imports", State->Inputs.LinkModuleImports},
                  {"module_entry_initialization", false},
                  {"invocation_stages", j::Array{"after-entry"}},
                  {"invocation_abis", j::Array{"win64"}}};
    return llvm::formatv("{0}", *V).str();
  };
  Result.Run = std::move(Prepared->Run);
  return Result;
}
} // namespace neverd::emulation
