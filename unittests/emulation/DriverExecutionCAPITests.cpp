//===- DriverExecutionCAPITests.cpp - Real DLL persistent WDM boundary
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Real DLL persistent WDM boundary.
///

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace {
namespace j = llvm::json;
std::string json(j::Value V) { return llvm::formatv("{0}", V).str(); }
std::string take(const char *Text) {
  std::string S = Text ? Text : "";
  neverd_free_string(Text);
  return S;
}
class DriverExecutionCAPI : public testing::Test {
protected:
  neverd_module_set_t Sources = 0;
  neverd_execution_t Run = 0;
  const char *Error = nullptr;
  std::filesystem::path Root = NEVERD_DRIVER_FIXTURES;
  void SetUp() override {
    auto Origin = neverd::pathToUTF8(Root);
    neverd_module_set_options_v1 O{sizeof(O), "{}", Origin.c_str()};
    Sources = neverd_module_set_create_v1(&O, &Error);
    ASSERT_NE(Sources, 0u) << error();
    ASSERT_EQ(neverd_module_set_add_json(
                  Sources, R"({"id":"driver","image":"driver_invocation.sys",
                    "debug":{"kind":"pdb","path":"driver_invocation.pdb"}})",
                  &Error),
              1)
        << error();
  }
  void TearDown() override {
    neverd_free_string(Error);
    neverd_execution_destroy(Run, nullptr);
    neverd_module_set_destroy(Sources, nullptr);
  }
  std::string error() {
    auto S = take(Error);
    Error = nullptr;
    return S;
  }
  j::Object config() {
    return j::Object{
        {"environment",
         j::Object{{"kind", "windows-driver"},
                   {"image", "driver_invocation.sys"},
                   {"backend", "unicorn"},
                   {"contract", "checked-x64-v1"},
                   {"scenario", j::Object{{"load_address", "0x190000000"}}}}},
        {"run",
         j::Object{{"pause_stages", j::Array{"after-entry"}},
                   {"instruction_quantum", 2},
                   {"actions_per_run", 1},
                   {"limits", j::Object{{"instructions", 100000},
                                        {"events", 10000},
                                        {"timeout_microseconds", 5000123}}}}}};
  }
  void create(j::Object Config) {
    auto Text = json(std::move(Config));
    neverd_execution_options_v1 O{sizeof(O), Sources, Text.c_str()};
    Run = neverd_execution_create_v1(&O, &Error);
    ASSERT_NE(Run, 0u) << error();
    ASSERT_EQ(Error, nullptr);
  }
  j::Value parsed(const char *Report) {
    EXPECT_NE(Report, nullptr) << error();
    if (!Report)
      return nullptr;
    auto V = j::parse(take(Report));
    EXPECT_TRUE(bool(V));
    if (!V) {
      llvm::consumeError(V.takeError());
      return nullptr;
    }
    return std::move(*V);
  }
  j::Value snapshot() {
    return parsed(neverd_execution_inspect_json(Run, &Error));
  }
  j::Value advance() {
    for (unsigned N = 0; N < 100000; ++N) {
      auto E = parsed(neverd_execution_run_json(Run, &Error));
      const auto *O = E.getAsObject();
      if (!O || O->getString("reason") != "quantum")
        return E;
      EXPECT_EQ(neverd_execution_resume(Run, &Error), 1) << error();
    }
    ADD_FAILURE() << "public driver action bound exceeded";
    return nullptr;
  }
  uint64_t address(const char *Name) {
    auto V = parsed(
        neverd_module_set_lookup_symbol_json(Sources, "driver", Name, &Error));
    const auto *O = V.getAsObject();
    if (!O || !O->getBoolean("found").value_or(false)) {
      ADD_FAILURE() << "missing PDB function " << Name;
      return 0;
    }
    uint64_t Address = 0;
    const auto *Symbol = O->getObject("symbol");
    auto Text = Symbol ? Symbol->getString("address") : std::nullopt;
    if (!Text || Text->drop_front(2).getAsInteger(16, Address)) {
      ADD_FAILURE() << "invalid static symbol address";
      return 0;
    }
    return Address - 0x180000000 + 0x190000000;
  }
  j::Object request(const char *Name) {
    return j::Object{{"entry", "0x" + llvm::utohexstr(address(Name))},
                     {"abi", "win64"}};
  }
  j::Value invoke(j::Object Request) {
    const auto Text = json(std::move(Request));
    EXPECT_EQ(neverd_execution_invoke_json(Run, Text.c_str(), &Error), 1)
        << error();
    return advance();
  }
};
TEST_F(DriverExecutionCAPI, CreationAndInspectDoNotExecuteGuestCode) {
  create(config());
  ASSERT_NE(Run, 0u);
  auto V = snapshot();
  const auto *O = V.getAsObject();
  ASSERT_TRUE(O);
  EXPECT_EQ(O->getString("instructions"), "0x0");
  const auto *D = O->getObject("environment_report");
  ASSERT_TRUE(D);
  EXPECT_EQ(D->getString("image_base"), "0x190000000");
  EXPECT_TRUE(D->get("nt_status")->getAsNull());
  EXPECT_TRUE(D->getBoolean("observation_only").value_or(false));
  EXPECT_FALSE(D->get("stop_reason"));
  EXPECT_TRUE(D->getObject("capabilities")
                  ->getBoolean("module_publication")
                  .value_or(false));
}
TEST_F(DriverExecutionCAPI,
       PdbAddressArgumentsAndRepeatedBuffersUseTheRealModel) {
  create(config());
  ASSERT_NE(Run, 0u);
  auto E = advance();
  ASSERT_TRUE(E.getAsObject());
  ASSERT_EQ(E.getAsObject()->getString("reason"), "stage");
  auto R = request("ProbeSum");
  R["arguments"] = j::Array{j::Object{{"value", 1}}, j::Object{{"value", 2}},
                            j::Object{{"value", 3}}, j::Object{{"value", 4}},
                            j::Object{{"value", 5}}, j::Object{{"value", 6}},
                            j::Object{{"value", 7}}};
  E = invoke(std::move(R));
  ASSERT_TRUE(E.getAsObject());
  ASSERT_EQ(E.getAsObject()->getString("reason"), "invocation_complete");
  EXPECT_EQ(E.getAsObject()->getObject("invocation")->getString("value"),
            "0x45");
  for (unsigned I = 0; I < 2; ++I) {
    R = request("ProbeBuffers");
    R["arguments"] =
        j::Array{j::Object{{"buffer", 0}}, j::Object{{"value", 4}}};
    R["buffers"] = j::Array{j::Object{{"size", 4}, {"input_hex", "0080FF01"}}};
    E = invoke(std::move(R));
    ASSERT_EQ(E.getAsObject()->getString("reason"), "invocation_complete");
    const auto *Call = E.getAsObject()->getObject("invocation");
    EXPECT_EQ(Call->getString("value"), I ? "0x2B" : "0x2A");
    EXPECT_EQ(
        (*Call->getArray("buffers"))[0].getAsObject()->getString("bytes_hex"),
        I ? "2aaad52b" : "29a9d628");
  }
  ASSERT_EQ(neverd_execution_resume(Run, &Error), 1) << error();
  E = advance();
  EXPECT_EQ(E.getAsObject()->getString("state"), "completed");
  auto V = snapshot();
  const auto *D = V.getAsObject()->getObject("environment_report");
  ASSERT_TRUE(D);
  EXPECT_EQ(D->getString("stop_reason"), "returned");
  EXPECT_FALSE(D->getBoolean("observation_only").value_or(true));
}
TEST_F(DriverExecutionCAPI,
       ApiAndNestedCallbackReportsRemainAvailableAfterSourceRelease) {
  create(config());
  ASSERT_NE(Run, 0u);
  auto R = request("ProbeNotify");
  ASSERT_EQ(neverd_module_set_destroy(Sources, &Error), 1) << error();
  Sources = 0;
  ASSERT_EQ(advance().getAsObject()->getString("reason"), "stage");
  R["arguments"] = j::Array{j::Object{{"value", 5}}};
  auto E = invoke(std::move(R));
  ASSERT_EQ(E.getAsObject()->getString("reason"), "invocation_complete");
  EXPECT_EQ(E.getAsObject()->getObject("invocation")->getString("value"),
            "0x12");
  auto V = snapshot();
  auto *Calls =
      V.getAsObject()->getObject("environment_report")->getArray("calls");
  ASSERT_TRUE(Calls);
  ASSERT_FALSE(Calls->empty());
  EXPECT_TRUE(std::any_of(Calls->begin(), Calls->end(), [](const auto &C) {
    return C.getAsObject()->getString("name") == "KeGetCurrentIrql";
  }));
}
TEST_F(DriverExecutionCAPI, StrictEnvironmentAndKernelContractAdmission) {
  for (const auto *Field : {"unknown", "cpu"}) {
    auto C = config();
    (*C.getObject("environment"))[Field] = 1;
    const auto Text = json(std::move(C));
    neverd_execution_options_v1 O{sizeof(O), Sources, Text.c_str()};
    EXPECT_EQ(neverd_execution_create_v1(&O, &Error), 0u);
    EXPECT_NE(error().find("unknown field"), std::string::npos);
  }
  auto C = config();
  (*C.getObject("environment"))["contract"] = "checked-user-x64-v1";
  const auto Text = json(std::move(C));
  neverd_execution_options_v1 O{sizeof(O), Sources, Text.c_str()};
  EXPECT_EQ(neverd_execution_create_v1(&O, &Error), 0u);
  EXPECT_FALSE(error().empty());
}
TEST_F(DriverExecutionCAPI, FailedCallRemainsInspectableAndCannotResume) {
  create(config());
  ASSERT_NE(Run, 0u);
  auto Stage = advance();
  ASSERT_TRUE(Stage.getAsObject());
  ASSERT_EQ(Stage.getAsObject()->getString("reason"), "stage");
  auto E = invoke(request("ProbeIRQLLeak"));
  ASSERT_TRUE(E.getAsObject());
  EXPECT_EQ(E.getAsObject()->getString("state"), "failed");
  EXPECT_EQ(E.getAsObject()->getString("reason"), "environment_failure");
  auto V = snapshot();
  ASSERT_TRUE(V.getAsObject());
  EXPECT_EQ(V.getAsObject()->getObject("event")->getString("state"), "failed");
  auto *D = V.getAsObject()->getObject("environment_report");
  ASSERT_TRUE(D);
  EXPECT_TRUE(D->getBoolean("observation_only").value_or(false));
  EXPECT_FALSE(D->get("stop_reason"));
  EXPECT_EQ(neverd_execution_resume(Run, &Error), 0);
  EXPECT_FALSE(error().empty());
}
TEST_F(DriverExecutionCAPI, RelativeFilesRequireAnExplicitOrigin) {
  ASSERT_EQ(neverd_module_set_destroy(Sources, &Error), 1);
  Sources = neverd_module_set_create_v1(nullptr, &Error);
  ASSERT_NE(Sources, 0u);
  auto C = config();
  auto Text = json(std::move(C));
  neverd_execution_options_v1 O{sizeof(O), Sources, Text.c_str()};
  EXPECT_EQ(neverd_execution_create_v1(&O, &Error), 0u);
  EXPECT_NE(error().find("base_directory"), std::string::npos);
  C = config();
  (*C.getObject("environment"))["base_directory"] = neverd::pathToUTF8(Root);
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  EXPECT_EQ(advance().getAsObject()->getString("reason"), "stage");
}
TEST_F(DriverExecutionCAPI,
       ImageModuleUsesTheSamePdbObservationAsSymbolQueries) {
  auto C = config();
  auto *Environment = C.getObject("environment");
  Environment->erase("image");
  (*Environment)["image_module"] = "driver";
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  auto R = request("ProbeSum");
  R["arguments"] = j::Array{j::Object{{"value", 1}}, j::Object{{"value", 2}},
                            j::Object{{"value", 3}}, j::Object{{"value", 4}},
                            j::Object{{"value", 5}}, j::Object{{"value", 6}},
                            j::Object{{"value", 7}}};
  auto SourceReport = parsed(neverd_module_set_inspect_json(Sources, &Error));
  const auto *Source =
      (*SourceReport.getAsObject()->getArray("modules"))[0].getAsObject();
  ASSERT_TRUE(Source);
  EXPECT_TRUE(Source->getBoolean("prepared").value_or(false));
  auto V = snapshot();
  const auto *Inputs = V.getAsObject()
                           ->getObject("environment_report")
                           ->getArray("input_modules");
  ASSERT_TRUE(Inputs);
  ASSERT_EQ(Inputs->size(), 1u);
  EXPECT_EQ((*Inputs)[0].getAsObject()->getString("module_id"), "driver");
  EXPECT_EQ((*Inputs)[0].getAsObject()->getString("image_sha256"),
            Source->getString("image_sha256"));
  EXPECT_EQ((*Inputs)[0].getAsObject()->getString("debug_sha256"),
            Source->getString("debug_sha256"));
  ASSERT_EQ(neverd_module_set_destroy(Sources, &Error), 1) << error();
  Sources = 0;
  ASSERT_EQ(advance().getAsObject()->getString("reason"), "stage");
  auto E = invoke(std::move(R));
  ASSERT_TRUE(E.getAsObject());
  ASSERT_EQ(E.getAsObject()->getString("reason"), "invocation_complete");
  EXPECT_EQ(E.getAsObject()->getObject("invocation")->getString("value"),
            "0x45");
}
TEST_F(DriverExecutionCAPI, KernelModuleSelectsTheCanonicalSourceAndPdbFacts) {
  ASSERT_EQ(neverd_module_set_add_json(
                Sources, R"({"id":"entry","image":"success.sys"})", &Error),
            1)
      << error();
  ASSERT_EQ(
      neverd_module_set_add_json(
          Sources,
          R"({"id":"reference","image":"driver_kernel_debugger_reference.sys",
                     "debug":{"kind":"pdb","path":"driver_kernel_debugger_reference.pdb"}})",
          &Error),
      1)
      << error();
  auto C = config();
  auto *Environment = C.getObject("environment");
  Environment->erase("image");
  (*Environment)["image_module"] = "entry";
  (*Environment)["kernel_module"] = "reference";
  (*Environment)["scenario"] =
      j::Object{{"windows_version",
                 j::Object{{"major", 10}, {"minor", 0}, {"build", 19041}}}};
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  ASSERT_EQ(advance().getAsObject()->getString("reason"), "stage");
  auto SourcesReport = parsed(neverd_module_set_inspect_json(Sources, &Error));
  const auto *Rows = SourcesReport.getAsObject()->getArray("modules");
  ASSERT_TRUE(Rows);
  const auto I = std::find_if(Rows->begin(), Rows->end(), [](const auto &V) {
    return V.getAsObject()->getString("id") == "reference";
  });
  ASSERT_NE(I, Rows->end());
  auto V = snapshot();
  const auto *D = V.getAsObject()->getObject("environment_report");
  ASSERT_TRUE(D);
  const auto *Configuration = D->getObject("configuration");
  ASSERT_TRUE(Configuration);
  EXPECT_EQ(Configuration->getString("kernel_image_sha256"),
            I->getAsObject()->getString("image_sha256"));
  EXPECT_EQ(Configuration->getString("kernel_pdb_sha256"),
            I->getAsObject()->getString("debug_sha256"));
  ASSERT_EQ(D->getArray("input_modules")->size(), 2u);
  const auto *K = (*D->getArray("input_modules"))[1].getAsObject();
  EXPECT_EQ(K->getString("role"), "kernel-reference");
  EXPECT_EQ(K->getString("module_id"), "reference");
}
TEST_F(DriverExecutionCAPI,
       ConflictingResourceSelectionsFailBeforePreparation) {
  for (bool Kernel : {false, true}) {
    auto C = config();
    auto *Environment = C.getObject("environment");
    if (Kernel) {
      (*Environment)["kernel_module"] = "driver";
      (*Environment->getObject("scenario"))["kernel_image"] = "missing.sys";
    } else {
      (*Environment)["image_module"] = "driver";
    }
    const auto Text = json(std::move(C));
    neverd_execution_options_v1 O{sizeof(O), Sources, Text.c_str()};
    EXPECT_EQ(neverd_execution_create_v1(&O, &Error), 0u);
    EXPECT_FALSE(error().empty());
    auto V = parsed(neverd_module_set_inspect_json(Sources, &Error));
    EXPECT_FALSE((*V.getAsObject()->getArray("modules"))[0]
                     .getAsObject()
                     ->getBoolean("prepared")
                     .value_or(true));
  }
}
TEST_F(DriverExecutionCAPI, MismatchedKernelCompanionCannotPublishAWorkload) {
  ASSERT_EQ(
      neverd_module_set_add_json(
          Sources,
          R"({"id":"reference","image":"driver_kernel_debugger_reference.sys",
                     "debug":{"kind":"pdb","path":"driver_invocation.pdb"}})",
          &Error),
      1)
      << error();
  auto C = config();
  (*C.getObject("environment"))["kernel_module"] = "reference";
  auto Text = json(std::move(C));
  neverd_execution_options_v1 O{sizeof(O), Sources, Text.c_str()};
  EXPECT_EQ(neverd_execution_create_v1(&O, &Error), 0u);
  EXPECT_NE(error().find("does not match"), std::string::npos);
}
const j::Object *instanceRow(const j::Value &V, llvm::StringRef ID) {
  for (const auto &I : *V.getAsObject()->getArray("instances"))
    if (I.getAsObject()->getString("id") == ID)
      return I.getAsObject();
  ADD_FAILURE() << "missing instance " << ID.str();
  return nullptr;
}
TEST_F(DriverExecutionCAPI, PathBootstrapPublishesItsOwnedIdentityBeforeEntry) {
  create(config());
  ASSERT_NE(Run, 0u);
  auto V = parsed(neverd_execution_instances_json(Run, &Error));
  ASSERT_EQ(V.getAsObject()->getArray("instances")->size(), 1u);
  const auto *I = instanceRow(V, "$driver");
  ASSERT_TRUE(I);
  EXPECT_EQ(I->getString("module_id"), "@driver-image");
  EXPECT_EQ(I->getString("role"), "driver");
  EXPECT_TRUE(I->getBoolean("environment_owned").value_or(false));
  EXPECT_TRUE(I->getBoolean("mapped").value_or(false));
  EXPECT_EQ(I->getString("base"), "0x190000000");
  EXPECT_TRUE(I->getString("image_sha256"));
  auto S = snapshot();
  EXPECT_EQ(S.getAsObject()->getString("instructions"), "0x0");
  EXPECT_TRUE(S.getAsObject()
                  ->getObject("environment_report")
                  ->getArray("input_modules")
                  ->empty());
}
TEST_F(DriverExecutionCAPI,
       PublishedNativeModuleUsesItsOwnCSehThroughThePublicRun) {
  ASSERT_EQ(neverd_module_set_add_json(
                Sources,
                R"({"id":"seh","image":"kernel_module_seh.sys",
         "debug":{"kind":"pdb","path":"kernel_module_seh.pdb"}})",
                &Error),
            1)
      << error();
  auto C = config();
  C["bindings"] = j::Array{j::Object{{"id", "seh"},
                                     {"module_id", "seh"},
                                     {"guest_name", "seh.sys"},
                                     {"address_space", "kernel"},
                                     {"base", "0x1B0000000"}}};
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  ASSERT_EQ(advance().getAsObject()->getString("reason"), "stage");
  auto Symbol = parsed(neverd_execution_lookup_symbol_json(
      Run, "seh", "ModuleSEHCatch", &Error));
  auto Address = Symbol.getAsObject()->getString("address");
  ASSERT_TRUE(Address);
  for (unsigned N = 0; N < 2; ++N) {
    auto E =
        invoke(j::Object{{"entry", *Address},
                         {"abi", "win64"},
                         {"arguments", j::Array{j::Object{{"value", 7}}}}});
    ASSERT_EQ(E.getAsObject()->getString("reason"), "invocation_complete")
        << json(E);
    EXPECT_EQ(E.getAsObject()->getObject("invocation")->getString("value"),
              "0x4A");
  }
  auto S = snapshot();
  EXPECT_TRUE(S.getAsObject()
                  ->getObject("environment_report")
                  ->getObject("capabilities")
                  ->getBoolean("module_x64_c_seh")
                  .value_or(false));
}
TEST_F(DriverExecutionCAPI,
       PrimaryImportsWaitForPublicSupplyWithoutExecutingEntry) {
  ASSERT_EQ(neverd_module_set_add_json(
                Sources,
                R"({"id":"primary","image":"driver_module_imports_ordinal.sys",
         "debug":{"kind":"pdb","path":"driver_module_imports_ordinal.pdb"}})",
                &Error),
            1)
      << error();
  auto C = config();
  auto *Env = C.getObject("environment");
  Env->erase("image");
  (*Env)["image_module"] = "primary";
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  auto E = advance();
  ASSERT_EQ(E.getAsObject()->getString("state"), "awaiting_resource")
      << json(E);
  const auto *Request = E.getAsObject()->getObject("request");
  ASSERT_TRUE(Request);
  EXPECT_EQ(Request->getString("kind"), "missing_dependency");
  EXPECT_EQ(Request->getString("dependency"), "provider.sys");
  auto S = snapshot();
  EXPECT_EQ(S.getAsObject()->getString("instructions"), "0x0");
  auto *Caps = S.getAsObject()
                   ->getObject("environment_report")
                   ->getObject("capabilities");
  EXPECT_TRUE(Caps->getBoolean("primary_module_imports").value_or(false));
  EXPECT_FALSE(S.getAsObject()
                   ->getObject("environment_report")
                   ->getBoolean("primary_imports_linked")
                   .value_or(true));
  ASSERT_EQ(
      neverd_module_set_add_json(
          Sources,
          R"({"id":"provider","image":"kernel_module_provider.sys","load":"lazy"})",
          &Error),
      1)
      << error();
  const auto Supply = json(
      j::Object{{"token", *Request->get("token")},
                {"bindings", j::Array{j::Object{{"id", "provider"},
                                                {"module_id", "provider"},
                                                {"guest_name", "provider.sys"},
                                                {"address_space", "kernel"},
                                                {"base", "0x1C0000000"}}}}});
  ASSERT_EQ(neverd_execution_supply_json(Run, Supply.c_str(), &Error), 1)
      << error();
  S = snapshot();
  EXPECT_EQ(S.getAsObject()->getString("instructions"), "0x0");
  EXPECT_TRUE(S.getAsObject()
                  ->getObject("environment_report")
                  ->getBoolean("primary_imports_linked")
                  .value_or(false));
  ASSERT_EQ(advance().getAsObject()->getString("reason"), "stage");
  for (const auto &[Name, Expected] : {std::pair{"PrimaryEntryCount", 1u},
                                       std::pair{"PrimaryObserved", 199u}}) {
    auto Symbol = parsed(
        neverd_execution_lookup_symbol_json(Run, "$driver", Name, &Error));
    auto Address = Symbol.getAsObject()->getString("address");
    ASSERT_TRUE(Address);
    uint64_t A = 0;
    ASSERT_FALSE(Address->drop_front(2).getAsInteger(16, A));
    uint8_t Bytes[8]{};
    ASSERT_EQ(neverd_execution_read_memory(Run, A, Bytes, 8, &Error), 1)
        << error();
    EXPECT_EQ(llvm::support::endian::read64le(Bytes), Expected);
  }
}
TEST_F(DriverExecutionCAPI,
       RequiredBootstrapPinsALazyResourceWithoutChangingItsPolicy) {
  ASSERT_EQ(
      neverd_module_set_add_json(
          Sources,
          R"({"id":"lazy-driver","image":"driver_invocation.sys","load":"lazy",
          "debug":{"kind":"pdb","path":"driver_invocation.pdb"}})",
          &Error),
      1)
      << error();
  auto C = config();
  auto *Environment = C.getObject("environment");
  Environment->erase("image");
  (*Environment)["image_module"] = "lazy-driver";
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  auto V = parsed(neverd_execution_instances_json(Run, &Error));
  const auto *I = instanceRow(V, "$driver");
  ASSERT_TRUE(I);
  EXPECT_EQ(I->getString("module_id"), "lazy-driver");
  EXPECT_EQ(I->getString("load"), "lazy");
  EXPECT_TRUE(I->getBoolean("mapped").value_or(false));
  auto Symbol = parsed(
      neverd_execution_lookup_symbol_json(Run, "$driver", "ProbeSum", &Error));
  EXPECT_TRUE(Symbol.getAsObject()->getBoolean("found").value_or(false));
  EXPECT_EQ(advance().getAsObject()->getString("reason"), "stage");
}
TEST_F(DriverExecutionCAPI, GuestModuleMappingAndCallsUseThePublicRun) {
  ASSERT_EQ(neverd_module_set_add_json(
                Sources,
                R"({"id":"support","image":"kernel_module_support.sys",
         "debug":{"kind":"pdb","path":"kernel_module_support.pdb"}})",
                &Error),
            1)
      << error();
  auto C = config();
  C["bindings"] = j::Array{j::Object{{"id", "helper"},
                                     {"module_id", "support"},
                                     {"guest_name", "helper.sys"},
                                     {"scope", "kernel"},
                                     {"address_space", "kernel"},
                                     {"base", "0x1B0000000"}}};
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  auto V = parsed(neverd_execution_instances_json(Run, &Error));
  EXPECT_FALSE(instanceRow(V, "helper")->getBoolean("mapped").value_or(true));
  ASSERT_EQ(advance().getAsObject()->getString("reason"), "stage");
  V = parsed(neverd_execution_lookup_symbol_json(Run, "helper", "SupportProbe",
                                                 &Error));
  EXPECT_EQ(V.getAsObject()->getBoolean("found"), true);
  auto Address = V.getAsObject()->getString("address");
  ASSERT_TRUE(Address);
  auto E = invoke(j::Object{{"entry", *Address},
                            {"abi", "win64"},
                            {"arguments", j::Array{j::Object{{"value", 1}}}}});
  ASSERT_EQ(E.getAsObject()->getString("reason"), "invocation_complete")
      << json(E);
  EXPECT_EQ(E.getAsObject()->getObject("invocation")->getString("value"),
            "0x2B");
  V = parsed(neverd_execution_instances_json(Run, &Error));
  const auto *I = instanceRow(V, "helper");
  EXPECT_EQ(I->getString("base"), "0x1B0000000");
  EXPECT_TRUE(I->getBoolean("mapped").value_or(false));
  auto State = snapshot();
  const auto *Caps = State.getAsObject()
                         ->getObject("environment_report")
                         ->getObject("capabilities");
  EXPECT_TRUE(Caps->getBoolean("module_publication").value_or(false));
  EXPECT_TRUE(Caps->getBoolean("resource_pause").value_or(false));
  EXPECT_FALSE(Caps->getBoolean("module_entry_initialization").value_or(true));
}
TEST_F(DriverExecutionCAPI, MissingGuestModuleCanBeSuppliedInsideAPublicCall) {
  ASSERT_EQ(neverd_module_set_add_json(
                Sources,
                R"({"id":"support","image":"kernel_module_support.sys",
         "debug":{"kind":"pdb","path":"kernel_module_support.pdb"}})",
                &Error),
            1)
      << error();
  auto C = config();
  C["bindings"] = j::Array{j::Object{{"id", "helper"},
                                     {"module_id", "support"},
                                     {"guest_name", "helper.sys"},
                                     {"scope", "kernel"},
                                     {"address_space", "kernel"},
                                     {"base", "0x1B0000000"}}};
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  ASSERT_EQ(advance().getAsObject()->getString("reason"), "stage");
  auto Caller = parsed(neverd_execution_lookup_symbol_json(
      Run, "helper", "SupportIndirect", &Error));
  auto Target = parsed(neverd_execution_lookup_symbol_json(
      Run, "helper", "SupportProbe", &Error));
  auto Address = Target.getAsObject()->getString("address");
  ASSERT_TRUE(Address);
  uint64_t Value = 0;
  ASSERT_FALSE(Address->drop_front(2).getAsInteger(16, Value));
  auto E = invoke(j::Object{
      {"entry", *Caller.getAsObject()->getString("address")},
      {"abi", "win64"},
      {"arguments",
       j::Array{
           j::Object{{"value", "0x" + llvm::utohexstr(Value + 0x10000000)}},
           j::Object{{"value", 7}}}}});
  ASSERT_EQ(E.getAsObject()->getString("state"), "awaiting_resource")
      << json(E);
  auto Before = snapshot();
  ASSERT_EQ(neverd_module_set_add_json(
                Sources, R"({"id":"late","image":"kernel_module_support.sys"})",
                &Error),
            1)
      << error();
  auto *Request = E.getAsObject()->getObject("request");
  ASSERT_TRUE(Request);
  auto Supply = json(
      j::Object{{"token", *Request->get("token")},
                {"bindings", j::Array{j::Object{{"id", "late-helper"},
                                                {"module_id", "late"},
                                                {"guest_name", "late.sys"},
                                                {"scope", "kernel"},
                                                {"address_space", "kernel"},
                                                {"base", "0x1C0000000"}}}}});
  ASSERT_EQ(neverd_execution_supply_json(Run, Supply.c_str(), &Error), 1)
      << error();
  auto After = snapshot();
  EXPECT_EQ(After.getAsObject()->getString("instructions"),
            Before.getAsObject()->getString("instructions"));
  E = advance();
  ASSERT_EQ(E.getAsObject()->getString("reason"), "invocation_complete")
      << json(E);
  EXPECT_EQ(E.getAsObject()->getObject("invocation")->getString("value"),
            "0x5C");
}
TEST_F(DriverExecutionCAPI,
       MissingDependencyPublishesTheImporterAndProviderTogether) {
  ASSERT_EQ(neverd_module_set_add_json(
                Sources,
                R"({"id":"dependent","image":"kernel_module_dependent.sys",
         "debug":{"kind":"pdb","path":"kernel_module_dependent.pdb"}})",
                &Error),
            1)
      << error();
  auto C = config();
  C["bindings"] = j::Array{j::Object{{"id", "helper"},
                                     {"module_id", "dependent"},
                                     {"guest_name", "dependent.sys"},
                                     {"address_space", "kernel"},
                                     {"base", "0x1B0000000"}}};
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  auto E = advance();
  ASSERT_EQ(E.getAsObject()->getString("state"), "awaiting_resource")
      << json(E);
  auto *Request = E.getAsObject()->getObject("request");
  ASSERT_TRUE(Request);
  EXPECT_EQ(Request->getString("kind"), "missing_dependency");
  EXPECT_EQ(Request->getString("dependency"), "provider.sys");
  EXPECT_EQ(snapshot().getAsObject()->getString("instructions"), "0x0");
  ASSERT_EQ(neverd_module_set_add_json(
                Sources,
                R"({"id":"supplier","image":"kernel_module_provider.sys"})",
                &Error),
            1)
      << error();
  const auto Supply = json(
      j::Object{{"token", *Request->get("token")},
                {"bindings", j::Array{j::Object{{"id", "supplier"},
                                                {"module_id", "supplier"},
                                                {"guest_name", "PROVIDER.SYS"},
                                                {"address_space", "kernel"},
                                                {"base", "0x1C0000000"}}}}});
  ASSERT_EQ(neverd_execution_supply_json(Run, Supply.c_str(), &Error), 1)
      << error();
  auto V = parsed(neverd_execution_instances_json(Run, &Error));
  auto *Rows = V.getAsObject()->getArray("instances");
  ASSERT_TRUE(Rows);
  ASSERT_EQ(Rows->size(), 3u);
  for (const auto &Row : *Rows)
    EXPECT_TRUE(Row.getAsObject()->getBoolean("mapped").value_or(false));
  ASSERT_EQ(advance().getAsObject()->getString("reason"), "stage");
  auto Symbol = parsed(neverd_execution_lookup_symbol_json(
      Run, "helper", "DependentProbe", &Error));
  E = invoke(j::Object{{"entry", *Symbol.getAsObject()->getString("address")},
                       {"abi", "win64"},
                       {"arguments", j::Array{j::Object{{"value", 1}}}}});
  ASSERT_EQ(E.getAsObject()->getString("reason"), "invocation_complete")
      << json(E);
  EXPECT_EQ(E.getAsObject()->getObject("invocation")->getString("value"),
            "0x5A");
}
} // namespace
