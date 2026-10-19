//===- KernelModulePublicationTests.cpp - Native module/common transaction
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native module/common transaction.
///

#include "gtest/gtest.h"
#include "os/windows/driver/DriverBootstrapModules.h"
#include "os/windows/driver/DriverExceptionPublication.h"
#include "os/windows/driver/DriverExecutionRun.h"
#include "os/windows/driver/DriverGuardPublication.h"
#include "os/windows/driver/DriverImportPublication.h"
#include "os/windows/driver/KernelModuleImage.h"
#include "os/windows/kernel/KernelGuestPageTables.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

using namespace neverd::emulation;
namespace {
namespace m = neverd::modules;
constexpr uint64_t Base = 0x1b0000000;
constexpr uint64_t LateBase = 0x1c0000000;
std::filesystem::path fixture(const char *Name,
                              const char *Extension = ".sys") {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + Extension);
}
llvm::Expected<ExecutionRunEvent> advance(ExecutionRun &Run) {
  for (unsigned N = 0; N < 100000; ++N) {
    auto E = Run.run();
    if (!E)
      return E.takeError();
    if (E->State == ExecutionRunState::Paused &&
        E->Reason == ExecutionRunReason::Quantum) {
      if (auto Error = Run.resume())
        return std::move(Error);
      continue;
    }
    return E;
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "module test action bound");
}
class KernelModulePublication
    : public testing::TestWithParam<ExecutionContract> {
protected:
  std::shared_ptr<m::ModuleSet> Sources;
  std::optional<PreparedDriverExecution> Driver;
  void SetUp() override {
    auto B = queryExecutionBackendBuild(ExecutionBackendKind::Unicorn,
                                        GuestArchitecture::X64);
    ASSERT_TRUE(bool(B));
    if (B->Availability != BackendAvailability::Available)
      GTEST_SKIP() << B->Reason;
    m::ModuleOptions O;
    O.Modules.push_back(
        {"support", fixture("kernel_module_support"),
         m::DebugInput{m::DebugKind::PDB,
                       fixture("kernel_module_support", ".pdb")}});
    Sources = llvm::cantFail(m::ModuleSet::create(std::move(O)));
  }
  ModuleBinding binding(std::string ID = "helper", uint64_t Address = Base,
                        std::string Name = "helper.sys") {
    return {std::move(ID), "support", std::move(Name), "kernel", {},
            Address,       {}};
  }
  void create(std::vector<ModuleBinding> Bindings = {},
              bool ResourcePauses = false, bool PageTables = false,
              uint64_t Quantum = 2) {
    DriverOptions O;
    O.Contract = GetParam();
    O.PageTables = PageTables;
    if (PageTables)
      O.Version = {10, 0, 19041};
    ExecutionRunOptions C;
    C.Limits = {O.InstructionLimit, O.EventLimit, O.TimeoutMilliseconds * 1000};
    C.PauseStages = {"after-entry"};
    C.EnableResourcePauses = ResourcePauses;
    C.InstructionQuantum = Quantum;
    C.ActionsPerRun = 1;
    C.Bindings = std::move(Bindings);
    auto P = createDriverExecutionRun(fixture("success"), O, C, Sources);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    Driver = std::move(*P);
  }
  void createPrimary(const char *Name, std::vector<ModuleBinding> Bindings = {},
                     bool PageTables = false, uint64_t Quantum = 2) {
    DriverOptions O;
    O.Contract = GetParam();
    O.PageTables = PageTables;
    if (PageTables)
      O.Version = {10, 0, 19041};
    O.LoadAddress = 0x190000000;
    ExecutionRunOptions C;
    C.Limits = {O.InstructionLimit, O.EventLimit, O.TimeoutMilliseconds * 1000};
    C.PauseStages = {"after-entry"};
    C.EnableResourcePauses = false;
    C.InstructionQuantum = Quantum;
    C.ActionsPerRun = 1;
    C.Bindings = std::move(Bindings);
    DriverSessionState::ResourceInputs Input;
    ASSERT_EQ(llvm::toString(Sources->add(
                  {"primary", fixture(Name),
                   m::DebugInput{m::DebugKind::PDB, fixture(Name, ".pdb")}})),
              "");
    Input.Image = llvm::cantFail(Sources->prepare("primary"));
    Input.ImageID = "primary";
    auto P = createDriverExecutionRun({}, O, C, Sources, std::move(Input));
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    Driver = std::move(*P);
  }
  uint64_t primaryValue(const char *Name) {
    auto Address = Driver->Run->modules().lookupSymbol("$driver", Name);
    if (!Address || !*Address) {
      ADD_FAILURE() << "missing primary symbol " << Name;
      if (!Address)
        llvm::consumeError(Address.takeError());
      return UINT64_MAX;
    }
    return llvm::cantFail(Driver->State->CPU->readInteger(**Address, 8));
  }
  void createSEH() {
    ASSERT_EQ(
        llvm::toString(Sources->add({"seh", fixture("kernel_module_seh")})),
        "");
    auto B = binding("seh", Base, "seh.sys");
    B.ModuleID = "seh";
    create({B});
    ASSERT_TRUE(Driver);
    stage();
  }
  uint64_t root() {
    return llvm::cantFail(Driver->State->CPU->readInteger(
        Driver->State->Kernel->driverObject() + windows::DriverSectionOffset,
        8));
  }
  void stage() {
    auto E = advance(*Driver->Run);
    ASSERT_TRUE(bool(E)) << llvm::toString(E.takeError());
    ASSERT_EQ(E->Reason, ExecutionRunReason::Stage) << E->Diagnostic;
  }
  uint64_t entry(const char *Name, const char *ID = "helper") {
    auto E = Driver->Run->modules().lookupExport(ID, Name);
    if (!E || !*E) {
      ADD_FAILURE() << "missing export " << Name;
      if (!E)
        llvm::consumeError(E.takeError());
      return 0;
    }
    return (**E).Address;
  }
  llvm::Expected<ExecutionRunEvent>
  call(const char *Name, std::vector<GuestInvocationArgument> Args = {},
       std::vector<GuestInvocationBuffer> Buffers = {}) {
    GuestInvocationRequest R;
    R.Entry = entry(Name);
    R.ABI = IntegerCallingConvention::Win64;
    R.Arguments = std::move(Args);
    R.Buffers = std::move(Buffers);
    if (auto E = Driver->Run->invoke(std::move(R)))
      return std::move(E);
    return advance(*Driver->Run);
  }
  uint32_t count() {
    auto E = call("SupportModules", {{0, 0}, {4096, {}}}, {{4096, {}, true}});
    if (!E || !E->Invocation) {
      ADD_FAILURE() << "query did not return";
      if (!E)
        llvm::consumeError(E.takeError());
      return 0;
    }
    EXPECT_EQ(E->Invocation->Value, 8 + 4 * 296);
    return llvm::support::endian::read32le(
        E->Invocation->Buffers[0].Bytes.data());
  }
};
TEST_P(KernelModulePublication,
       AdditionalActiveCfgUsesMixedWin64CheckAndDispatch) {
  for (const auto Variant : {"active", "inactive"}) {
    Driver.reset();
    const auto Name = std::string("kernel_module_guard_") + Variant;
    ASSERT_EQ(llvm::toString(Sources->add({Name, fixture(Name.c_str())})), "");
    auto B = binding("guard", Base, "guard.sys");
    B.ModuleID = Name;
    create({B});
    ASSERT_TRUE(Driver);
    auto *Policy = Driver->State->Guard.get();
    stage();
    EXPECT_EQ(Driver->State->Guard.get(), Policy);
    const auto I = Driver->Run->modules().find("guard");
    auto Data =
        std::static_pointer_cast<const KernelModuleRuntimeData>(I->RuntimeData);
    EXPECT_EQ(Policy->enabled(), Data->Guard.Enabled);
    const auto Check =
        *Driver->State->CPU->readInteger(Data->Guard.CheckPointerAddress, 8);
    EXPECT_EQ(Check == profile::GuardCheckThunk, Data->Guard.Enabled);
    for (unsigned N = 1; N <= 2; ++N) {
      GuestInvocationRequest R;
      R.Entry = entry("ModuleGuardSelf", "guard");
      R.ABI = IntegerCallingConvention::Win64;
      ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
      auto E = advance(*Driver->Run);
      ASSERT_TRUE(bool(E));
      ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete)
          << E->Diagnostic;
      EXPECT_EQ(E->Invocation->Value, 308u);
      EXPECT_EQ(*Driver->State->CPU->readInteger(
                    entry("ModuleGuardCount", "guard"), 8),
                N);
    }
  }
}
TEST_P(KernelModulePublication, ActiveCfgAdmitsOnlyExactNativeFunctionExports) {
  ASSERT_EQ(llvm::toString(
                Sources->add({"guard", fixture("kernel_module_guard_active")})),
            "");
  for (unsigned InvalidStage = 0; InvalidStage <= 2; ++InvalidStage) {
    Driver.reset();
    auto B = binding("guard", Base, "guard.sys");
    B.ModuleID = "guard";
    create({B, binding("provider", LateBase, "provider.sys")});
    ASSERT_TRUE(Driver);
    stage();
    GuestInvocationRequest R;
    R.Entry = entry("ModuleGuardCall", "guard");
    R.ABI = IntegerCallingConvention::Win64;
    R.Arguments = {{entry("SupportProbe", "provider"), {}}, {InvalidStage, {}}};
    ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
    auto E = advance(*Driver->Run);
    ASSERT_TRUE(bool(E));
    if (!InvalidStage) {
      ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete)
          << E->Diagnostic;
      EXPECT_EQ(E->Invocation->Value, 43u);
    } else {
      EXPECT_EQ(E->State, ExecutionRunState::Failed);
      EXPECT_NE(E->Diagnostic.find("declared entry point"), std::string::npos);
    }
  }
}
TEST_P(KernelModulePublication,
       LaterActiveCfgPublicationKeepsTheSuspendedPolicyOwner) {
  ASSERT_EQ(llvm::toString(
                Sources->add({"guard", fixture("kernel_module_guard_active")})),
            "");
  create({binding()});
  ASSERT_TRUE(Driver);
  stage();
  auto *Owner = Driver->State->Guard.get();
  EXPECT_FALSE(Owner->enabled());
  auto B = binding("guard", LateBase, "guard.sys");
  B.ModuleID = "guard";
  ASSERT_EQ(llvm::toString(Driver->Run->declareModule(B)), "");
  auto Prepared = llvm::cantFail(Sources->prepare("guard"));
  auto Plan = llvm::cantFail(
      planKernelGuestModule(B, *Prepared, Driver->State->Options.MemoryLimit));
  auto Function = llvm::find_if(
      Plan.Exports, [](const auto &E) { return E.Name == "ModuleGuardSelf"; });
  ASSERT_NE(Function, Plan.Exports.end());
  auto E = call("SupportIndirect", {{Function->Address, {}}, {1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 351u);
  EXPECT_EQ(Driver->State->Guard.get(), Owner);
  EXPECT_TRUE(Owner->enabled());
  EXPECT_TRUE(Driver->State->GuardModuleIDs.contains("guard"));
}
TEST_P(KernelModulePublication,
       ChangedActiveCfgMetadataCannotUseCachedTargets) {
  ASSERT_EQ(llvm::toString(
                Sources->add({"guard", fixture("kernel_module_guard_active")})),
            "");
  auto B = binding("guard", Base, "guard.sys");
  B.ModuleID = "guard";
  create({B});
  ASSERT_TRUE(Driver);
  stage();
  auto I = Driver->Run->modules().find("guard");
  auto Data =
      std::static_pointer_cast<const KernelModuleRuntimeData>(I->RuntimeData);
  ASSERT_FALSE(Data->Guard.MetadataRanges.empty());
  const uint64_t At = I->Base + Data->Guard.MetadataRanges.front().RVA + 4;
  const uint8_t Changed[] = {
      uint8_t(*Driver->State->CPU->readInteger(At, 1) ^ 1)};
  ASSERT_EQ(llvm::toString(Driver->State->CPU->writeBacking(At, Changed)), "");
  GuestInvocationRequest R;
  R.Entry = entry("ModuleGuardSelf", "guard");
  R.ABI = IntegerCallingConvention::Win64;
  ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(E->Diagnostic.find("CFG metadata or helper binding changed"),
            std::string::npos);
}
TEST_P(KernelModulePublication,
       FailedRamPublicationCannotEnableCfgOrGrantTargets) {
  DriverOptions O;
  O.Contract = GetParam();
  auto State =
      llvm::cantFail(DriverSessionState::create(fixture("success"), O));
  ASSERT_EQ(llvm::toString(
                Sources->add({"guard", fixture("kernel_module_guard_active")})),
            "");
  auto B = binding("guard", Base, "guard.sys");
  B.ModuleID = "guard";
  B.Space = State->CPU->addressSpace();
  auto Bindings = llvm::cantFail(ModuleBindings::create(
      Sources, 256,
      [&](llvm::ArrayRef<ModuleInstance> Preview)
          -> llvm::Expected<std::unique_ptr<ModulePublication>> {
        auto P = prepareDriverImportPublication(*State, Preview);
        if (!P)
          return P.takeError();
        auto E =
            prepareDriverExceptionPublication(*State, Preview, std::move(*P));
        if (!E)
          return E.takeError();
        return prepareDriverGuardPublication(*State, Preview, std::move(*E));
      },
      [&](auto Existing, auto Candidates) {
        return finalizeKernelGuestModules(Existing, Candidates, State->Exports);
      }));
  ASSERT_EQ(llvm::toString(adoptDriverBootstrapModules(*State, *Bindings)), "");
  const auto Owner = State->Guard.get();
  const auto Physical = B.Space->physicalMemory();
  auto Filled = llvm::cantFail(
      Physical->allocate(Physical->limit() - Physical->allocatedBytes()));
  auto Builder = [&](const auto &Binding, const auto &Image) {
    return planKernelGuestModule(Binding, Image, State->Options.MemoryLimit);
  };
  auto E = Bindings->materialize({B}, Builder);
  ASSERT_TRUE(bool(E));
  EXPECT_TRUE(E.isA<GuestMemoryLimitError>());
  llvm::consumeError(std::move(E));
  EXPECT_EQ(State->Guard.get(), Owner);
  EXPECT_FALSE(Owner->enabled());
  EXPECT_TRUE(State->GuardModuleIDs.empty());
  Filled.reset();
  ASSERT_EQ(llvm::toString(Bindings->materialize({B}, Builder)), "");
  EXPECT_EQ(State->Guard.get(), Owner);
  EXPECT_TRUE(Owner->enabled());
  EXPECT_TRUE(State->GuardModuleIDs.contains("guard"));
}
TEST_P(KernelModulePublication,
       LazyActiveCfgModulePublishesAtTheActualFetchStop) {
  ASSERT_EQ(llvm::toString(Sources->add({"guard",
                                         fixture("kernel_module_guard_active"),
                                         {},
                                         m::LoadPolicy::Lazy})),
            "");
  auto B = binding("guard", LateBase, "guard.sys");
  B.ModuleID = "guard";
  auto Source = llvm::cantFail(Sources->prepare("guard"));
  auto Plan = llvm::cantFail(
      planKernelGuestModule(B, *Source, profile::DefaultMemoryLimit));
  B.Size = Plan.Size;
  const auto Function = llvm::find_if(
      Plan.Exports, [](const auto &E) { return E.Name == "ModuleGuardSelf"; });
  ASSERT_NE(Function, Plan.Exports.end());
  create({binding(), B}, true);
  ASSERT_TRUE(Driver);
  stage();
  EXPECT_FALSE(Driver->Run->modules().find("guard")->mapped());
  auto E = call("SupportIndirect", {{Function->Address, {}}, {1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 351u);
  EXPECT_TRUE(Driver->Run->modules().find("guard")->mapped());
  EXPECT_TRUE(Driver->State->Guard->enabled());
  EXPECT_EQ(
      *Driver->State->CPU->readInteger(entry("ModuleGuardCount", "guard"), 8),
      1u);
}
TEST(KernelGuestPagingRanges, CompleteOwnedWindowsUseExactByteBounds) {
  using P = KernelGuestPageTables;
  EXPECT_TRUE(P::overlapsReservedMemory(P::TableBase, 1));
  EXPECT_TRUE(P::overlapsReservedMemory(P::TableBase - 1, 2));
  EXPECT_TRUE(P::overlapsReservedMemory(P::TableBase + P::TableBytes - 1, 1));
  EXPECT_FALSE(P::overlapsReservedMemory(P::TableBase - 4096, 4096));
  EXPECT_FALSE(P::overlapsReservedMemory(P::TableBase + P::TableBytes, 4096));
  EXPECT_TRUE(P::overlapsReservedMemory(P::pteBase() - 4096, 8192));
  EXPECT_TRUE(P::overlapsReservedMemory(P::pteBase() + (1ULL << 39) - 1, 1));
  EXPECT_FALSE(P::overlapsReservedMemory(P::pteBase() - 4096, 4096));
  EXPECT_FALSE(P::overlapsReservedMemory(P::pteBase() + (1ULL << 39), 4096));
  EXPECT_TRUE(P::overlapsReservedMemory(UINT64_MAX, 2));
  EXPECT_TRUE(P::overlapsReservedMemory(0, 0));
}
TEST_P(KernelModulePublication, EagerModulePtesExistBeforeItsFirstInstruction) {
  ASSERT_EQ(
      llvm::toString(Sources->add({"paging", fixture("kernel_module_paging")})),
      "");
  auto B = binding("paging", Base, "kernel_module_paging.sys");
  B.ModuleID = "paging";
  createPrimary("kernel_module_paging_primary", {B}, true, 100000);
  ASSERT_TRUE(Driver);
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->Reason, ExecutionRunReason::Stage) << E->Diagnostic;
  EXPECT_EQ(Driver->State->Result.NTStatus, 0u);
  EXPECT_EQ(*Driver->State->CPU->readInteger(entry("PagingCalls", "paging"), 8),
            1u);
}
TEST_P(KernelModulePublication, RepairedCpuReadSeesTheNewPteWithoutAReplay) {
  ASSERT_EQ(
      llvm::toString(Sources->add({"paging", fixture("kernel_module_paging")})),
      "");
  auto B = binding("paging", Base, "paging.sys");
  B.ModuleID = "paging";
  create({B}, true, true, 100000);
  ASSERT_TRUE(Driver);
  stage();
  constexpr uint64_t HighBase = 0xfffff88010000000ULL;
  const auto Target = entry("PagingValue", "paging") + HighBase - Base;
  GuestInvocationRequest R;
  R.Entry = entry("ModulePagingAfterRead", "paging");
  R.ABI = IntegerCallingConvention::Win64;
  R.Arguments = {{Target, {}},
                 {KernelGuestPageTables::pteBase(), {}},
                 {0x8000000000000003ULL, {}}};
  ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->State, ExecutionRunState::AwaitingResource) << E->Diagnostic;
  ASSERT_TRUE(E->Request && E->Resource);
  EXPECT_EQ(E->Request->Address, Target);
  auto Counter = Driver->Run->readMemory(entry("PagingCalls", "paging"), 8);
  ASSERT_TRUE(bool(Counter));
  EXPECT_EQ(llvm::support::endian::read64le(Counter->data()), 1u);
  auto Late = binding("late-paging", HighBase, "late-paging.sys");
  Late.ModuleID = "paging";
  ASSERT_EQ(llvm::toString(Driver->Run->supply(E->Request->Token, {Late})), "");
  auto Done = advance(*Driver->Run);
  ASSERT_TRUE(bool(Done));
  ASSERT_EQ(Done->Reason, ExecutionRunReason::InvocationComplete)
      << Done->Diagnostic;
  EXPECT_EQ(Done->Invocation->Value, 42u);
  EXPECT_EQ(*Driver->State->CPU->readInteger(entry("PagingCalls", "paging"), 8),
            1u);
  EXPECT_EQ(
      *Driver->State->CPU->readInteger(entry("PagingCalls", "late-paging"), 8),
      0u);
}
TEST_P(KernelModulePublication,
       PagingNamespacesRejectModulesBeforePublication) {
  for (const auto At : {KernelGuestPageTables::TableBase,
                        KernelGuestPageTables::pteBase() + 0x4000000}) {
    Driver.reset();
    create({binding()}, false, true, 100000);
    ASSERT_TRUE(Driver);
    stage();
    auto Forbidden = binding("forbidden", At, "forbidden.sys");
    ASSERT_EQ(llvm::toString(Driver->Run->declareModule(Forbidden)), "");
    const auto Space = Driver->State->CPU->addressSpace();
    const auto Generation = Space->mappingGeneration();
    const auto Instructions = Driver->Run->inspect()->Instructions;
    auto E = call("SupportProbe", {{0, {}}});
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(E->Reason, ExecutionRunReason::EnvironmentFailure);
    EXPECT_NE(E->Diagnostic.find("reserved guest page-table memory"),
              std::string::npos)
        << E->Diagnostic;
    EXPECT_FALSE(Driver->Run->modules().find("forbidden")->mapped());
    EXPECT_EQ(Space->mappingGeneration(), Generation);
    EXPECT_EQ(Driver->Run->inspect()->Instructions, Instructions);
  }
}
TEST_P(KernelModulePublication, MissingRecursivePteIsNotAMissingModule) {
  ASSERT_EQ(
      llvm::toString(Sources->add({"paging", fixture("kernel_module_paging")})),
      "");
  auto B = binding("paging", Base, "paging.sys");
  B.ModuleID = "paging";
  create({B}, true, true, 100000);
  ASSERT_TRUE(Driver);
  stage();
  // No real table aliases this part of the reserved recursive window.
  GuestInvocationRequest R;
  R.Entry = entry("ModulePagingAfterRead", "paging");
  R.ABI = IntegerCallingConvention::Win64;
  R.Arguments = {{KernelGuestPageTables::pteBase() + 0x4000000, {}},
                 {KernelGuestPageTables::pteBase(), {}},
                 {1, {}}};
  ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_FALSE(E->Request);
  EXPECT_NE(E->Reason, ExecutionRunReason::Resource);
}
TEST_P(KernelModulePublication, PagingProjectionFailureKeepsModelErrorCleanup) {
  create({}, false, true, 100000);
  ASSERT_TRUE(Driver);
  // Inject an invalid internal mapping, independently of the module mapper.
  ASSERT_EQ(llvm::toString(Driver->State->CPU->addressSpace()->map(
                KernelGuestPageTables::pteBase() + 0x4000000, profile::PageSize,
                Read)),
            "");
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->Reason, ExecutionRunReason::EnvironmentFailure);
  EXPECT_NE(E->Diagnostic.find("recursive page-table window"),
            std::string::npos);
  EXPECT_EQ(Driver->State->Result.Stop, DriverStopReason::ModelError);
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  ASSERT_EQ(llvm::toString(Driver->finishStoppedFailure(*E)), "");
  EXPECT_EQ(Driver->State->Result.Stop, DriverStopReason::ModelError);
}
TEST_P(KernelModulePublication,
       BootstrapDriverUsesTheInstanceTableWithoutDuplicatingItsLoaderRecord) {
  create();
  ASSERT_TRUE(Driver);
  const auto Rows = Driver->Run->modules().inspect();
  ASSERT_EQ(Rows.size(), 1u);
  const auto &I = Rows.front();
  EXPECT_EQ(I.Binding.ID, DriverBootstrapID);
  EXPECT_EQ(I.Binding.ModuleID, "@driver-image");
  EXPECT_EQ(I.Binding.GuestName, Driver->State->Options.ServiceName + ".sys");
  EXPECT_TRUE(I.EnvironmentOwned);
  EXPECT_EQ(I.Role, "driver");
  EXPECT_EQ(I.Source, Driver->State->Inputs.Image);
  EXPECT_EQ(I.Base, Driver->State->Image->Base);
  EXPECT_EQ(I.Entry, Driver->State->Image->Entry);
  EXPECT_EQ(Driver->State->Result.Instructions, 0u);
  const auto Record = root();
  const auto Space = Driver->State->CPU->addressSpace();
  const auto Generation = Space->mappingGeneration();
  const auto Allocated = Space->physicalMemory()->allocatedBytes();
  auto P = Driver->State->Kernel->prepareModulePublication(Rows);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_TRUE((*P)->patches().empty());
  {
    auto Lease = llvm::cantFail(Space->prepareMetadataPublication());
    llvm::cantFail(Lease->commit());
    (*P)->commit();
  }
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record);
  auto Removed = Driver->State->Kernel->prepareModulePublication({});
  ASSERT_FALSE(bool(Removed));
  EXPECT_NE(llvm::toString(Removed.takeError()).find("withdrawing"),
            std::string::npos);
  stage();
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record);
}
TEST_P(KernelModulePublication,
       NativeCSehUsesItsOwnUnwindTableAndRetainsMutableGlobals) {
  createSEH();
  ASSERT_TRUE(Driver);
  ASSERT_TRUE(Driver->State->ExceptionModuleIDs.contains("seh"));
  for (unsigned N = 1; N <= 2; ++N) {
    GuestInvocationRequest R;
    R.Entry = entry("ModuleSEHCatch", "seh");
    R.ABI = IntegerCallingConvention::Win64;
    R.Arguments = {{7, {}}};
    ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
    auto E = advance(*Driver->Run);
    ASSERT_TRUE(bool(E));
    ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete)
        << E->Diagnostic;
    EXPECT_EQ(E->Invocation->Value, 74u);
    auto Counter = Driver->Run->modules().lookupExport("seh", "ModuleSEHCount");
    ASSERT_TRUE(bool(Counter) && bool(*Counter));
    EXPECT_EQ(*Driver->State->CPU->readInteger((**Counter).Address, 8), N);
  }
  EXPECT_EQ(Driver->State->Result.Calls.size(), 2u);
}
TEST_P(KernelModulePublication,
       LaterNativePublicationPreservesCrossImageUnwindOwnership) {
  createSEH();
  ASSERT_TRUE(Driver);
  const auto Throw = entry("ModuleSEHThrow", "seh") + LateBase - Base;
  auto B = binding("aaa-late", LateBase, "late-seh.sys");
  B.ModuleID = "seh";
  ASSERT_EQ(llvm::toString(Driver->Run->declareModule(B)), "");
  GuestInvocationRequest R;
  R.Entry = entry("ModuleSEHCall", "seh");
  R.ABI = IntegerCallingConvention::Win64;
  R.Arguments = {{Throw, {}}, {7, {}}};
  ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 140u);
  EXPECT_EQ(Driver->State->ExceptionModuleIDs.size(), 2u);
  EXPECT_TRUE(Driver->Run->modules().find("aaa-late")->mapped());
}
TEST_P(KernelModulePublication,
       ChangedNativeUnwindDirectoryCannotUseCachedFacts) {
  createSEH();
  ASSERT_TRUE(Driver);
  const auto I = Driver->Run->modules().find("seh");
  auto Data =
      std::static_pointer_cast<const KernelModuleRuntimeData>(I->RuntimeData);
  const uint64_t At = I->Base + Data->Unwind.Metadata->DirectoryRVA;
  ASSERT_NE(Data->Unwind.Metadata->DirectorySize, 0u);
  const uint8_t Changed[] = {
      uint8_t(*Driver->State->CPU->readInteger(At, 1) ^ 1)};
  ASSERT_EQ(llvm::toString(Driver->State->CPU->writeBacking(At, Changed)), "");
  GuestInvocationRequest R;
  R.Entry = entry("ModuleSEHCatch", "seh");
  R.ABI = IntegerCallingConvention::Win64;
  R.Arguments = {{7, {}}};
  ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(
      E->Diagnostic.find("exception metadata or personality binding changed"),
      std::string::npos);
}
TEST_P(KernelModulePublication,
       FailedRamPublicationDoesNotReplaceTheUnwindOwner) {
  DriverOptions O;
  O.Contract = GetParam();
  auto State =
      llvm::cantFail(DriverSessionState::create(fixture("success"), O));
  ASSERT_EQ(llvm::toString(Sources->add({"seh", fixture("kernel_module_seh")})),
            "");
  auto B = binding("seh", Base, "seh.sys");
  B.ModuleID = "seh";
  B.Space = State->CPU->addressSpace();
  auto Bindings = llvm::cantFail(ModuleBindings::create(
      Sources, 256,
      [&](llvm::ArrayRef<ModuleInstance> Preview)
          -> llvm::Expected<std::unique_ptr<ModulePublication>> {
        auto Model = prepareDriverImportPublication(*State, Preview);
        if (!Model)
          return Model.takeError();
        return prepareDriverExceptionPublication(*State, Preview,
                                                 std::move(*Model));
      },
      [&](auto Existing, auto Candidates) {
        return finalizeKernelGuestModules(Existing, Candidates, State->Exports);
      }));
  ASSERT_EQ(llvm::toString(adoptDriverBootstrapModules(*State, *Bindings)), "");
  const auto Planner = State->Exceptions.get();
  const auto Physical = B.Space->physicalMemory();
  auto Filled = llvm::cantFail(
      Physical->allocate(Physical->limit() - Physical->allocatedBytes()));
  auto Builder = [&](const auto &Binding, const auto &Image) {
    return planKernelGuestModule(Binding, Image, State->Options.MemoryLimit);
  };
  auto Error = Bindings->materialize({B}, Builder);
  ASSERT_TRUE(bool(Error));
  EXPECT_TRUE(Error.isA<GuestMemoryLimitError>());
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(State->Exceptions.get(), Planner);
  EXPECT_TRUE(State->ExceptionModuleIDs.empty());
  EXPECT_FALSE(Bindings->find("seh"));
  Filled.reset();
  ASSERT_EQ(llvm::toString(Bindings->materialize({B}, Builder)), "");
  EXPECT_NE(State->Exceptions.get(), Planner);
  EXPECT_TRUE(State->ExceptionModuleIDs.contains("seh"));
}

TEST_P(KernelModulePublication,
       ChangedNativePersonalityIatCannotRetainItsCachedIdentity) {
  createSEH();
  ASSERT_TRUE(Driver);
  const auto I = Driver->Run->modules().find("seh");
  const auto &Image = I->Source->image();
  const auto Import = std::find_if(
      Image.Imports.begin(), Image.Imports.end(),
      [](const auto &X) { return X.Name == "__C_specific_handler"; });
  ASSERT_NE(Import, Image.Imports.end());
  const auto Slot = I->Base + Import->IATAddr - Image.Base;
  const uint64_t Changed = *Driver->State->CPU->readInteger(Slot, 8) + 16;
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, Changed);
  ASSERT_EQ(llvm::toString(Driver->State->CPU->writeBacking(Slot, Bytes)), "");
  GuestInvocationRequest R;
  R.Entry = entry("ModuleSEHCatch", "seh");
  R.ABI = IntegerCallingConvention::Win64;
  R.Arguments = {{7, {}}};
  ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(
      E->Diagnostic.find("exception metadata or personality binding changed"),
      std::string::npos);
}
TEST_P(KernelModulePublication,
       PrimaryNamedAndDataImportsCommitBeforeAnyCpuAction) {
  ASSERT_EQ(llvm::toString(
                Sources->add({"provider", fixture("kernel_module_provider")})),
            "");
  auto B = binding("provider", LateBase, "PROVIDER.SYS");
  B.ModuleID = "provider";
  createPrimary("driver_module_imports_named", {B});
  ASSERT_TRUE(Driver);
  EXPECT_FALSE(Driver->State->ModuleImportsLinked);
  EXPECT_EQ(primaryValue("PrimaryEntryCount"), 0u);
  ASSERT_EQ(Driver->State->ModuleImports.size(), 2u);
  for (const auto &I : Driver->State->ModuleImports)
    EXPECT_EQ(*Driver->State->CPU->readInteger(I.Slot, 8), 0u);
  stage();
  EXPECT_TRUE(Driver->State->ModuleImportsLinked);
  EXPECT_EQ(primaryValue("PrimaryEntryCount"), 1u);
  EXPECT_EQ(primaryValue("PrimaryObserved"), 85u);
  EXPECT_EQ(Driver->State->Result.NTStatus, 0u);
  for (const auto &I : Driver->State->ModuleImports) {
    auto E = Driver->Run->modules().lookupExport("provider", I.Name);
    ASSERT_TRUE(bool(E) && bool(*E));
    EXPECT_EQ(*Driver->State->CPU->readInteger(I.Slot, 8), (**E).Address);
    EXPECT_FALSE(*Driver->State->CPU->canAccess(I.Slot, 8, Write));
    EXPECT_FALSE(*Driver->State->Exports.findImport(I));
  }
  ASSERT_EQ(Driver->State->Result.Calls.size(), 1u);
  EXPECT_EQ(Driver->State->Result.Calls.front().Name, "KeGetCurrentIrql");
}
TEST_P(KernelModulePublication,
       PrimaryRequiredLazyProviderMapsDuringInitialLink) {
  ASSERT_EQ(llvm::toString(Sources->add({"provider",
                                         fixture("kernel_module_provider"),
                                         {},
                                         m::LoadPolicy::Lazy})),
            "");
  ASSERT_EQ(llvm::toString(Sources->add(
                {"unused", fixture("absent_unused"), {}, m::LoadPolicy::Lazy})),
            "");
  auto B = binding("provider", LateBase, "provider.sys");
  B.ModuleID = "provider";
  createPrimary("driver_module_imports_named", {B});
  ASSERT_TRUE(Driver);
  EXPECT_FALSE(Sources->prepared("provider"));
  stage();
  EXPECT_TRUE(Sources->prepared("provider"));
  EXPECT_FALSE(Sources->prepared("unused"));
  EXPECT_TRUE(Driver->Run->modules().find("provider")->mapped());
  EXPECT_EQ(primaryValue("PrimaryEntryCount"), 1u);
  EXPECT_EQ(primaryValue("PrimaryObserved"), 85u);
}
TEST_P(KernelModulePublication,
       PrimaryMissingOrdinalProviderRetainsTheActionAcrossFailedSupply) {
  createPrimary("driver_module_imports_ordinal");
  ASSERT_TRUE(Driver);
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E) && bool(E->Request));
  ASSERT_EQ(E->State, ExecutionRunState::AwaitingResource) << E->Diagnostic;
  EXPECT_EQ(E->Request->Kind, ModuleRequestKind::MissingDependency);
  EXPECT_EQ(E->Request->Dependency, "provider.sys");
  EXPECT_FALSE(E->Resource);
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  EXPECT_EQ(primaryValue("PrimaryEntryCount"), 0u);
  const auto Record = root();
  const auto Request = E->Request->Token;
  ASSERT_EQ(
      llvm::toString(Sources->add({"provider", fixture("missing_provider")})),
      "");
  auto B = binding("provider", LateBase, "provider.sys");
  B.ModuleID = "provider";
  EXPECT_NE(llvm::toString(Driver->Run->supply(Request, {B})), "");
  EXPECT_EQ(Driver->Run->modules().pendingRequest()->Token, Request);
  EXPECT_FALSE(Driver->Run->modules().find("provider"));
  EXPECT_FALSE(Driver->State->ModuleImportsLinked);
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record);
  for (const auto &I : Driver->State->ModuleImports)
    EXPECT_EQ(*Driver->State->CPU->readInteger(I.Slot, 8), 0u);
  ASSERT_EQ(llvm::toString(Sources->reviseUnprepared(
                "provider", fixture("kernel_module_provider"))),
            "");
  ASSERT_EQ(llvm::toString(Driver->Run->supply(Request, {B})), "");
  EXPECT_TRUE(Driver->State->ModuleImportsLinked);
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  EXPECT_EQ(primaryValue("PrimaryEntryCount"), 0u);
  stage();
  EXPECT_EQ(primaryValue("PrimaryEntryCount"), 1u);
  EXPECT_EQ(primaryValue("PrimaryObserved"), 199u);
  EXPECT_EQ(Driver->State->Result.NTStatus, 0u);
  ASSERT_EQ(Driver->State->Result.Calls.size(), 1u);
}
TEST_P(KernelModulePublication,
       PrimaryDeclaredMissingProviderCanBeCorrectedBeforeEntry) {
  ASSERT_EQ(
      llvm::toString(Sources->add(
          {"provider", fixture("missing_provider"), {}, m::LoadPolicy::Lazy})),
      "");
  auto B = binding("provider", LateBase, "provider.sys");
  B.ModuleID = "provider";
  createPrimary("driver_module_imports_named", {B});
  ASSERT_TRUE(Driver);
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E) && bool(E->Request));
  EXPECT_EQ(E->State, ExecutionRunState::AwaitingResource);
  EXPECT_EQ(E->Request->InstanceID, "provider");
  EXPECT_EQ(E->Request->ModuleID, "provider");
  EXPECT_FALSE(Driver->Run->modules().find("provider")->mapped());
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  ASSERT_EQ(llvm::toString(Sources->reviseUnprepared(
                "provider", fixture("kernel_module_provider"))),
            "");
  ASSERT_EQ(llvm::toString(Driver->Run->supply(E->Request->Token, {B})), "");
  EXPECT_EQ(primaryValue("PrimaryEntryCount"), 0u);
  stage();
  EXPECT_EQ(primaryValue("PrimaryEntryCount"), 1u);
  EXPECT_EQ(primaryValue("PrimaryObserved"), 85u);
}
TEST_P(KernelModulePublication,
       GuestModuleCannotReplaceAnExistingKernelIdentity) {
  ASSERT_EQ(llvm::toString(Sources->add(
                {"model-provider", fixture("kernel_module_model_provider")})),
            "");
  auto B = binding("model-provider", LateBase, "NTOSKRNL.EXE");
  B.ModuleID = "model-provider";
  create({binding(), B});
  ASSERT_TRUE(Driver);
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(E->Diagnostic.find("aliases an existing kernel identity"),
            std::string::npos);
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  EXPECT_FALSE(Driver->Run->modules().find("model-provider")->mapped());
}
TEST_P(KernelModulePublication,
       PrimaryOrdinalAdmissionKeepsTheLegacyStrictProfile) {
  auto Source = llvm::cantFail(m::ModuleSet::create({}));
  ASSERT_EQ(llvm::toString(Source->add(
                {"primary", fixture("driver_module_imports_ordinal")})),
            "");
  auto Image = llvm::cantFail(Source->prepare("primary"));
  auto Strict = loadDriverImage(*Image, 64 * 1024 * 1024);
  ASSERT_FALSE(bool(Strict));
  EXPECT_NE(llvm::toString(Strict.takeError()).find("ordinal"),
            std::string::npos);
  auto Linked = loadDriverImage(*Image, 64 * 1024 * 1024, 0x190000000, true);
  ASSERT_TRUE(bool(Linked)) << llvm::toString(Linked.takeError());
  EXPECT_EQ(std::count_if(Linked->Imports.begin(), Linked->Imports.end(),
                          [](const auto &I) { return bool(I.Ordinal); }),
            1u);
  DriverOptions O;
  O.Contract = GetParam();
  auto Legacy = emulateDriver(fixture("driver_module_imports_named"), O);
  ASSERT_TRUE(bool(Legacy)) << llvm::toString(Legacy.takeError());
  EXPECT_EQ(Legacy->Stop, DriverStopReason::UnsupportedAPI);
  EXPECT_NE(Legacy->Diagnostic.find("provider.sys!SupportProbe"),
            std::string::npos);
}
TEST_P(KernelModulePublication,
       ReferenceInstanceKeepsModelExportsAndOpaqueGlobalsUnderExistingOwners) {
  DriverOptions O;
  O.Contract = GetParam();
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.KernelExports["ZwQuerySystemInformation"] = true;
  O.KernelExports["ExEventObjectType"] = true;
  O.KernelImage = fixture("driver_kernel_debugger_reference");
  O.KernelPDB = fixture("driver_kernel_debugger_reference", ".pdb");
  ExecutionRunOptions C;
  C.Limits = {O.InstructionLimit, O.EventLimit, O.TimeoutMilliseconds * 1000};
  C.PauseStages = {"after-entry"};
  auto P = createDriverExecutionRun(fixture("success"), O, C, Sources);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  Driver = std::move(*P);
  auto DataSlot = Driver->State->Exports.existingImport(
      {0, "ntoskrnl.exe", "ExEventObjectType"});
  ASSERT_TRUE(bool(DataSlot)) << llvm::toString(DataSlot.takeError());
  auto Type = Driver->State->CPU->readInteger(*DataSlot, 8);
  ASSERT_TRUE(bool(Type)) << llvm::toString(Type.takeError());
  EXPECT_NE(*Type, 0u);
  EXPECT_FALSE(llvm::cantFail(
      Driver->State->CPU->addressSpace()->canAccess(*DataSlot, 8, Write)));
  const auto I = Driver->Run->modules().find(KernelReferenceBootstrapID);
  ASSERT_TRUE(I && I->mapped());
  EXPECT_EQ(Driver->Run->modules().inspect().size(), 2u);
  EXPECT_TRUE(I->EnvironmentOwned);
  EXPECT_EQ(I->Role, "kernel-reference");
  EXPECT_EQ(I->Source, Driver->State->Inputs.Kernel);
  EXPECT_EQ(I->Base, Driver->State->Reference->base());
  EXPECT_EQ(I->Entry, 0u);
  auto E = Driver->Run->modules().lookupExport(KernelReferenceBootstrapID,
                                               "KeGetCurrentIrql");
  ASSERT_TRUE(bool(E) && bool(*E));
  EXPECT_EQ((**E).Address, *Driver->State->Exports.resolve("KeGetCurrentIrql"));
  ASSERT_TRUE((**E).Ordinal);
  auto Ordinal = Driver->Run->modules().lookupExport(KernelReferenceBootstrapID,
                                                     *(**E).Ordinal);
  ASSERT_TRUE(bool(Ordinal) && bool(*Ordinal));
  EXPECT_EQ((**Ordinal).Address, (**E).Address);
  auto Symbol = Driver->Run->modules().lookupSymbol(KernelReferenceBootstrapID,
                                                    "KeGetCurrentIrql");
  ASSERT_TRUE(bool(Symbol) && bool(*Symbol));
  EXPECT_EQ(**Symbol, (**E).Address);
  // A separate HAL identity must not change publication of the kernel's
  // authenticated export table. Name-only dynamic lookup remains ambiguous.
  auto HAL =
      Driver->State->Exports.bindImport({0, "hal.dll", "KeGetCurrentIrql"});
  ASSERT_TRUE(bool(HAL)) << llvm::toString(HAL.takeError());
  EXPECT_NE(*HAL, (**E).Address);
  auto Ambiguous = Driver->State->Exports.resolve("KeGetCurrentIrql");
  ASSERT_FALSE(bool(Ambiguous));
  EXPECT_NE(llvm::toString(Ambiguous.takeError()).find("ambiguous"),
            std::string::npos);
  auto Bootstrap = llvm::cantFail(ModuleBindings::create(Sources));
  ASSERT_EQ(
      llvm::toString(adoptDriverBootstrapModules(*Driver->State, *Bootstrap)),
      "");
  auto Exact =
      Bootstrap->lookupExport(KernelReferenceBootstrapID, "KeGetCurrentIrql");
  ASSERT_TRUE(bool(Exact) && bool(*Exact));
  EXPECT_EQ((**Exact).Address, (**E).Address);
  EXPECT_EQ(Driver->State->Result.Instructions, 0u);
  const auto Record = root();
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record);
  // An exported data address does not make its file initializer runtime state.
  auto Opaque =
      Driver->State->Reference->exportData().names().find("CommitCounter");
  ASSERT_NE(Opaque, Driver->State->Reference->exportData().names().end());
  EXPECT_NE(llvm::toString(Driver->State->Kernel->validateGuestAccess(
                I->Base + Opaque->second, 8, false)),
            "");
  auto B = binding();
  B.Space = Driver->State->CPU->addressSpace();
  ASSERT_EQ(llvm::toString(Driver->Run->declareModule(B)), "");
  stage();
  EXPECT_EQ(Driver->Run->modules().inspect().size(), 3u);
  auto Result = call("SupportProbe", {{1, {}}});
  ASSERT_TRUE(bool(Result) && bool(Result->Invocation));
  EXPECT_EQ(Result->Invocation->Value, 43u);
}
TEST_P(KernelModulePublication,
       EagerCommitPublishesCodeSymbolsAndTheLiveLoaderRing) {
  create({binding()});
  ASSERT_TRUE(Driver);
  const auto Initial = root();
  EXPECT_EQ(*Driver->State->CPU->readInteger(Initial, 8), Initial);
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
  stage();
  ASSERT_EQ(Driver->Run->state(), ExecutionRunState::Paused);
  const auto I = Driver->Run->modules().find("helper");
  ASSERT_TRUE(I && I->mapped());
  EXPECT_EQ(I->Base, Base);
  auto Data = Driver->Run->modules().lookupExport("helper", "SupportValue");
  ASSERT_TRUE(bool(Data));
  ASSERT_TRUE(*Data);
  EXPECT_EQ((**Data).Kind, m::SymbolKind::Data);
  EXPECT_EQ(*Driver->State->CPU->readInteger((**Data).Address, 8), 42u);
  ASSERT_TRUE((**Data).Ordinal);
  auto Ordinal =
      Driver->Run->modules().lookupExport("helper", *(**Data).Ordinal);
  ASSERT_TRUE(bool(Ordinal));
  ASSERT_TRUE(*Ordinal);
  EXPECT_EQ((**Ordinal).Address, (**Data).Address);
  auto Symbol = Driver->Run->modules().lookupSymbol("helper", "SupportProbe");
  ASSERT_TRUE(bool(Symbol));
  EXPECT_EQ(*Symbol, entry("SupportProbe"));
  auto E = call("SupportProbe", {{1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 43u); // Entry would change the value to 1000.
  EXPECT_EQ(count(), 4u);
  E = call("SupportRing", {{Initial, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  EXPECT_EQ(E->Invocation->Value, 2u);
  E = call("SupportRecord", {{Initial, {}}, {Base, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  const auto Record = E->Invocation->Value;
  ASSERT_NE(Record, 0u);
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record + 0x30, 8), Base);
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record + 8, 8), Initial);
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Initial);
}
TEST_P(KernelModulePublication, GlobalsAndApiEffectsUseTheSameRetainedPump) {
  create({binding()});
  stage();
  auto E = call("SupportSet", {{99, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  EXPECT_EQ(E->Invocation->Value, 99u);
  E = call("SupportProbe", {{7, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  EXPECT_EQ(E->Invocation->Value, 106u);
  EXPECT_EQ(Driver->State->Result.Calls.back().Name, "KeGetCurrentIrql");
  EXPECT_EQ(Driver->State->Kernel->currentIRQL(), 0u);
}
TEST_P(KernelModulePublication,
       SubsequentEagerPublicationKeepsExistingCodeAndRing) {
  create({binding()});
  stage();
  auto E = call("SupportSet", {{77, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  const auto Before = Driver->Run->inspect()->Instructions;
  ASSERT_EQ(llvm::toString(Driver->Run->declareModule(
                binding("second", Base + 0x100000, "second.sys"))),
            "");
  EXPECT_FALSE(Driver->Run->modules().find("second")->mapped());
  E = call("SupportRing", {{root(), {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 3u);
  EXPECT_EQ(Driver->Run->inspect()->Instructions,
            Before + E->Invocation->Instructions);
  E = call("SupportProbe", {{1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  EXPECT_EQ(E->Invocation->Value, 78u);
  auto Next = Driver->Run->modules().find("second");
  ASSERT_TRUE(Next && Next->mapped());
  GuestInvocationRequest R;
  R.Entry = entry("SupportProbe", "second");
  R.ABI = IntegerCallingConvention::Win64;
  R.Arguments = {{1, {}}};
  ASSERT_EQ(llvm::toString(Driver->Run->invoke(std::move(R))), "");
  E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  EXPECT_EQ(E->Invocation->Value, 43u);
  auto Query = call("SupportModules", {{0, 0}, {4096, {}}}, {{4096, {}, true}});
  ASSERT_TRUE(bool(Query));
  ASSERT_TRUE(Query->Invocation);
  EXPECT_EQ(Query->Invocation->Value, 8 + 5 * 296);
  EXPECT_EQ(llvm::support::endian::read32le(
                Query->Invocation->Buffers[0].Bytes.data()),
            5u);
}
TEST_P(KernelModulePublication, ModuleQueryReadsEachLiveRecordFlags) {
  create({binding()});
  stage();
  auto E = call("SupportRecord", {{root(), {}}, {Base, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  const auto Record = E->Invocation->Value;
  ASSERT_NE(Record, 0u);
  E = call("SupportFlags", {{Record, {}}, {0x1234, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  E = call("SupportModules", {{0, 0}, {4096, {}}}, {{4096, {}, true}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  const auto &Bytes = E->Invocation->Buffers[0].Bytes;
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 8 + 28), 0u);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 8 + 3 * 296 + 28),
            0x1234u);
}
TEST_P(KernelModulePublication,
       MissingEagerSourceCanBeCorrectedWithoutEntryReplay) {
  ASSERT_EQ(
      llvm::toString(Sources->add({"repair", fixture("absent-module-input")})),
      "");
  auto B = binding();
  B.ModuleID = "repair";
  create({B}, true);
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->State, ExecutionRunState::AwaitingResource) << E->Diagnostic;
  ASSERT_TRUE(E->Request);
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
  const auto Token = E->Request->Token;
  EXPECT_NE(llvm::toString(Driver->Run->supply(Token, {B})), "");
  EXPECT_EQ(Driver->Run->state(), ExecutionRunState::AwaitingResource);
  ASSERT_EQ(llvm::toString(Sources->reviseUnprepared(
                "repair", fixture("kernel_module_support"), {})),
            "");
  ASSERT_EQ(llvm::toString(Driver->Run->supply(Token, {B})), "");
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  EXPECT_TRUE(Driver->Run->modules().find("helper")->mapped());
  EXPECT_NE(llvm::toString(Driver->Run->supply(Token, {B})), "");
  stage();
  E = call("SupportProbe", {{1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation);
  EXPECT_EQ(E->Invocation->Value, 43u);
}
TEST_P(KernelModulePublication,
       InvocationSpaceRemainsReservedBeforeItIsMapped) {
  create({binding("helper", profile::InvocationStackBase)});
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(E->Diagnostic.find("reserved"), std::string::npos);
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
}
TEST_P(KernelModulePublication, PureImportLookupCannotInventOrAliasAProvider) {
  create();
  auto &Registry = Driver->State->Exports;
  const auto Before = Registry.availableThunkCount();
  auto Kernel =
      Registry.existingImport({0, "NTKRNLMP.EXE", "KeGetCurrentIrql"});
  ASSERT_TRUE(bool(Kernel));
  EXPECT_EQ(*Kernel, *Registry.resolve("KeGetCurrentIrql"));
  for (const auto &I : {DriverImport{0, "private.sys", "KeGetCurrentIrql"},
                        DriverImport{0, "ntoskrnl.exe", "PrivateAbsentAPI"}}) {
    auto Missing = Registry.existingImport(I);
    ASSERT_FALSE(bool(Missing));
    EXPECT_NE(llvm::toString(Missing.takeError()).find("no existing"),
              std::string::npos);
  }
  EXPECT_EQ(Registry.availableThunkCount(), Before);
}
TEST_P(KernelModulePublication, ExplicitNoEntryModuleStillRunsItsExportedCode) {
  ASSERT_EQ(llvm::toString(Sources->add(
                {"noentry", fixture("kernel_module_support_noentry")})),
            "");
  auto B = binding();
  B.ModuleID = "noentry";
  create({B});
  stage();
  EXPECT_EQ(Driver->Run->modules().find("helper")->Entry, 0u);
  auto E = call("SupportProbe", {{1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 43u);
}
TEST_P(KernelModulePublication,
       MissingForwarderDependencyPublishesNoModuleOrRing) {
  ASSERT_EQ(llvm::toString(Sources->add(
                {"forwarder", fixture("kernel_module_support_forwarder")})),
            "");
  auto B = binding();
  B.ModuleID = "forwarder";
  create({B});
  const auto Record = root();
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::AwaitingResource);
  ASSERT_TRUE(E->Request);
  EXPECT_EQ(E->Request->Kind, ModuleRequestKind::MissingDependency);
  EXPECT_EQ(E->Request->Dependency, "missing");
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record);
  EXPECT_FALSE(*Driver->State->CPU->canAccess(Base, 1, 0));
}
TEST_P(KernelModulePublication,
       PhysicalConflictAbandonsRamAndLoaderMetadataTogether) {
  create({binding()});
  const auto Record = root();
  auto Before = Driver->Run->readMemory(profile::KernelArenaBase,
                                        profile::KernelArenaSize);
  ASSERT_TRUE(bool(Before));
  ASSERT_EQ(llvm::toString(Driver->State->CPU->map(Base, 4096, Read | Write)),
            "");
  const auto Revision = Driver->Run->modules().revision();
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
  EXPECT_EQ(Driver->Run->modules().revision(), Revision);
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record);
  EXPECT_EQ(*Driver->Run->readMemory(profile::KernelArenaBase,
                                     profile::KernelArenaSize),
            *Before);
}
TEST_P(KernelModulePublication, AliasConflictHasNoPartialPublication) {
  create({binding("a", Base, "HELPER.sys"),
          binding("b", Base + 0x100000, "helper.SYS")});
  const auto Record = root();
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(E->Diagnostic.find("aliases"), std::string::npos);
  EXPECT_FALSE(Driver->Run->modules().find("a")->mapped());
  EXPECT_FALSE(Driver->Run->modules().find("b")->mapped());
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record);
  EXPECT_FALSE(*Driver->State->CPU->canAccess(Base, 1, 0));
}
TEST_P(KernelModulePublication, CorruptLiveLinksAreRejectedRatherThanRepaired) {
  create({binding()});
  const auto Record = root();
  ASSERT_EQ(
      llvm::toString(Driver->State->CPU->writeInteger(Record, Record + 8, 8)),
      "");
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record + 8);
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
}
TEST_P(KernelModulePublication,
       AbandonedPreparationConsumesNoArenaOrModelIdentity) {
  create();
  auto Source = Sources->prepare("support");
  ASSERT_TRUE(bool(Source));
  auto B = binding();
  B.Space = Driver->State->CPU->addressSpace();
  auto Plan =
      planKernelGuestModule(B, **Source, Driver->State->Options.MemoryLimit);
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  ModuleInstance I{
      B,          m::LoadPolicy::Eager, *Source,      Plan->Image.Base,
      Plan->Size, Plan->Image.Entry,    Plan->Exports};
  auto Preview = Driver->Run->modules().inspect();
  Preview.push_back(I);
  auto Prepare = Driver->State->Kernel->prepareModulePublication(Preview);
  ASSERT_TRUE(bool(Prepare)) << llvm::toString(Prepare.takeError());
  ASSERT_FALSE((*Prepare)->patches().empty());
  const auto Address = (*Prepare)->patches().front().Address;
  EXPECT_NE(llvm::toString(
                Driver->State->Kernel->validateGuestAccess(Address, 8, false)),
            "");
  Prepare->reset();
  auto Again = Driver->State->Kernel->prepareModulePublication(Preview);
  ASSERT_TRUE(bool(Again));
  EXPECT_EQ((*Again)->patches().front().Address, Address);
  EXPECT_NE(llvm::toString(
                Driver->State->Kernel->validateGuestAccess(Address, 8, false)),
            "");
}
TEST_P(KernelModulePublication, LazyReadMapsOnlyAtTheRetainedInstruction) {
  ASSERT_EQ(
      llvm::toString(Sources->add(
          {"lazy", fixture("kernel_module_support"), {}, m::LoadPolicy::Lazy})),
      "");
  create({binding()}, true);
  stage();
  auto B = binding("lazy-helper", LateBase, "lazy.sys");
  B.ModuleID = "lazy";
  B.Size = Driver->Run->modules().find("helper")->Size;
  ASSERT_EQ(llvm::toString(Driver->Run->declareModule(B)), "");
  auto Data = Driver->Run->modules().lookupExport("helper", "SupportValue");
  ASSERT_TRUE(bool(Data));
  ASSERT_TRUE(*Data);
  const auto Address = LateBase + (**Data).Address - Base;
  EXPECT_FALSE(Sources->prepared("lazy"));
  const auto Used = Driver->Run->inspect()->Instructions;
  auto E = call("SupportRead", {{Address, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  EXPECT_TRUE(Driver->Run->modules().find("lazy-helper")->mapped());
  auto Snapshot = Driver->Run->inspect();
  ASSERT_TRUE(bool(Snapshot));
  EXPECT_FALSE(Snapshot->Continuation);
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 85u);
  EXPECT_GT(Snapshot->Instructions, Used);
  EXPECT_EQ(
      std::count_if(Driver->State->Result.Calls.begin(),
                    Driver->State->Result.Calls.end(),
                    [](const auto &C) { return C.Name == "KeGetCurrentIrql"; }),
      1);
}
TEST_P(KernelModulePublication,
       UnknownReadCanSupplyANewSourceInsideTheSameCall) {
  create({binding()}, true);
  stage();
  auto Data = Driver->Run->modules().lookupExport("helper", "SupportValue");
  ASSERT_TRUE(bool(Data));
  ASSERT_TRUE(*Data);
  const auto Address = LateBase + (**Data).Address - Base;
  auto E = call("SupportRead", {{Address, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->State, ExecutionRunState::AwaitingResource) << E->Diagnostic;
  ASSERT_TRUE(E->Request);
  EXPECT_EQ(E->Request->Address, Address);
  const auto Token = E->Request->Token;
  auto Snapshot = Driver->Run->inspect();
  ASSERT_TRUE(bool(Snapshot));
  ASSERT_TRUE(Snapshot->Invocation);
  const auto Used = Snapshot->Instructions;
  const auto Events = Snapshot->Events;
  ASSERT_EQ(
      llvm::toString(Sources->add({"late", fixture("kernel_module_support")})),
      "");
  auto B = binding("late-helper", LateBase, "late.sys");
  B.ModuleID = "late";
  auto Bad = B;
  Bad.GuestName = Driver->State->Options.ServiceName + ".sys";
  EXPECT_NE(llvm::toString(Driver->Run->supply(Token, {Bad})), "");
  EXPECT_EQ(Driver->Run->state(), ExecutionRunState::AwaitingResource);
  EXPECT_FALSE(Driver->Run->modules().find("late-helper"));
  ASSERT_EQ(llvm::toString(Driver->Run->supply(Token, {B})), "");
  EXPECT_EQ(Driver->Run->inspect()->Instructions, Used);
  EXPECT_EQ(Driver->Run->inspect()->Events, Events + 1);
  EXPECT_NE(llvm::toString(Driver->Run->supply(Token, {B})), "");
  E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 85u);
  EXPECT_EQ(
      std::count_if(Driver->State->Result.Calls.begin(),
                    Driver->State->Result.Calls.end(),
                    [](const auto &C) { return C.Name == "KeGetCurrentIrql"; }),
      1);
}
TEST_P(KernelModulePublication,
       MissingIndirectTargetKeepsTheCompletedCallFrame) {
  create({binding()}, true);
  stage();
  auto E = call("SupportIndirect",
                {{LateBase + entry("SupportProbe") - Base, {}}, {7, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->State, ExecutionRunState::AwaitingResource) << E->Diagnostic;
  ASSERT_TRUE(E->Request);
  ASSERT_TRUE(E->Resource);
  EXPECT_EQ(E->Request->Access, ModuleAccess::Fetch);
  EXPECT_EQ(E->Resource->Restart, ResourceRestartKind::FetchBoundary);
  const auto PC = E->Resource->ResumePC;
  ASSERT_EQ(
      llvm::toString(Sources->add({"late", fixture("kernel_module_support")})),
      "");
  auto B = binding("late-helper", LateBase, "late.sys");
  B.ModuleID = "late";
  ASSERT_EQ(llvm::toString(Driver->Run->supply(E->Request->Token, {B})), "");
  EXPECT_EQ(*Driver->State->CPU->reg(X64Register::PC), PC);
  E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 92u);
}
TEST_P(KernelModulePublication, LowPointersAndModelFieldsDoNotBecomeResources) {
  for (bool Object : {false, true}) {
    create({binding()}, true);
    stage();
    auto E = call("SupportRead", {{Object ? root() + 0x20 : 0x10000, {}}});
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(E->State, ExecutionRunState::Failed);
    EXPECT_FALSE(E->Request);
    EXPECT_FALSE(Driver->State->CPU->pendingResourcePause());
    Driver.reset();
  }
}
TEST_P(KernelModulePublication, DisabledRecoveryRetainsTerminalUnknownRead) {
  create({binding()}, false);
  stage();
  auto E = call("SupportRead", {{LateBase + 0x3000, {}}});
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_FALSE(E->Request);
}
TEST_P(KernelModulePublication,
       RemovingAnOwnedPageDoesNotTurnItIntoANewModule) {
  create({binding()}, true);
  stage();
  ASSERT_EQ(
      llvm::toString(Driver->State->CPU->addressSpace()->unmap(Base, 4096)),
      "");
  auto E = call("SupportRead", {{Base, {}}});
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_FALSE(E->Request);
  EXPECT_FALSE(Driver->State->CPU->pendingResourcePause());
}
TEST_P(KernelModulePublication, BatchImportsNamesDataAndOrdinalsInEitherOrder) {
  ASSERT_EQ(llvm::toString(Sources->add(
                {"dependent", fixture("kernel_module_dependent")})),
            "");
  ASSERT_EQ(llvm::toString(
                Sources->add({"provider", fixture("kernel_module_provider")})),
            "");
  for (const auto *ID : {"a-provider", "z-provider"}) {
    auto D = binding();
    D.ModuleID = "dependent";
    D.GuestName = "dependent.sys";
    auto P = binding(ID, LateBase, "PROVIDER.SYS");
    P.ModuleID = "provider";
    create({D, P});
    const auto Thunks = Driver->State->Exports.availableThunkCount();
    stage();
    EXPECT_EQ(Driver->State->Exports.availableThunkCount(), Thunks);
    auto E = call("DependentProbe", {{1, {}}});
    ASSERT_TRUE(bool(E));
    ASSERT_TRUE(E->Invocation) << E->Diagnostic;
    EXPECT_EQ(E->Invocation->Value, 90u);
    E = call("DependentSet", {{99, {}}});
    ASSERT_TRUE(bool(E));
    ASSERT_TRUE(E->Invocation) << E->Diagnostic;
    EXPECT_EQ(E->Invocation->Value, 99u);
    E = call("DependentProbe", {{1, {}}});
    ASSERT_TRUE(bool(E));
    ASSERT_TRUE(E->Invocation) << E->Diagnostic;
    EXPECT_EQ(E->Invocation->Value, 204u);
    EXPECT_TRUE(Driver->Run->modules().find(ID)->mapped());
    EXPECT_EQ(Driver->State->Exports.lookup(entry("SupportProbe", ID)),
              nullptr);
    Driver.reset();
  }
}
TEST_P(KernelModulePublication,
       ImportFirstUseExpandsOnlyTheRequiredLazyBinding) {
  ASSERT_EQ(llvm::toString(Sources->add(
                {"dependent", fixture("kernel_module_dependent")})),
            "");
  ASSERT_EQ(llvm::toString(Sources->add({"provider",
                                         fixture("kernel_module_provider"),
                                         {},
                                         m::LoadPolicy::Lazy})),
            "");
  ASSERT_EQ(llvm::toString(Sources->add(
                {"unused", fixture("absent-unused"), {}, m::LoadPolicy::Lazy})),
            "");
  auto D = binding();
  D.ModuleID = "dependent";
  D.GuestName = "dependent.sys";
  auto P = binding("provider", LateBase, "provider.sys");
  P.ModuleID = "provider";
  auto U = binding("unused", LateBase + 0x100000, "unused.sys");
  U.ModuleID = "unused";
  create({D, P, U});
  EXPECT_FALSE(Sources->prepared("provider"));
  stage();
  EXPECT_TRUE(Driver->Run->modules().find("provider")->mapped());
  EXPECT_FALSE(Driver->Run->modules().find("unused")->mapped());
  EXPECT_FALSE(Sources->prepared("unused"));
  auto E = call("DependentProbe", {{1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 90u);
}
TEST_P(KernelModulePublication,
       UnknownDependencySupplyRetriesTheWholeEagerBatch) {
  ASSERT_EQ(llvm::toString(Sources->add(
                {"dependent", fixture("kernel_module_dependent")})),
            "");
  auto D = binding();
  D.ModuleID = "dependent";
  D.GuestName = "dependent.sys";
  create({D}, true);
  const auto Record = root();
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->State, ExecutionRunState::AwaitingResource) << E->Diagnostic;
  ASSERT_TRUE(E->Request);
  EXPECT_EQ(E->Request->Kind, ModuleRequestKind::MissingDependency);
  EXPECT_EQ(E->Request->Dependency, "provider.sys");
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record);
  ASSERT_EQ(llvm::toString(
                Sources->add({"provider", fixture("kernel_module_provider")})),
            "");
  auto P = binding("provider", LateBase, "PROVIDER.SYS");
  P.ModuleID = "provider";
  auto Bad = P;
  Bad.ModuleID = "support";
  const auto Token = E->Request->Token;
  EXPECT_NE(llvm::toString(Driver->Run->supply(Token, {Bad})), "");
  EXPECT_EQ(Driver->Run->state(), ExecutionRunState::AwaitingResource);
  EXPECT_FALSE(Driver->Run->modules().find("provider"));
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
  ASSERT_EQ(llvm::toString(Driver->Run->supply(Token, {P})), "");
  EXPECT_TRUE(Driver->Run->modules().find("provider")->mapped());
  EXPECT_TRUE(Driver->Run->modules().find("helper")->mapped());
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  stage();
  E = call("DependentProbe", {{1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 90u);
}
TEST_P(KernelModulePublication,
       ForwardersResolveNamesOrdinalsDataAndExistingApis) {
  ASSERT_EQ(
      llvm::toString(Sources->add({"relay", fixture("kernel_module_relay")})),
      "");
  ASSERT_EQ(llvm::toString(
                Sources->add({"provider", fixture("kernel_module_provider")})),
            "");
  auto R = binding();
  R.ModuleID = "relay";
  R.GuestName = "relay.sys";
  auto P = binding("provider", LateBase, "provider.sys");
  P.ModuleID = "provider";
  create({R, P});
  stage();
  auto E = call("ForwardProbe", {{1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 43u);
  E = call("ForwardOrdinal", {{71, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 71u);
  auto Data = Driver->Run->modules().lookupExport("helper", "ForwardData");
  ASSERT_TRUE(bool(Data));
  ASSERT_TRUE(*Data);
  EXPECT_TRUE((**Data).Forwarded);
  EXPECT_EQ((**Data).Kind, m::SymbolKind::Data);
  EXPECT_EQ(*Driver->State->CPU->readInteger((**Data).Address, 8), 71u);
  auto K = Driver->Run->modules().lookupExport("helper", "ForwardKernel");
  ASSERT_TRUE(bool(K));
  ASSERT_TRUE(*K);
  EXPECT_EQ((**K).Address, *Driver->State->Exports.resolve("KeGetCurrentIrql"));
  EXPECT_TRUE((**K).Forwarded);
}
TEST_P(KernelModulePublication,
       ForwarderCyclesPublishNeitherRamNorLoaderRecords) {
  ASSERT_EQ(
      llvm::toString(Sources->add({"cycle", fixture("kernel_module_cycle")})),
      "");
  auto B = binding();
  B.ModuleID = "cycle";
  B.GuestName = "cycle.sys";
  create({B});
  const auto Record = root();
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(E->Diagnostic.find("forwarder cycle"), std::string::npos);
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
  EXPECT_EQ(*Driver->State->CPU->readInteger(Record, 8), Record);
  EXPECT_FALSE(*Driver->State->CPU->canAccess(Base, 1, 0));
}
TEST_P(KernelModulePublication,
       DependencySupplyRetainsTheOriginalCpuFaultAndCall) {
  ASSERT_EQ(llvm::toString(Sources->add(
                {"dependent", fixture("kernel_module_dependent"),
                 m::DebugInput{m::DebugKind::PDB,
                               fixture("kernel_module_dependent", ".pdb")},
                 m::LoadPolicy::Lazy})),
            "");
  auto Source = Sources->prepare("dependent");
  ASSERT_TRUE(bool(Source));
  auto Symbol = (*Source)->lookupSymbol("DependentProbe");
  ASSERT_TRUE(bool(Symbol));
  ASSERT_TRUE(*Symbol);
  auto D = binding("dependent", LateBase, "dependent.sys");
  D.ModuleID = "dependent";
  auto Plan = planKernelGuestModule(D, **Source, 64 * 1024 * 1024);
  ASSERT_TRUE(bool(Plan));
  D.Size = Plan->Size;
  const auto Target = LateBase + (**Symbol).Address - (*Source)->image().Base;
  create({binding(), D}, true);
  stage();
  auto E = call("SupportIndirect", {{Target, {}}, {1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->State, ExecutionRunState::AwaitingResource) << E->Diagnostic;
  ASSERT_TRUE(E->Request);
  ASSERT_TRUE(E->Resource);
  EXPECT_EQ(E->Request->InstanceID, "dependent");
  EXPECT_NE(E->Diagnostic.find("provider.sys"), std::string::npos);
  EXPECT_FALSE(Driver->Run->modules().find("dependent")->mapped());
  const auto Used = Driver->Run->inspect()->Instructions;
  const auto Token = E->Request->Token;
  ASSERT_EQ(llvm::toString(
                Sources->add({"provider", fixture("kernel_module_provider")})),
            "");
  auto P = binding("provider", LateBase + 0x100000, "provider.sys");
  P.ModuleID = "provider";
  EXPECT_NE(llvm::toString(Driver->Run->supply(Token, {P})), "");
  EXPECT_EQ(Driver->Run->state(), ExecutionRunState::AwaitingResource);
  ASSERT_EQ(llvm::toString(Driver->Run->supply(Token, {D, P})), "");
  EXPECT_EQ(Driver->Run->inspect()->Instructions, Used);
  E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 133u);
}
TEST_P(KernelModulePublication,
       CachedForwarderSuffixCannotHideAnExcessiveDepth) {
  ASSERT_EQ(
      llvm::toString(Sources->add({"deep", fixture("kernel_module_deep")})),
      "");
  ASSERT_EQ(llvm::toString(
                Sources->add({"provider", fixture("kernel_module_provider")})),
            "");
  auto D = binding();
  D.ModuleID = "deep";
  D.GuestName = "deep.sys";
  auto P = binding("provider", LateBase, "provider.sys");
  P.ModuleID = "provider";
  create({D, P});
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(E->Diagnostic.find("depth/work limit"), std::string::npos);
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
  EXPECT_FALSE(Driver->Run->modules().find("provider")->mapped());
}
TEST_P(KernelModulePublication,
       ExtensionlessForwarderCannotChooseAnAmbiguousProvider) {
  ASSERT_EQ(
      llvm::toString(Sources->add({"relay", fixture("kernel_module_relay")})),
      "");
  ASSERT_EQ(llvm::toString(
                Sources->add({"provider", fixture("kernel_module_provider")})),
            "");
  auto R = binding();
  R.ModuleID = "relay";
  R.GuestName = "relay.sys";
  auto A = binding("sys", LateBase, "provider.sys");
  A.ModuleID = "provider";
  auto B = binding("dll", LateBase + 0x100000, "provider.dll");
  B.ModuleID = "provider";
  create({R, A, B});
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(E->Diagnostic.find("ambiguous extensionless"), std::string::npos);
  EXPECT_FALSE(Driver->Run->modules().find("helper")->mapped());
}
TEST_P(KernelModulePublication,
       MissingDeclaredDependencyCanBeCorrectedBeforeCpuEntry) {
  ASSERT_EQ(llvm::toString(Sources->add(
                {"dependent", fixture("kernel_module_dependent")})),
            "");
  ASSERT_EQ(
      llvm::toString(Sources->add(
          {"provider", fixture("absent-provider"), {}, m::LoadPolicy::Lazy})),
      "");
  auto D = binding();
  D.ModuleID = "dependent";
  D.GuestName = "dependent.sys";
  auto P = binding("provider", LateBase, "PROVIDER.SYS");
  P.ModuleID = "provider";
  create({D, P}, true);
  auto E = advance(*Driver->Run);
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->State, ExecutionRunState::AwaitingResource) << E->Diagnostic;
  ASSERT_TRUE(E->Request);
  EXPECT_EQ(E->Request->Kind, ModuleRequestKind::MissingDependency);
  EXPECT_EQ(E->Request->InstanceID, "provider");
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  ASSERT_EQ(llvm::toString(Sources->reviseUnprepared(
                "provider", fixture("kernel_module_provider"), {})),
            "");
  ASSERT_EQ(llvm::toString(Driver->Run->supply(E->Request->Token, {P})), "");
  EXPECT_TRUE(Driver->Run->modules().find("helper")->mapped());
  EXPECT_EQ(Driver->Run->inspect()->Instructions, 0u);
  stage();
  E = call("DependentProbe", {{1, {}}});
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(E->Invocation) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 90u);
}
INSTANTIATE_TEST_SUITE_P(
    Contracts, KernelModulePublication,
    testing::Values(ExecutionContract::Legacy, ExecutionContract::CheckedX64),
    [](const testing::TestParamInfo<ExecutionContract> &P) {
      return P.param == ExecutionContract::Legacy ? "Legacy" : "Checked";
    });
} // namespace
