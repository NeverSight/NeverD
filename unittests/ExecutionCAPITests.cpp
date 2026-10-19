//===- ExecutionCAPITests.cpp - Actual SDK persistent workloads
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Actual SDK persistent workloads.
///

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

extern "C" int neverd_test_execution_c_header(void);
namespace {
namespace j = llvm::json;
std::string json(j::Value V) { return llvm::formatv("{0}", V).str(); }
std::string take(const char *Text) {
  std::string S = Text ? Text : "";
  neverd_free_string(Text);
  return S;
}
class ExecutionCAPI : public testing::Test {
protected:
  neverd_module_set_t Sources = 0;
  neverd_execution_t Run = 0;
  const char *Error = nullptr;
  std::filesystem::path Root, File;
  void SetUp() override {
    Sources = neverd_module_set_create_v1(nullptr, &Error);
    ASSERT_NE(Sources, 0u) << error();
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-execution-capi",
                                                      Directory));
    Root = std::u8string(Directory.begin(), Directory.end());
    File = Root / "data.elf";
    using ELF = llvm::object::ELF64LE;
    ELF::Ehdr H{};
    std::memcpy(H.e_ident, "\177ELF", 4);
    H.e_ident[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
    H.e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
    H.e_ident[llvm::ELF::EI_VERSION] = llvm::ELF::EV_CURRENT;
    H.e_type = llvm::ELF::ET_EXEC;
    H.e_machine = llvm::ELF::EM_X86_64;
    H.e_version = llvm::ELF::EV_CURRENT;
    H.e_entry = 0; // A data-only resource has no executable entry.
    H.e_ehsize = sizeof(H);
    H.e_phoff = sizeof(H);
    H.e_phentsize = sizeof(ELF::Phdr);
    H.e_phnum = 1;
    H.e_shentsize = sizeof(ELF::Shdr);
    ELF::Phdr P{};
    P.p_type = llvm::ELF::PT_LOAD;
    P.p_flags = llvm::ELF::PF_R | llvm::ELF::PF_W;
    P.p_offset = 4096;
    P.p_vaddr = 0x50000;
    P.p_filesz = 8;
    P.p_memsz = 8;
    P.p_align = 4096;
    std::vector<uint8_t> Bytes(4104, 0);
    std::memcpy(Bytes.data(), &H, sizeof(H));
    std::memcpy(Bytes.data() + sizeof(H), &P, sizeof(P));
    Bytes[4096] = 42;
    std::ofstream OS(File, std::ios::binary);
    OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  }
  void TearDown() override {
    neverd_free_string(Error);
    neverd_execution_destroy(Run, nullptr);
    neverd_module_set_destroy(Sources, nullptr);
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  std::string error() {
    auto S = take(Error);
    Error = nullptr;
    return S;
  }
  j::Object config(std::string Code = "B84900000090", uint64_t Stop = 0x10005) {
    std::string Bytes = Code;
    Bytes.resize(0x100 * 2, '0');
    // mov [rcx],rdx; mov rax,rdx; add rax,2; ret
    Bytes += "4889114889D04883C002C3";
    return j::Object{
        {"environment",
         j::Object{{"kind", "buffer"},
                   {"cpu", j::Object{{"backend", "unicorn"}}},
                   {"memory_limit", 1048576},
                   {"entry", 0x10000},
                   {"stop_pc", Stop},
                   {"stack_pointer", 0x80F00},
                   {"regions", j::Array{j::Object{{"address", 0x10000},
                                                  {"size", 4096},
                                                  {"permissions", "rx"},
                                                  {"bytes_hex", Bytes}},
                                        j::Object{{"address", 0x60000},
                                                  {"size", 4096},
                                                  {"permissions", "rw"}},
                                        j::Object{{"address", 0x80000},
                                                  {"size", 4096},
                                                  {"permissions", "rw"}},
                                        j::Object{{"address", 0xA0000},
                                                  {"size", 4096},
                                                  {"permissions", "rw"}}}},
                   {"invocation", j::Object{{"stack_base", 0xA0000},
                                            {"stack_size", 4096},
                                            {"return_pc", 0x10300}}}}},
        {"run", j::Object{{"pause_stages", j::Array{"stopped"}},
                          {"memory_transfer_limit", 4096}}}};
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
    auto V = j::parse(take(Report));
    EXPECT_TRUE(bool(V));
    if (!V) {
      llvm::consumeError(V.takeError());
      return nullptr;
    }
    return std::move(*V);
  }
  j::Value event() { return parsed(neverd_execution_run_json(Run, &Error)); }
  j::Value snapshot() {
    return parsed(neverd_execution_inspect_json(Run, &Error));
  }
  j::Value command(j::Object Input) {
    Input["id"] = "test-command";
    auto Text = json(std::move(Input));
    auto V = parsed(neverd_execution_command_json(Run, Text.c_str(), &Error));
    const auto *O = V.getAsObject();
    EXPECT_NE(O, nullptr);
    if (!O || !O->get("result"))
      return nullptr;
    EXPECT_EQ(O->getString("id"), "test-command");
    EXPECT_EQ(O->getBoolean("ok"), true);
    return *O->get("result");
  }
  void add(std::string ID = "data", std::filesystem::path Path = {}) {
    auto U8 = (Path.empty() ? File : Path).u8string();
    auto Text = json(j::Object{{"id", ID},
                               {"image", std::string(U8.begin(), U8.end())},
                               {"load", "lazy"}});
    ASSERT_EQ(neverd_module_set_add_json(Sources, Text.c_str(), &Error), 1)
        << error();
  }
  j::Object binding() {
    return j::Object{{"id", "data-instance"}, {"module_id", "data"},
                     {"guest_name", "data"},  {"address_space", "buffer"},
                     {"base", "0x50000"},     {"size", 4096}};
  }
  std::string supply(const j::Value &Event) {
    auto *Request = Event.getAsObject()->getObject("request");
    EXPECT_NE(Request, nullptr);
    return json(j::Object{{"token", *Request->get("token")},
                          {"bindings", j::Array{binding()}}});
  }
};
TEST_F(ExecutionCAPI, PureCHeaderLinksAndRunsTheActualSharedLibrary) {
  EXPECT_EQ(neverd_test_execution_c_header(), 1);
}
TEST_F(ExecutionCAPI, ExactV1PrefixIsCheckedBeforeReadingAnyOtherField) {
  EXPECT_EQ(neverd_execution_create_v1(nullptr, &Error), 0u);
  EXPECT_FALSE(error().empty());
  struct Prefix {
    size_t Size;
  } P{sizeof(P)};
  EXPECT_EQ(neverd_execution_create_v1(
                reinterpret_cast<neverd_execution_options_v1 *>(&P), &Error),
            0u);
  EXPECT_NE(error().find("exact v1"), std::string::npos);
  P.Size = sizeof(neverd_execution_options_v1) + 8;
  EXPECT_EQ(neverd_execution_create_v1(
                reinterpret_cast<neverd_execution_options_v1 *>(&P), &Error),
            0u);
  EXPECT_FALSE(error().empty());
}
TEST_F(ExecutionCAPI, ConfigurationMustSelectASupportedEnvironmentExplicitly) {
  for (
      const auto *JSON :
      {"{}", "[]", R"({"environment":{"kind":"ntoskrnl"}})",
       R"({"environment":{"kind":"buffer"},"environment":{"kind":"buffer"}})"}) {
    neverd_execution_options_v1 O{sizeof(O), Sources, JSON};
    EXPECT_EQ(neverd_execution_create_v1(&O, &Error), 0u);
    EXPECT_FALSE(error().empty());
  }
  auto Text = json(config());
  neverd_execution_options_v1 O{sizeof(O), 0, Text.c_str()};
  EXPECT_EQ(neverd_execution_create_v1(&O, &Error), 0u);
  EXPECT_FALSE(error().empty());
}
TEST_F(ExecutionCAPI, CreateOwnsItsInputsAndRunsOnlyWhenAsked) {
  create(config());
  ASSERT_NE(Run, 0u);
  auto Before = snapshot();
  ASSERT_TRUE(Before.getAsObject());
  EXPECT_EQ(Before.getAsObject()->getString("instructions"), "0x0");
  EXPECT_EQ(Before.getAsObject()->getObject("event")->getString("state"),
            "ready");
  auto E = event();
  EXPECT_EQ(E.getAsObject()->getString("reason"), "stage");
  EXPECT_EQ(E.getAsObject()->getString("stage"), "stopped");
  EXPECT_EQ(neverd_execution_resume(Run, &Error), 1) << error();
  E = event();
  EXPECT_EQ(E.getAsObject()->getString("state"), "completed");
  EXPECT_EQ(neverd_execution_run_json(Run, &Error), nullptr);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(neverd_execution_resume(Run, &Error), 0);
  EXPECT_FALSE(error().empty());
}
TEST_F(ExecutionCAPI, BufferCompletionAtTheExactInstructionLimitIsUncharged) {
  auto Configuration = config("90", 0x10001);
  auto *Control = Configuration.getObject("run");
  (*Control)["pause_stages"] = j::Array{};
  (*Control)["limits"] = j::Object{{"instructions", 1}};
  create(std::move(Configuration));
  ASSERT_NE(Run, 0u);
  const auto E = event();
  EXPECT_EQ(E.getAsObject()->getString("state"), "completed");
  const auto Before = snapshot();
  EXPECT_EQ(Before.getAsObject()->getString("instructions"), "0x1");
}
TEST_F(ExecutionCAPI, HandlesRejectWrongKindsStaleIdsAndReuse) {
  EXPECT_EQ(neverd_execution_state_json(Sources, &Error), nullptr);
  EXPECT_FALSE(error().empty());
  create(config());
  const auto Old = Run;
  EXPECT_EQ(neverd_module_set_inspect_json(Run, &Error), nullptr);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(neverd_execution_destroy(Run, &Error), 1);
  Run = 0;
  EXPECT_EQ(neverd_execution_destroy(Old, &Error), 0);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(neverd_execution_destroy(0, &Error), 1);
  EXPECT_EQ(Error, nullptr);
  create(config());
  EXPECT_NE(Run, Old);
}
TEST_F(ExecutionCAPI, SourceHandleCanBeReleasedBeforeTheRetainedWorkload) {
  create(config());
  ASSERT_NE(Run, 0u);
  EXPECT_EQ(neverd_module_set_destroy(Sources, &Error), 1);
  Sources = 0;
  auto E = event();
  ASSERT_TRUE(E.getAsObject());
  EXPECT_EQ(E.getAsObject()->getString("state"), "paused");
  EXPECT_EQ(neverd_execution_resume(Run, &Error), 1);
  E = event();
  EXPECT_EQ(E.getAsObject()->getString("state"), "completed");
}
TEST_F(ExecutionCAPI, InvocationProducesExactBytesThroughTheSameRun) {
  create(config());
  ASSERT_NE(Run, 0u);
  const auto *Request = R"({"entry":"0x10100","abi":"win64",
     "arguments":[{"buffer":0},{"value":40}],"buffers":[{"size":8,"input_hex":"FFFFFFFFFFFFFFFF"}]})";
  EXPECT_EQ(neverd_execution_invoke_json(Run, Request, &Error), 0);
  EXPECT_FALSE(error().empty());
  auto E = event();
  ASSERT_EQ(E.getAsObject()->getString("reason"), "stage");
  for (unsigned I = 0; I < 2; ++I) {
    ASSERT_EQ(neverd_execution_invoke_json(Run, Request, &Error), 1) << error();
    E = event();
    EXPECT_EQ(E.getAsObject()->getString("reason"), "invocation_complete");
    EXPECT_EQ(E.getAsObject()->getString("stage"), "stopped");
    const auto *R = E.getAsObject()->getObject("invocation");
    ASSERT_TRUE(R);
    EXPECT_EQ(R->getString("value"), "0x2A");
    auto *Buffer = (*R->getArray("buffers"))[0].getAsObject();
    EXPECT_EQ(Buffer->getString("bytes_hex"), "2800000000000000");
  }
}
TEST_F(ExecutionCAPI, MemoryTransfersCheckStateBoundsAndCallerStorage) {
  create(config());
  ASSERT_NE(Run, 0u);
  uint8_t Bytes[] = {1, 2, 3, 4};
  ASSERT_EQ(neverd_execution_write_memory(Run, 0x60000, Bytes, 4, &Error), 1);
  std::memset(Bytes, 0, 4);
  EXPECT_EQ(neverd_execution_read_memory(Run, 0x60000, Bytes, 4, &Error), 1);
  EXPECT_EQ(Bytes[3], 4);
  EXPECT_EQ(neverd_execution_read_memory(Run, 0x70000, Bytes, 4, &Error), 0);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(Bytes[3], 4);
  EXPECT_EQ(neverd_execution_read_memory(Run, 0x60000, nullptr, 1, &Error), 0);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(neverd_execution_write_memory(Run, 0x60000, nullptr, 1, &Error), 0);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(neverd_execution_read_memory(Run, 0x60000, Bytes, SIZE_MAX, &Error),
            0);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(
      neverd_execution_write_memory(Run, 0x60000, Bytes, SIZE_MAX, &Error), 0);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(neverd_execution_read_memory(Run, 0x60000, nullptr, 0, &Error), 1);
  auto E = event();
  EXPECT_EQ(neverd_execution_resume(Run, &Error), 1);
  E = event();
  EXPECT_EQ(neverd_execution_read_memory(Run, 0x60000, Bytes, 4, &Error), 1);
  EXPECT_EQ(neverd_execution_write_memory(Run, 0x60000, Bytes, 4, &Error), 0);
  EXPECT_FALSE(error().empty());
}
TEST_F(ExecutionCAPI, LateSourceSupplyRetainsTheFaultAndCumulativeBudget) {
  create(config("48BB0000050000000000488B0348A3000006000000000090", 0x10017));
  ASSERT_NE(Run, 0u);
  auto E = event();
  ASSERT_TRUE(E.getAsObject()->getObject("request"));
  EXPECT_EQ(E.getAsObject()->getString("state"), "awaiting_resource");
  auto BeforeSnapshot = snapshot();
  ASSERT_TRUE(BeforeSnapshot.getAsObject());
  const auto Before = json(*BeforeSnapshot.getAsObject()->get("instructions"));
  add();
  ASSERT_EQ(Error, nullptr);
  auto Supply = supply(E);
  auto Wrong = j::parse(Supply);
  (*Wrong->getAsObject()->getObject("token"))["version"] = "0xFFFFFFFFFFFFFFFF";
  EXPECT_EQ(neverd_execution_supply_json(Run, json(std::move(*Wrong)).c_str(),
                                         &Error),
            0);
  EXPECT_FALSE(error().empty());
  ASSERT_EQ(neverd_execution_supply_json(Run, Supply.c_str(), &Error), 1)
      << error();
  EXPECT_EQ(json(*snapshot().getAsObject()->get("instructions")), Before);
  E = event();
  ASSERT_EQ(E.getAsObject()->getString("reason"), "stage");
  uint64_t Value = 0;
  EXPECT_EQ(neverd_execution_read_memory(Run, 0x60000,
                                         reinterpret_cast<uint8_t *>(&Value),
                                         sizeof(Value), &Error),
            1);
  EXPECT_EQ(Value, 42u);
  EXPECT_EQ(neverd_execution_supply_json(Run, Supply.c_str(), &Error), 0);
  EXPECT_FALSE(error().empty());
}
TEST_F(ExecutionCAPI, DeclaredLazySourceMapsOnlyAtTheFirstGuestAccess) {
  add();
  auto Config =
      config("48BB0000050000000000488B0348A3000006000000000090", 0x10017);
  Config["bindings"] = j::Array{binding()};
  create(std::move(Config));
  ASSERT_NE(Run, 0u);
  auto Before = parsed(neverd_execution_instances_json(Run, &Error));
  EXPECT_EQ((*Before.getAsObject()->getArray("instances"))[0]
                .getAsObject()
                ->getBoolean("mapped"),
            false);
  auto E = event();
  EXPECT_EQ(E.getAsObject()->getString("reason"), "stage");
  auto After = parsed(neverd_execution_instances_json(Run, &Error));
  EXPECT_EQ((*After.getAsObject()->getArray("instances"))[0]
                .getAsObject()
                ->getBoolean("mapped"),
            true);
  auto Found = parsed(neverd_execution_lookup_symbol_json(Run, "data-instance",
                                                          "absent", &Error));
  EXPECT_EQ(Found.getAsObject()->getBoolean("found"), false);
}
TEST_F(ExecutionCAPI, GuestFailureIsAReportAndCannotBeResumed) {
  create(config("0F0B90", 0x10002));
  ASSERT_NE(Run, 0u);
  auto E = event();
  EXPECT_EQ(E.getAsObject()->getString("state"), "failed");
  EXPECT_EQ(E.getAsObject()->getString("reason"), "guest_failure");
  EXPECT_EQ(Error, nullptr);
  EXPECT_EQ(neverd_execution_request_pause(Run, &Error), 0);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(neverd_execution_resume(Run, &Error), 0);
  EXPECT_FALSE(error().empty());
  auto S = snapshot();
  EXPECT_EQ(S.getAsObject()->getObject("event")->getString("state"), "failed");
}
TEST_F(ExecutionCAPI, PauseBeforeRunDoesNotExecuteInstructions) {
  create(config());
  ASSERT_NE(Run, 0u);
  EXPECT_EQ(neverd_execution_request_pause(Run, &Error), 1);
  auto E = event();
  EXPECT_EQ(E.getAsObject()->getString("reason"), "user_pause");
  auto S = snapshot();
  EXPECT_EQ(S.getAsObject()->getString("instructions"), "0x0");
}
TEST_F(ExecutionCAPI, ConcurrentPauseIsAllowedAndOtherMutationsAreBusy) {
  auto Config = config("EBFE90", 0x10002);
  auto *Options = Config.getObject("run");
  (*Options)["instruction_quantum"] = 1000000;
  (*Options)["actions_per_run"] = 1000000;
  (*Options)["limits"] = j::Object{{"instructions", 1000000000},
                                   {"events", 1000000},
                                   {"timeout_microseconds", 5000000}};
  create(std::move(Config));
  ASSERT_NE(Run, 0u);
  std::string WorkerError, WorkerEvent;
  std::thread Worker([&] {
    const char *Failure = nullptr;
    WorkerEvent = take(neverd_execution_run_json(Run, &Failure));
    WorkerError = take(Failure);
  });
  bool Running = false;
  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < Deadline) {
    auto S = parsed(neverd_execution_state_json(Run, &Error));
    if (S.getAsObject() && S.getAsObject()->getString("state") == "running") {
      Running = true;
      break;
    }
    std::this_thread::yield();
  }
  if (Running) {
    EXPECT_EQ(neverd_execution_destroy(Run, &Error), 0);
    EXPECT_NE(error().find("busy"), std::string::npos);
    EXPECT_EQ(neverd_execution_inspect_json(Run, &Error), nullptr);
    EXPECT_NE(error().find("busy"), std::string::npos);
    EXPECT_EQ(neverd_module_set_prepare_eager(Sources, &Error), 0);
    EXPECT_NE(error().find("busy"), std::string::npos);
  }
  EXPECT_EQ(neverd_execution_request_pause(Run, &Error), 1) << error();
  Worker.join();
  EXPECT_TRUE(Running);
  EXPECT_TRUE(WorkerError.empty()) << WorkerError;
  auto E = j::parse(WorkerEvent);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->getAsObject()->getString("reason"), "user_pause");
}
TEST_F(ExecutionCAPI, DeclarationIsOneStrictObjectAndDoesNotPrepareTheSource) {
  add();
  create(config());
  ASSERT_NE(Run, 0u);
  auto Text = json(binding());
  EXPECT_EQ(neverd_execution_declare_binding_json(
                Run, (Text + "," + Text).c_str(), &Error),
            0);
  EXPECT_FALSE(error().empty());
  auto Unknown = binding();
  Unknown["address_space"] = "unknown";
  EXPECT_EQ(neverd_execution_declare_binding_json(
                Run, json(std::move(Unknown)).c_str(), &Error),
            0);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(neverd_execution_declare_binding_json(Run, Text.c_str(), &Error), 1)
      << error();
  auto Rows = parsed(neverd_execution_instances_json(Run, &Error));
  EXPECT_EQ(Rows.getAsObject()->getArray("instances")->size(), 1u);
  EXPECT_EQ(
      (*Rows.getAsObject()->getArray("instances"))[0].getAsObject()->getBoolean(
          "mapped"),
      false);
}
TEST_F(ExecutionCAPI, DataEntryDoesNotAcquireExecutePermissionDuringSupply) {
  const uint64_t Entry = 0x50000;
  std::fstream OS(File, std::ios::binary | std::ios::in | std::ios::out);
  OS.seekp(offsetof(llvm::object::ELF64LE::Ehdr, e_entry));
  OS.write(reinterpret_cast<const char *>(&Entry), sizeof(Entry));
  OS.close();
  add();
  auto C = config("48BB0000050000000000488B0348A3000006000000000090", 0x10017);
  C["bindings"] = j::Array{binding()};
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  auto E = event();
  ASSERT_TRUE(E.getAsObject());
  EXPECT_EQ(E.getAsObject()->getString("state"), "awaiting_resource");
  auto D = E.getAsObject()->getString("diagnostic");
  ASSERT_TRUE(D);
  EXPECT_NE(D->find("entry permission"), llvm::StringRef::npos);
  auto Rows = parsed(neverd_execution_instances_json(Run, &Error));
  EXPECT_EQ(
      (*Rows.getAsObject()->getArray("instances"))[0].getAsObject()->getBoolean(
          "mapped"),
      false);
  uint8_t Value = 77;
  EXPECT_EQ(neverd_execution_read_memory(Run, 0x50000, &Value, 1, &Error), 0);
  EXPECT_FALSE(error().empty());
  EXPECT_EQ(Value, 77);
}
TEST_F(ExecutionCAPI,
       BufferFactoryRejectsInvalidInputsWithoutPublishingAHandle) {
  for (unsigned I = 0; I < 7; ++I) {
    auto C = config();
    auto *E = C.getObject("environment");
    auto *Regions = E->getArray("regions");
    switch (I) {
    case 0:
      (*E)["unknown"] = true;
      break;
    case 1:
      (*Regions)[0].getAsObject()->operator[]("bytes_hex") = "GG";
      break;
    case 2:
      (*Regions)[1].getAsObject()->operator[]("address") = 0x10000;
      break;
    case 3:
      (*E)["memory_limit"] = 4096;
      break;
    case 4:
      (*E)["stop_pc"] = 0x70000;
      break;
    case 5:
      (*E->getObject("invocation"))["stack_base"] = 0x80000;
      break;
    case 6:
      (*Regions)[0].getAsObject()->erase("permissions");
      break;
    }
    auto Text = json(std::move(C));
    neverd_execution_options_v1 O{sizeof(O), Sources, Text.c_str()};
    const auto Created = neverd_execution_create_v1(&O, &Error);
    EXPECT_EQ(Created, 0u);
    EXPECT_FALSE(error().empty());
    if (Created)
      neverd_execution_destroy(Created, nullptr);
  }
}
TEST_F(ExecutionCAPI, EagerMissingInputCanBeCorrectedBeforeAnyInstructionRuns) {
  const auto Missing = (Root / "missing.elf").u8string();
  const auto Image = File.u8string();
  auto Input =
      json(j::Object{{"id", "data"},
                     {"image", std::string(Missing.begin(), Missing.end())},
                     {"load", "eager"}});
  ASSERT_EQ(neverd_module_set_add_json(Sources, Input.c_str(), &Error), 1);
  auto C = config("48BB0000050000000000488B0348A3000006000000000090", 0x10017);
  C["bindings"] = j::Array{binding()};
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  auto E = event();
  ASSERT_TRUE(E.getAsObject()->getObject("request"));
  EXPECT_EQ(E.getAsObject()->getObject("request")->getString("kind"),
            "declared_module");
  auto S = snapshot();
  EXPECT_EQ(S.getAsObject()->getString("instructions"), "0x0");
  Input = json(j::Object{{"id", "data"},
                         {"image", std::string(Image.begin(), Image.end())}});
  ASSERT_EQ(
      neverd_module_set_revise_unprepared_json(Sources, Input.c_str(), &Error),
      1)
      << error();
  ASSERT_EQ(neverd_execution_supply_json(Run, supply(E).c_str(), &Error), 1)
      << error();
  E = event();
  EXPECT_EQ(E.getAsObject()->getString("reason"), "stage");
}
TEST_F(ExecutionCAPI, ExplicitCheckedAarch64UsesTheSamePublicLifecycle) {
  auto C = config("400580D21F2003D5", 0x10004);
  auto *E = C.getObject("environment");
  E->erase("invocation");
  (*E)["cpu"] = j::Object{{"backend", "unicorn"},
                          {"architecture", "aarch64"},
                          {"contract", "checked-aarch64-v1"}};
  create(std::move(C));
  ASSERT_NE(Run, 0u);
  auto Event = event();
  EXPECT_EQ(Event.getAsObject()->getString("reason"), "stage");
  auto S = snapshot();
  EXPECT_EQ(S.getAsObject()->getString("instructions"), "0x1");
}
TEST_F(ExecutionCAPI, CommandFacadeUsesTheSameLifecycleAndMemoryPrimitives) {
  create(config());
  ASSERT_NE(Run, 0u);
  auto S = command(j::Object{{"op", "state"}});
  ASSERT_TRUE(S.getAsObject());
  EXPECT_EQ(S.getAsObject()->getString("state"), "ready");
  auto W = command(j::Object{
      {"op", "write"}, {"address", "0x60000"}, {"bytes_hex", "0080ff01"}});
  ASSERT_TRUE(W.getAsObject());
  EXPECT_EQ(W.getAsObject()->getBoolean("done"), true);
  auto R =
      command(j::Object{{"op", "read"}, {"address", "0x60000"}, {"size", 4}});
  ASSERT_TRUE(R.getAsObject());
  EXPECT_EQ(R.getAsObject()->getString("bytes_hex"), "0080ff01");
  auto P = command(j::Object{{"op", "pause"}});
  ASSERT_TRUE(P.getAsObject());
  auto E = command(j::Object{{"op", "run"}});
  ASSERT_TRUE(E.getAsObject());
  EXPECT_EQ(E.getAsObject()->getString("reason"), "user_pause");
  command(j::Object{{"op", "resume"}});
  E = command(j::Object{{"op", "run"}});
  ASSERT_TRUE(E.getAsObject());
  EXPECT_EQ(E.getAsObject()->getString("reason"), "stage");
  auto I = command(j::Object{{"op", "inspect"}});
  ASSERT_TRUE(I.getAsObject());
  EXPECT_EQ(I.getAsObject()->getObject("event")->getString("stage"), "stopped");
  auto Q = command(j::Object{{"op", "quit"}});
  ASSERT_TRUE(Q.getAsObject());
  EXPECT_EQ(Q.getAsObject()->getBoolean("quit"), true);
  // Transport quit neither destroys nor resumes the SDK workload.
  auto Alive = parsed(neverd_execution_state_json(Run, &Error));
  EXPECT_EQ(Alive.getAsObject()->getString("state"), "paused");
}
TEST_F(ExecutionCAPI,
       CommandFacadeAddsAndSuppliesRetainedSourcesAfterPublicRelease) {
  create(config("48BB0000050000000000488B0348A3000006000000000090", 0x10017));
  ASSERT_NE(Run, 0u);
  ASSERT_EQ(neverd_module_set_destroy(Sources, &Error), 1);
  Sources = 0;
  auto E = command(j::Object{{"op", "run"}});
  ASSERT_TRUE(E.getAsObject()->getObject("request"));
  auto U8 = File.u8string();
  command(j::Object{
      {"op", "module_add"},
      {"module", j::Object{{"id", "data"},
                           {"image", std::string(U8.begin(), U8.end())},
                           {"load", "lazy"}}}});
  auto Catalogue = command(j::Object{{"op", "modules"}});
  ASSERT_TRUE(Catalogue.getAsObject());
  ASSERT_EQ(Catalogue.getAsObject()->getArray("modules")->size(), 1u);
  EXPECT_EQ((*Catalogue.getAsObject()->getArray("modules"))[0]
                .getAsObject()
                ->getBoolean("prepared"),
            false);
  auto Supply = j::parse(supply(E));
  ASSERT_TRUE(bool(Supply));
  command(j::Object{{"op", "supply"}, {"supply", std::move(*Supply)}});
  E = command(j::Object{{"op", "run"}});
  ASSERT_TRUE(E.getAsObject());
  EXPECT_EQ(E.getAsObject()->getString("reason"), "stage");
  auto R =
      command(j::Object{{"op", "read"}, {"address", "0x60000"}, {"size", 8}});
  EXPECT_EQ(R.getAsObject()->getString("bytes_hex"), "2a00000000000000");
  auto Instances = command(j::Object{{"op", "instances"}});
  EXPECT_EQ(Instances.getAsObject()->getArray("instances")->size(), 1u);
  auto Found = command(j::Object{{"op", "lookup"},
                                 {"instance_id", "data-instance"},
                                 {"symbol", "absent"}});
  EXPECT_EQ(Found.getAsObject()->getBoolean("found"), false);
}
TEST_F(ExecutionCAPI, CommandFacadeRejectsAmbiguousInputBeforeGuestEffects) {
  create(config());
  ASSERT_NE(Run, 0u);
  for (
      const auto *JSON :
      {"{}", R"({"id":"a","op":"unknown"})",
       R"({"id":"a","op":"pause","extra":true})",
       R"({"id":"a","op":"pause","op":"run"})",
       R"({"id":"a","op":"read","address":1,"size":1.0})",
       R"({"id":"a","op":"read","address":"0xFFFFFFFFFFFFFFFF","size":2})",
       R"({"id":"a","op":"read","address":0,"size":4097})",
       R"({"id":"a","op":"write","address":0,"bytes_hex":"F"})",
       R"({"id":"a","op":"invoke","invocation":{"entry":1,"entry":2,"abi":"win64"}})",
       R"({"id":"\uD800","op":"pause"})"}) {
    EXPECT_EQ(neverd_execution_command_json(Run, JSON, &Error), nullptr);
    EXPECT_FALSE(error().empty());
  }
  auto S = snapshot();
  ASSERT_TRUE(S.getAsObject());
  EXPECT_EQ(S.getAsObject()->getString("instructions"), "0x0");
  EXPECT_EQ(S.getAsObject()->getObject("event")->getString("state"), "ready");
  EXPECT_EQ(neverd_execution_resume(Run, &Error), 0);
  auto Direct = error();
  EXPECT_EQ(
      neverd_execution_command_json(Run, R"({"id":"a","op":"resume"})", &Error),
      nullptr);
  EXPECT_EQ(error(), Direct);
}
TEST_F(ExecutionCAPI, CommandFacadeReusesTheFiniteInvocationPump) {
  create(config());
  ASSERT_NE(Run, 0u);
  command(j::Object{{"op", "run"}});
  auto Input = j::parse(
      R"({"entry":"0x10100","abi":"win64","arguments":[{"buffer":0},{"value":40}],"buffers":[{"size":8}]})");
  ASSERT_TRUE(bool(Input));
  command(j::Object{{"op", "invoke"}, {"invocation", std::move(*Input)}});
  auto E = command(j::Object{{"op", "run"}});
  ASSERT_TRUE(E.getAsObject()->getObject("invocation"));
  EXPECT_EQ(E.getAsObject()->getObject("invocation")->getString("value"),
            "0x2A");
  EXPECT_EQ(E.getAsObject()->getString("stage"), "stopped");
}
TEST_F(ExecutionCAPI, CommandDeclarationRevisionAndDebugShareSourcePolicy) {
  // MAP loading currently provides function names, so use an actual RX code
  // segment for this debug fixture. It is queried as metadata, never entered.
  std::fstream ImageFile(File, std::ios::binary | std::ios::in | std::ios::out);
  const uint32_t Flags = llvm::ELF::PF_R | llvm::ELF::PF_X;
  ImageFile.seekp(sizeof(llvm::object::ELF64LE::Ehdr) +
                  offsetof(llvm::object::ELF64LE::Phdr, p_flags));
  ImageFile.write(reinterpret_cast<const char *>(&Flags), sizeof(Flags));
  const uint8_t Function[] = {0xB8, 42, 0, 0, 0, 0xC3, 0x90, 0x90};
  ImageFile.seekp(4096);
  ImageFile.write(reinterpret_cast<const char *>(Function), sizeof(Function));
  ImageFile.close();
  create(config("48BB0000050000000000488B0348A3000006000000000090", 0x10017));
  ASSERT_NE(Run, 0u);
  auto Missing = (Root / "missing.elf").u8string();
  auto Image = File.u8string();
  command(j::Object{{"op", "module_add"},
                    {"module", j::Object{{"id", "data"},
                                         {"image", std::string(Missing.begin(),
                                                               Missing.end())},
                                         {"load", "lazy"}}}});
  command(j::Object{
      {"op", "module_revise"},
      {"module",
       j::Object{{"id", "data"},
                 {"image", std::string(Image.begin(), Image.end())}}}});
  const auto Map = Root / "data.map";
  std::ofstream OS(Map);
  OS << "VMA LMA Size Align Out In Symbol\n"
        "0000000000050000 0000000000050000 00000008 16 .text\n"
        "0000000000050000 0000000000050000 00000008 1 probe.o:(.text)\n"
        "0000000000050000 0000000000050000 00000008 1 mapped_probe\n";
  OS.close();
  auto MapU8 = Map.u8string();
  command(j::Object{
      {"op", "module_debug"},
      {"module_id", "data"},
      {"debug", j::Object{{"kind", "map"},
                          {"path", std::string(MapU8.begin(), MapU8.end())}}}});
  command(j::Object{{"op", "declare"}, {"binding", binding()}});
  auto Rows = command(j::Object{{"op", "instances"}});
  EXPECT_EQ(
      (*Rows.getAsObject()->getArray("instances"))[0].getAsObject()->getBoolean(
          "mapped"),
      false);
  auto Catalogue = command(j::Object{{"op", "modules"}});
  EXPECT_EQ((*Catalogue.getAsObject()->getArray("modules"))[0]
                .getAsObject()
                ->getBoolean("prepared"),
            true);
  command(j::Object{{"op", "run"}});
  auto Found = command(j::Object{{"op", "lookup"},
                                 {"instance_id", "data-instance"},
                                 {"symbol", "mapped_probe"}});
  ASSERT_TRUE(Found.getAsObject());
  EXPECT_EQ(Found.getAsObject()->getBoolean("found"), true);
  EXPECT_EQ(Found.getAsObject()->getString("address"), "0x50000");
}
} // namespace
