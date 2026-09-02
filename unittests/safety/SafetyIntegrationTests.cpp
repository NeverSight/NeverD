//===- SafetyIntegrationTests.cpp - End-to-end audit / hunt --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Drives the public C API against checked-in PE, ELF, and Mach-O fixtures for
/// x86-64 and AArch64.  Every matrix entry is mandatory on every CI host.
///
//===----------------------------------------------------------------------===//

#include "../../lib/sdk/capi/SessionImpl.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sdk/NeverDCAPISafety.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef SAFETY_FIXTURE_ROOT
#define SAFETY_FIXTURE_ROOT ""
#endif

namespace {

struct FixtureSpec {
  std::string Path;
  std::string Format;
  std::string Arch;
};

class TemporaryFixture {
public:
  explicit TemporaryFixture(const std::string &Source) {
    llvm::SmallString<128> UniqueDirectory;
    if (std::error_code EC = llvm::sys::fs::createUniqueDirectory(
            "neverd-safety-identity", UniqueDirectory)) {
      Error = EC.message();
      return;
    }
    Directory = UniqueDirectory.c_str();
    Binary = Directory / std::filesystem::path(Source).filename();
    if (std::error_code EC = llvm::sys::fs::copy_file(Source, Binary.string()))
      Error = EC.message();
  }

  ~TemporaryFixture() {
    if (Directory.empty())
      return;
    std::error_code EC;
    std::filesystem::remove_all(Directory, EC);
  }

  bool valid() const { return Error.empty() && !Binary.empty(); }
  const std::filesystem::path &path() const { return Binary; }
  const std::string &error() const { return Error; }

private:
  std::filesystem::path Directory;
  std::filesystem::path Binary;
  std::string Error;
};

std::vector<FixtureSpec> fixtureSpecs() {
  const std::string Root = SAFETY_FIXTURE_ROOT;
  return {
      {Root + "/safety_cases_elf_x64", "ELF", "x86_64"},
      {Root + "/safety_cases_elf_arm64", "ELF", "aarch64"},
      {Root + "/safety_cases_macho_x64", "Mach-O", "x86_64"},
      {Root + "/safety_cases_macho_arm64", "Mach-O", "aarch64"},
      {Root + "/safety_cases_pe_x64.exe", "PE", "x86_64"},
      {Root + "/safety_cases_pe_arm64.exe", "PE", "aarch64"},
  };
}

// Load one fixture and run one track; returns the parsed report or false.
bool runTrack(const std::string &Path, bool Hunt, llvm::json::Value &Out,
              const neverd_safety_options *RequestedOptions = nullptr,
              std::string *Serialized = nullptr) {
  if (Path.empty())
    return false;

  neverd_session_t Sess = neverd_session_create();
  if (!neverd_session_load(Sess, Path.c_str())) {
    neverd_session_destroy(Sess);
    return false;
  }
  neverd_safety_options Options{};
  Options.struct_size = sizeof(Options);
  const neverd_safety_options *EffectiveOptions =
      RequestedOptions ? RequestedOptions : &Options;
  const char *Json = Hunt ? neverd_session_hunt_json(Sess, EffectiveOptions)
                          : neverd_session_audit_json(Sess, EffectiveOptions);
  bool Ok = false;
  if (Json) {
    if (Serialized)
      *Serialized = Json;
    if (llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Json)) {
      Out = std::move(*Parsed);
      Ok = true;
    } else {
      llvm::consumeError(Parsed.takeError());
    }
    neverd_free_string(Json);
  }
  neverd_session_destroy(Sess);
  return Ok;
}

std::string takeOwned(const char *Value) {
  if (!Value)
    return {};
  std::string Result(Value);
  neverd_free_string(Value);
  return Result;
}

bool namesMatch(llvm::StringRef Have, llvm::StringRef Want) {
  return Have == Want || Have == ("_" + Want).str();
}

bool hasFunctionNamed(neverd_session_t Sess, llvm::StringRef Name) {
  const int Count = neverd_func_count(Sess);
  for (int I = 0; I < Count; ++I) {
    const std::string Candidate = takeOwned(neverd_func_name(Sess, I));
    if (namesMatch(Candidate, Name))
      return true;
  }
  return false;
}

bool hasClass(const llvm::json::Object &Root, llvm::StringRef Class) {
  const llvm::json::Array *Findings = Root.getArray("findings");
  if (!Findings)
    return false;
  for (const llvm::json::Value &V : *Findings)
    if (const llvm::json::Object *O = V.getAsObject())
      if (O->getString("class") == Class)
        return true;
  return false;
}

const llvm::json::Object *findingNamed(const llvm::json::Object &Root,
                                       llvm::StringRef Function) {
  const llvm::json::Array *Findings = Root.getArray("findings");
  if (!Findings)
    return nullptr;
  for (const llvm::json::Value &V : *Findings) {
    const llvm::json::Object *O = V.getAsObject();
    if (!O)
      continue;
    if (auto Fn = O->getString("function"); Fn && namesMatch(*Fn, Function))
      return O;
  }
  return nullptr;
}

std::vector<const llvm::json::Object *>
findingsAtCall(const llvm::json::Object &Root, llvm::StringRef Function,
               llvm::StringRef Callee) {
  std::vector<const llvm::json::Object *> Matches;
  const llvm::json::Array *Findings = Root.getArray("findings");
  if (!Findings)
    return Matches;
  for (const llvm::json::Value &V : *Findings) {
    const llvm::json::Object *O = V.getAsObject();
    if (!O)
      continue;
    const std::optional<llvm::StringRef> Fn = O->getString("function");
    const std::optional<llvm::StringRef> Name = O->getString("name");
    if (Fn && Name && namesMatch(*Fn, Function) && namesMatch(*Name, Callee))
      Matches.push_back(O);
  }
  return Matches;
}

const llvm::json::Object *findingWithNameSource(const llvm::json::Object &Root,
                                                llvm::StringRef Source) {
  const llvm::json::Array *Findings = Root.getArray("findings");
  if (!Findings)
    return nullptr;
  for (const llvm::json::Value &V : *Findings)
    if (const llvm::json::Object *O = V.getAsObject())
      if (O->getString("name_source") == Source)
        return O;
  return nullptr;
}

void expectFixtureIdentity(const llvm::json::Object &Root,
                           const FixtureSpec &Spec) {
  EXPECT_EQ(Root.getString("format"), Spec.Format);
  EXPECT_EQ(Root.getString("arch"), Spec.Arch);
}

// Every function the fixture source defines.  A finding attributed to anything
// else means the containing function was not recovered from the companion.
bool isFixtureFunction(llvm::StringRef Name) {
  static constexpr llvm::StringLiteral Defined[] = {
      llvm::StringLiteral("tainted_stack_overflow"),
      llvm::StringLiteral("tainted_heap_overflow"),
      llvm::StringLiteral("bounded_copy_is_safe"),
      llvm::StringLiteral("strlen_guarded_copy"),
      llvm::StringLiteral("leaks_memory"),
      llvm::StringLiteral("leak_on_one_path"),
      llvm::StringLiteral("guarded_free"),
      llvm::StringLiteral("double_frees"),
      llvm::StringLiteral("uses_after_free"),
      llvm::StringLiteral("balanced_alloc"),
      llvm::StringLiteral("interproc_budget_entry"),
      llvm::StringLiteral("interproc_forward_outer"),
      llvm::StringLiteral("interproc_forward_inner"),
      llvm::StringLiteral("interproc_isolation_entry"),
      llvm::StringLiteral("interproc_isolation_leaf"),
      llvm::StringLiteral("interproc_dead_tainted_helper"),
      llvm::StringLiteral("main")};
  for (llvm::StringRef Candidate : Defined)
    if (namesMatch(Name, Candidate))
      return true;
  return false;
}

class SafetyIntegration : public ::testing::TestWithParam<FixtureSpec> {};

} // namespace

TEST_P(SafetyIntegration, HuntReportsTaintedOverflowWithImportIdentity) {
  llvm::json::Value V(nullptr);
  const FixtureSpec &Spec = GetParam();
  ASSERT_TRUE(runTrack(Spec.Path, /*Hunt=*/true, V)) << Spec.Path;
  const llvm::json::Object *Root = V.getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));
  expectFixtureIdentity(*Root, Spec);

  EXPECT_EQ(Root->getInteger("scanned").value_or(-1), 6);
  EXPECT_EQ(Root->getInteger("skipped").value_or(-1), 1);
  EXPECT_EQ(Root->getInteger("unsafe").value_or(-1), 2);
  EXPECT_EQ(Root->getInteger("safe").value_or(0) +
                Root->getInteger("unknown").value_or(0),
            4);

  const llvm::json::Array *Findings = Root->getArray("findings");
  ASSERT_NE(Findings, nullptr);
  bool FoundUnsafeImport = false;
  for (const llvm::json::Value &FV : *Findings) {
    const llvm::json::Object *O = FV.getAsObject();
    if (!O || O->getString("verdict") != "UNSAFE")
      continue;
    if (O->getString("name_source") != "import")
      continue;
    if (const llvm::json::Object *Ev = O->getObject("evidence"))
      EXPECT_TRUE(Ev->getObject("concrete_input") != nullptr);
    FoundUnsafeImport = true;
  }
  EXPECT_TRUE(FoundUnsafeImport);

  const llvm::json::Object *Bounded =
      findingNamed(*Root, "bounded_copy_is_safe");
  ASSERT_NE(Bounded, nullptr);
  EXPECT_NE(Bounded->getString("verdict"), "UNSAFE");
  const llvm::json::Object *BoundedEvidence = Bounded->getObject("evidence");
  ASSERT_NE(BoundedEvidence, nullptr);
  EXPECT_TRUE(BoundedEvidence->getString("skip_reason").has_value());

  if (const llvm::json::Object *O =
          findingNamed(*Root, "strlen_guarded_copy")) {
    EXPECT_NE(O->getString("verdict"), "UNSAFE");
    if (O->getString("verdict") == "UNKNOWN")
      EXPECT_EQ(O->getString("confidence"), "HIGH");
  }
}

TEST_P(SafetyIntegration, LiteralGetenvReplayIsExactAcrossTheFixtureMatrix) {
  llvm::json::Value V(nullptr);
  const FixtureSpec &Spec = GetParam();
  ASSERT_TRUE(runTrack(Spec.Path, /*Hunt=*/true, V)) << Spec.Path;
  const llvm::json::Object *Root = V.getAsObject();
  ASSERT_NE(Root, nullptr);

  const llvm::json::Object *Finding =
      findingNamed(*Root, "tainted_stack_overflow");
  ASSERT_NE(Finding, nullptr) << Spec.Path;
  ASSERT_EQ(Finding->getString("verdict"), "UNSAFE") << Spec.Path;
  const std::optional<int64_t> Capacity = Finding->getInteger("capacity");
  ASSERT_TRUE(Capacity.has_value()) << Spec.Path;
  ASSERT_GT(*Capacity, 0) << Spec.Path;

  const llvm::json::Object *Evidence = Finding->getObject("evidence");
  ASSERT_NE(Evidence, nullptr) << Spec.Path;
  ASSERT_TRUE(Evidence->getBoolean("replayable").value_or(false)) << Spec.Path;
  EXPECT_EQ(Evidence->getArray("symbolic_model"), nullptr) << Spec.Path;

  const llvm::json::Object *Replay = Evidence->getObject("replay");
  ASSERT_NE(Replay, nullptr) << Spec.Path;
  EXPECT_EQ(Replay->getString("adapter"), "process-input-v1") << Spec.Path;
  EXPECT_EQ(Replay->get("reason"), nullptr) << Spec.Path;
  const llvm::json::Array *QueryVariables = Replay->getArray("query_variables");
  ASSERT_NE(QueryVariables, nullptr) << Spec.Path;
  EXPECT_TRUE(QueryVariables->empty()) << Spec.Path;
  const llvm::json::Array *Inputs = Replay->getArray("inputs");
  ASSERT_NE(Inputs, nullptr) << Spec.Path;
  ASSERT_EQ(Inputs->size(), 1u) << Spec.Path;
  const llvm::json::Object *Input = Inputs->front().getAsObject();
  ASSERT_NE(Input, nullptr) << Spec.Path;
  EXPECT_EQ(Input->getString("kind"), "environment") << Spec.Path;
  EXPECT_EQ(Input->getString("name"), "PAYLOAD") << Spec.Path;
  EXPECT_TRUE(Input->getString("call_va").has_value()) << Spec.Path;
  EXPECT_GE(Input->getInteger("seq").value_or(-1), 0) << Spec.Path;
  EXPECT_EQ(Input->getInteger("invocation"), 0) << Spec.Path;
  EXPECT_EQ(Input->getInteger("offset"), 0) << Spec.Path;
  EXPECT_FALSE(Input->getBoolean("eof_after").value_or(true)) << Spec.Path;
  EXPECT_TRUE(Input->getBoolean("terminator_implicit").value_or(false))
      << Spec.Path;
  EXPECT_EQ(Input->getArray("bindings"), nullptr) << Spec.Path;

  std::string ExpectedBytes = "0x";
  for (int64_t I = 0; I < *Capacity; ++I)
    ExpectedBytes += "41";
  EXPECT_EQ(Input->getString("bytes_hex"), ExpectedBytes) << Spec.Path;
}

TEST_P(SafetyIntegration,
       FirstReadFromStdinReplayIsExactAcrossTheFixtureMatrix) {
  llvm::json::Value V(nullptr);
  const FixtureSpec &Spec = GetParam();
  ASSERT_TRUE(runTrack(Spec.Path, /*Hunt=*/true, V)) << Spec.Path;
  const llvm::json::Object *Root = V.getAsObject();
  ASSERT_NE(Root, nullptr);

  const llvm::json::Object *Finding =
      findingNamed(*Root, "tainted_heap_overflow");
  ASSERT_NE(Finding, nullptr) << Spec.Path;
  ASSERT_EQ(Finding->getString("verdict"), "UNSAFE") << Spec.Path;

  const llvm::json::Object *Evidence = Finding->getObject("evidence");
  ASSERT_NE(Evidence, nullptr) << Spec.Path;
  ASSERT_TRUE(Evidence->getBoolean("replayable").value_or(false)) << Spec.Path;
  const llvm::json::Array *Model = Evidence->getArray("symbolic_model");
  ASSERT_NE(Model, nullptr) << Spec.Path;
  ASSERT_EQ(Model->size(), 1u) << Spec.Path;
  const llvm::json::Object *Assignment = Model->front().getAsObject();
  ASSERT_NE(Assignment, nullptr) << Spec.Path;
  const std::optional<int64_t> AssignmentId = Assignment->getInteger("id");
  ASSERT_TRUE(AssignmentId.has_value()) << Spec.Path;
  EXPECT_EQ(Assignment->getString("name"), "call$1") << Spec.Path;
  EXPECT_EQ(Assignment->getInteger("width"), 64) << Spec.Path;
  EXPECT_EQ(Assignment->getString("value_hex"), "0x100") << Spec.Path;
  EXPECT_EQ(Assignment->getString("origin"), "fresh") << Spec.Path;

  const llvm::json::Object *Replay = Evidence->getObject("replay");
  ASSERT_NE(Replay, nullptr) << Spec.Path;
  EXPECT_EQ(Replay->getString("adapter"), "process-input-v1") << Spec.Path;
  EXPECT_EQ(Replay->get("reason"), nullptr) << Spec.Path;
  const llvm::json::Array *QueryVariables = Replay->getArray("query_variables");
  ASSERT_NE(QueryVariables, nullptr) << Spec.Path;
  ASSERT_EQ(QueryVariables->size(), Model->size()) << Spec.Path;
  ASSERT_TRUE(QueryVariables->front().getAsInteger().has_value()) << Spec.Path;
  EXPECT_EQ(*QueryVariables->front().getAsInteger(), *AssignmentId)
      << Spec.Path;
  const llvm::json::Array *Inputs = Replay->getArray("inputs");
  ASSERT_NE(Inputs, nullptr) << Spec.Path;
  ASSERT_EQ(Inputs->size(), 1u) << Spec.Path;
  const llvm::json::Object *Input = Inputs->front().getAsObject();
  ASSERT_NE(Input, nullptr) << Spec.Path;
  EXPECT_EQ(Input->getString("kind"), "stdin") << Spec.Path;
  EXPECT_EQ(Input->get("name"), nullptr) << Spec.Path;
  EXPECT_TRUE(Input->getString("call_va").has_value()) << Spec.Path;
  EXPECT_GE(Input->getInteger("seq").value_or(-1), 0) << Spec.Path;
  EXPECT_EQ(Input->getInteger("invocation"), 0) << Spec.Path;
  EXPECT_EQ(Input->getInteger("offset"), 0) << Spec.Path;
  EXPECT_TRUE(Input->getBoolean("eof_after").value_or(false)) << Spec.Path;
  EXPECT_FALSE(Input->getBoolean("terminator_implicit").value_or(true))
      << Spec.Path;
  std::string ExpectedBytes = "0x";
  for (unsigned I = 0; I < 256; ++I)
    ExpectedBytes += "41";
  EXPECT_EQ(Input->getString("bytes_hex"), ExpectedBytes) << Spec.Path;

  const llvm::json::Array *Bindings = Input->getArray("bindings");
  ASSERT_NE(Bindings, nullptr) << Spec.Path;
  ASSERT_EQ(Bindings->size(), 1u) << Spec.Path;
  EXPECT_EQ(Bindings->size(), QueryVariables->size()) << Spec.Path;
  const llvm::json::Object *Binding = Bindings->front().getAsObject();
  ASSERT_NE(Binding, nullptr) << Spec.Path;
  EXPECT_EQ(Binding->getInteger("assignment_id"), *AssignmentId) << Spec.Path;
  EXPECT_EQ(Binding->getString("role"), "extent") << Spec.Path;
  EXPECT_EQ(Binding->getInteger("offset"), 0) << Spec.Path;
}

TEST_P(SafetyIntegration, AuditReportsHeapDefects) {
  llvm::json::Value V(nullptr);
  const FixtureSpec &Spec = GetParam();
  ASSERT_TRUE(runTrack(Spec.Path, /*Hunt=*/false, V)) << Spec.Path;
  const llvm::json::Object *Root = V.getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));
  expectFixtureIdentity(*Root, Spec);

  EXPECT_EQ(Root->getInteger("scanned").value_or(-1), 7);
  // In-image ELF DWARF authenticates the void declarations on leaks_memory and
  // guarded_free, so neither can publish ownership into main.  PDB Phase A
  // publishes names only, while these particular reproducible Mach-O fixtures
  // intentionally omit LC_UUID and therefore cannot authenticate their
  // adjacent dSYM's type graph.  On those fixtures the live ABI return carrier
  // is indistinguishable from an untyped wrapper that returns its allocation:
  // preserve a May summary and report the two ignored results in main as
  // UNKNOWN instead of folding that uncertainty to No.
  const bool HasReturnDeclarations = Spec.Format == "ELF";
  EXPECT_EQ(Root->getInteger("unsafe").value_or(-1),
            HasReturnDeclarations ? 4 : 3);
  EXPECT_EQ(Root->getInteger("unknown").value_or(-1),
            HasReturnDeclarations ? 1 : 4);
  const llvm::json::Array *Findings = Root->getArray("findings");
  ASSERT_NE(Findings, nullptr);
  ASSERT_EQ(Findings->size(), HasReturnDeclarations ? 5u : 7u);
  struct ExpectedImportFinding {
    const char *Function;
    const char *Callee;
    const char *Class;
    const char *Verdict;
  };
  const std::vector<ExpectedImportFinding> ExpectedImports = {
      {"tainted_heap_overflow", "memcpy", "uninitialized_read", "UNKNOWN"},
      {"leaks_memory", "malloc", "heap_leak",
       HasReturnDeclarations ? "UNSAFE" : "UNKNOWN"},
      {"leak_on_one_path", "malloc", "heap_leak", "UNSAFE"},
      {"double_frees", "free", "double_free", "UNSAFE"},
      {"uses_after_free", "free", "use_after_free", "UNSAFE"},
  };
  for (const ExpectedImportFinding &Expected : ExpectedImports) {
    const std::vector<const llvm::json::Object *> Matches =
        findingsAtCall(*Root, Expected.Function, Expected.Callee);
    ASSERT_EQ(Matches.size(), 1u)
        << Expected.Function << " -> " << Expected.Callee;
    EXPECT_EQ(Matches.front()->getString("name_source"), "import");
    EXPECT_EQ(Matches.front()->getString("class"), Expected.Class);
    EXPECT_EQ(Matches.front()->getString("verdict"), Expected.Verdict);
  }
  EXPECT_TRUE(hasClass(*Root, "heap_leak"));
  EXPECT_TRUE(hasClass(*Root, "double_free"));
  EXPECT_TRUE(hasClass(*Root, "use_after_free"));

  const llvm::json::Object *UncheckedRead =
      findingNamed(*Root, "tainted_heap_overflow");
  ASSERT_NE(UncheckedRead, nullptr);
  EXPECT_EQ(UncheckedRead->getString("class"), "uninitialized_read");
  EXPECT_EQ(UncheckedRead->getString("verdict"), "UNKNOWN");

  const llvm::json::Object *LeaksMemory = findingNamed(*Root, "leaks_memory");
  ASSERT_NE(LeaksMemory, nullptr) << Spec.Path;
  EXPECT_EQ(LeaksMemory->getString("class"), "heap_leak");
  EXPECT_EQ(LeaksMemory->getString("verdict"),
            HasReturnDeclarations ? "UNSAFE" : "UNKNOWN");
  EXPECT_TRUE(
      namesMatch(LeaksMemory->getString("name").value_or(""), "malloc"));
  EXPECT_EQ(LeaksMemory->getString("name_source"), "import");
  if (const llvm::json::Object *O = findingNamed(*Root, "leak_on_one_path"))
    EXPECT_EQ(O->getString("class"), "heap_leak");
  EXPECT_EQ(findingNamed(*Root, "guarded_free"), nullptr);
  EXPECT_EQ(findingNamed(*Root, "balanced_alloc"), nullptr);

  for (llvm::StringRef Callee :
       {llvm::StringRef("leaks_memory"), llvm::StringRef("guarded_free")}) {
    const std::vector<const llvm::json::Object *> CallerFindings =
        findingsAtCall(*Root, "main", Callee);
    if (HasReturnDeclarations) {
      EXPECT_TRUE(CallerFindings.empty()) << Callee.str();
      continue;
    }
    ASSERT_EQ(CallerFindings.size(), 1u) << Callee.str();
    const llvm::json::Object &CallerFinding = *CallerFindings.front();
    EXPECT_EQ(CallerFinding.getString("class"), "heap_leak");
    EXPECT_EQ(CallerFinding.getString("verdict"), "UNKNOWN");
    EXPECT_EQ(CallerFinding.getString("name_source"),
              Spec.Format == "PE" ? "pdb" : "export");
    EXPECT_TRUE(CallerFinding.getString("detail").value_or("").contains(
        "callee may return heap ownership"));
  }
}

TEST_P(SafetyIntegration, ExitVerdictTallyIsConsistent) {
  llvm::json::Value V(nullptr);
  const FixtureSpec &Spec = GetParam();
  ASSERT_TRUE(runTrack(Spec.Path, /*Hunt=*/true, V)) << Spec.Path;
  const llvm::json::Object *Root = V.getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));
  expectFixtureIdentity(*Root, Spec);
  const llvm::json::Array *Findings = Root->getArray("findings");
  ASSERT_NE(Findings, nullptr);
  const int64_t Total = Root->getInteger("unsafe").value_or(0) +
                        Root->getInteger("safe").value_or(0) +
                        Root->getInteger("unknown").value_or(0);
  EXPECT_EQ(Total, static_cast<int64_t>(Findings->size()));
}

TEST_P(SafetyIntegration, InterproceduralBudgetsPreserveExactRootContext) {
  const FixtureSpec &Spec = GetParam();
  const auto Hunt = [&](unsigned MaxCallDepth, unsigned MaxSummaryIterations,
                        llvm::json::Value &Out,
                        std::string *Serialized = nullptr) {
    neverd_safety_options Options{};
    Options.struct_size = sizeof(Options);
    Options.max_call_depth = MaxCallDepth;
    Options.max_summary_iterations = MaxSummaryIterations;
    return runTrack(Spec.Path, /*Hunt=*/true, Out, &Options, Serialized);
  };

  llvm::json::Value Default(nullptr);
  std::string FirstSerialization;
  ASSERT_TRUE(Hunt(0, 0, Default, &FirstSerialization)) << Spec.Path;
  const llvm::json::Object *DefaultRoot = Default.getAsObject();
  ASSERT_NE(DefaultRoot, nullptr);
  EXPECT_EQ(DefaultRoot->getInteger("control_reachable"), 6);
  EXPECT_EQ(DefaultRoot->getInteger("attacker_reachable"), 4);
  const llvm::json::Object *DefaultFinding =
      findingNamed(*DefaultRoot, "interproc_forward_inner");
  ASSERT_NE(DefaultFinding, nullptr);
  const llvm::json::Object *DefaultReach =
      DefaultFinding->getObject("reachability");
  ASSERT_NE(DefaultReach, nullptr);
  EXPECT_EQ(DefaultReach->getString("status"), "REACHABLE");
  EXPECT_EQ(DefaultReach->getString("attacker_control"), "TAINTED");
  EXPECT_FALSE(DefaultReach->getBoolean("budget_hit").value_or(true));
  const llvm::json::Object *Entry = DefaultReach->getObject("entry");
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->getString("kind"), "export");
  EXPECT_TRUE(Entry->getString("name") == "interproc_budget_entry" ||
              Entry->getString("name") == "_interproc_budget_entry");
  const llvm::json::Array *Chain = DefaultReach->getArray("call_chain");
  ASSERT_NE(Chain, nullptr);
  ASSERT_EQ(Chain->size(), 2u);
  for (const llvm::json::Value &Edge : *Chain) {
    const llvm::json::Object *Call = Edge.getAsObject();
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getString("kind"), "direct");
    EXPECT_TRUE(Call->getString("caller_va").has_value());
    EXPECT_TRUE(Call->getString("call_va").has_value());
    EXPECT_TRUE(Call->getString("callee_va").has_value());
  }

  const llvm::json::Object *IsolationFinding =
      findingNamed(*DefaultRoot, "interproc_isolation_leaf");
  ASSERT_NE(IsolationFinding, nullptr);
  const llvm::json::Object *IsolationReach =
      IsolationFinding->getObject("reachability");
  ASSERT_NE(IsolationReach, nullptr);
  EXPECT_EQ(IsolationReach->getString("status"), "REACHABLE");
  EXPECT_EQ(IsolationReach->getString("attacker_control"), "UNKNOWN");
  EXPECT_FALSE(IsolationReach->getBoolean("budget_hit").value_or(true));
  const llvm::json::Object *IsolationEntry = IsolationReach->getObject("entry");
  ASSERT_NE(IsolationEntry, nullptr);
  EXPECT_EQ(IsolationEntry->getString("kind"), "export");
  EXPECT_TRUE(namesMatch(IsolationEntry->getString("name").value_or(""),
                         "interproc_isolation_entry"));
  const llvm::json::Array *IsolationChain =
      IsolationReach->getArray("call_chain");
  ASSERT_NE(IsolationChain, nullptr);
  ASSERT_EQ(IsolationChain->size(), 1u);
  const llvm::json::Object *IsolationCall =
      IsolationChain->front().getAsObject();
  ASSERT_NE(IsolationCall, nullptr);
  EXPECT_EQ(IsolationCall->getString("kind"), "direct");

  llvm::json::Value Repeated(nullptr);
  std::string SecondSerialization;
  ASSERT_TRUE(Hunt(0, 0, Repeated, &SecondSerialization)) << Spec.Path;
  EXPECT_EQ(SecondSerialization, FirstSerialization);

  llvm::json::Value Depth(nullptr);
  ASSERT_TRUE(Hunt(1, 0, Depth)) << Spec.Path;
  const llvm::json::Object *DepthRoot = Depth.getAsObject();
  ASSERT_NE(DepthRoot, nullptr);
  const llvm::json::Object *DepthFinding =
      findingNamed(*DepthRoot, "interproc_forward_inner");
  ASSERT_NE(DepthFinding, nullptr);
  const llvm::json::Object *DepthReach =
      DepthFinding->getObject("reachability");
  ASSERT_NE(DepthReach, nullptr);
  EXPECT_EQ(DepthReach->getString("status"), "UNKNOWN");
  EXPECT_EQ(DepthReach->getString("attacker_control"), "UNKNOWN");
  EXPECT_TRUE(DepthReach->getBoolean("budget_hit").value_or(false));

  llvm::json::Value Summary(nullptr);
  ASSERT_TRUE(Hunt(0, 1, Summary)) << Spec.Path;
  const llvm::json::Object *SummaryRoot = Summary.getAsObject();
  ASSERT_NE(SummaryRoot, nullptr);
  const llvm::json::Object *SummaryFinding =
      findingNamed(*SummaryRoot, "interproc_forward_inner");
  ASSERT_NE(SummaryFinding, nullptr);
  const llvm::json::Object *SummaryReach =
      SummaryFinding->getObject("reachability");
  ASSERT_NE(SummaryReach, nullptr);
  EXPECT_EQ(SummaryReach->getString("status"), "REACHABLE");
  EXPECT_EQ(SummaryReach->getString("attacker_control"), "UNKNOWN");
  EXPECT_TRUE(SummaryReach->getBoolean("budget_hit").value_or(false));
}

TEST_P(SafetyIntegration, CompanionIdentityAndNoDebugPolicyAreEnforced) {
  const FixtureSpec &Spec = GetParam();

  neverd_session_t WithDebug = neverd_session_create();
  ASSERT_TRUE(neverd_session_load(WithDebug, Spec.Path.c_str())) << Spec.Path;
  EXPECT_EQ(takeOwned(neverd_session_debug_info_kind(WithDebug)),
            Spec.Format == "PE" ? "pdb" : "dwarf");
  EXPECT_FALSE(takeOwned(neverd_session_debug_info_path(WithDebug)).empty());
  EXPECT_TRUE(hasFunctionNamed(WithDebug, "leaks_memory"));
  neverd_session_destroy(WithDebug);

  neverd_session_t WithoutDebug = neverd_session_create();
  neverd_session_set_debug_info_enabled(WithoutDebug, 0);
  ASSERT_TRUE(neverd_session_load(WithoutDebug, Spec.Path.c_str()))
      << Spec.Path;
  EXPECT_EQ(takeOwned(neverd_session_debug_info_kind(WithoutDebug)), "none");
  EXPECT_TRUE(takeOwned(neverd_session_debug_info_path(WithoutDebug)).empty());
  neverd_session_destroy(WithoutDebug);
}

TEST_P(SafetyIntegration, EveryFindingCarriesStatedIdentity) {
  const FixtureSpec &Spec = GetParam();
  for (bool Hunt : {true, false}) {
    llvm::json::Value V(nullptr);
    ASSERT_TRUE(runTrack(Spec.Path, Hunt, V)) << Spec.Path;
    const llvm::json::Object *Root = V.getAsObject();
    ASSERT_NE(Root, nullptr);
    const llvm::json::Array *Findings = Root->getArray("findings");
    ASSERT_NE(Findings, nullptr);
    ASSERT_FALSE(Findings->empty()) << Spec.Path;

    unsigned MainLeaksMemory = 0;
    unsigned MainGuardedFree = 0;

    for (const llvm::json::Value &FV : *Findings) {
      const llvm::json::Object *O = FV.getAsObject();
      ASSERT_NE(O, nullptr);
      const std::string Where = Spec.Format + " " + Spec.Arch + " " +
                                O->getString("name").value_or("?").str() +
                                " @ " +
                                O->getString("call_va").value_or("?").str();

      // The enclosing function is named by the companion, not by discovery.
      const std::optional<llvm::StringRef> Fn = O->getString("function");
      ASSERT_TRUE(Fn.has_value()) << Where;
      EXPECT_TRUE(isFixtureFunction(*Fn)) << Where;

      const std::optional<llvm::StringRef> Name = O->getString("name");
      ASSERT_TRUE(Name.has_value()) << Where;
      const bool IsMainLeaksMemory = !Hunt && Spec.Format != "ELF" &&
                                     namesMatch(*Fn, "main") &&
                                     namesMatch(*Name, "leaks_memory");
      const bool IsMainGuardedFree = !Hunt && Spec.Format != "ELF" &&
                                     namesMatch(*Fn, "main") &&
                                     namesMatch(*Name, "guarded_free");
      const bool IsUntrustedReturnMay = IsMainLeaksMemory || IsMainGuardedFree;
      MainLeaksMemory += IsMainLeaksMemory;
      MainGuardedFree += IsMainGuardedFree;

      if (IsUntrustedReturnMay) {
        EXPECT_EQ(O->getString("class"), "heap_leak") << Where;
        EXPECT_EQ(O->getString("verdict"), "UNKNOWN") << Where;
        EXPECT_TRUE(O->getString("detail").value_or("").contains(
            "callee may return heap ownership"))
            << Where;
        EXPECT_EQ(O->getString("name_source"),
                  Spec.Format == "PE" ? "pdb" : "export")
            << Where;
      } else {
        // Every direct runtime sink in the fixtures must still resolve through
        // the IAT, PLT, or dyld bind table.  The only non-import findings are
        // the two precisely enumerated May summaries above.
        EXPECT_EQ(O->getString("name_source"), "import") << Where;
      }

      // DWARF states the line a call sits on; a PDB carries names and
      // addresses only, so the report omits the key rather than inventing one.
      const std::optional<llvm::StringRef> Loc = O->getString("source");
      if (Spec.Format == "PE") {
        EXPECT_FALSE(Loc.has_value()) << Where;
      } else {
        ASSERT_TRUE(Loc.has_value()) << Where;
        const bool IsInterproc = Fn->contains("interproc_");
        EXPECT_TRUE(Loc->contains(IsInterproc ? "interproc_cases.c:"
                                              : "safety_cases.c:"))
            << Where << " -> " << Loc->str();
      }
    }

    const unsigned ExpectedMainMay = !Hunt && Spec.Format != "ELF" ? 1u : 0u;
    EXPECT_EQ(MainLeaksMemory, ExpectedMainMay) << Spec.Path;
    EXPECT_EQ(MainGuardedFree, ExpectedMainMay) << Spec.Path;
  }
}

TEST_P(SafetyIntegration, MapOnlyIdentityReplacesTheDefaultCompanion) {
  const FixtureSpec &Spec = GetParam();
  const std::string MapPath = Spec.Path + ".map";

  neverd_session_t Sess = neverd_session_create();
  neverd_session_set_map_path(Sess, MapPath.c_str());
  ASSERT_TRUE(neverd_session_load(Sess, Spec.Path.c_str()))
      << takeOwned(neverd_last_error(Sess));

  // An explicit --map outranks the PDB beside a PE and the DWARF inside an ELF
  // or Mach-O, so the run must report names with no types and no lines.
  EXPECT_EQ(takeOwned(neverd_session_debug_info_kind(Sess)), "map");
  EXPECT_TRUE(hasFunctionNamed(Sess, "leaks_memory"));

  neverd_safety_options Options{};
  Options.struct_size = sizeof(Options);
  const std::string Json = takeOwned(neverd_session_audit_json(Sess, &Options));
  neverd_session_destroy(Sess);

  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Json);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  const llvm::json::Object *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));

  // The heap defects are a property of the code, so dropping to a MAP must not
  // change which ones are found — only what the report can say about them.
  const bool HasCallerMay = Spec.Format != "ELF";
  EXPECT_EQ(Root->getInteger("unsafe").value_or(-1), 3);
  EXPECT_EQ(Root->getInteger("unknown").value_or(-1), HasCallerMay ? 4 : 2);
  EXPECT_TRUE(hasClass(*Root, "heap_leak"));
  EXPECT_TRUE(hasClass(*Root, "double_free"));
  EXPECT_TRUE(hasClass(*Root, "use_after_free"));
  const llvm::json::Object *LeaksMemory = findingNamed(*Root, "leaks_memory");
  ASSERT_NE(LeaksMemory, nullptr) << Spec.Path;
  EXPECT_EQ(LeaksMemory->getString("class"), "heap_leak");
  EXPECT_EQ(LeaksMemory->getString("verdict"), "UNKNOWN");
  EXPECT_TRUE(
      namesMatch(LeaksMemory->getString("name").value_or(""), "malloc"));
  EXPECT_EQ(LeaksMemory->getString("name_source"), "import");
  EXPECT_EQ(findingNamed(*Root, "guarded_free"), nullptr);

  const llvm::json::Array *Findings = Root->getArray("findings");
  ASSERT_NE(Findings, nullptr);
  ASSERT_EQ(Findings->size(), HasCallerMay ? 7u : 5u);
  struct ExpectedImportFinding {
    const char *Function;
    const char *Callee;
    const char *Class;
    const char *Verdict;
  };
  const std::vector<ExpectedImportFinding> ExpectedImports = {
      {"tainted_heap_overflow", "memcpy", "uninitialized_read", "UNKNOWN"},
      {"leaks_memory", "malloc", "heap_leak", "UNKNOWN"},
      {"leak_on_one_path", "malloc", "heap_leak", "UNSAFE"},
      {"double_frees", "free", "double_free", "UNSAFE"},
      {"uses_after_free", "free", "use_after_free", "UNSAFE"},
  };
  for (const ExpectedImportFinding &Expected : ExpectedImports) {
    const std::vector<const llvm::json::Object *> Matches =
        findingsAtCall(*Root, Expected.Function, Expected.Callee);
    ASSERT_EQ(Matches.size(), 1u)
        << Expected.Function << " -> " << Expected.Callee;
    EXPECT_EQ(Matches.front()->getString("name_source"), "import");
    EXPECT_EQ(Matches.front()->getString("class"), Expected.Class);
    EXPECT_EQ(Matches.front()->getString("verdict"), Expected.Verdict);
  }
  for (llvm::StringRef Callee :
       {llvm::StringRef("leaks_memory"), llvm::StringRef("guarded_free")}) {
    const std::vector<const llvm::json::Object *> CallerFindings =
        findingsAtCall(*Root, "main", Callee);
    if (!HasCallerMay) {
      EXPECT_TRUE(CallerFindings.empty()) << Callee.str();
      continue;
    }
    ASSERT_EQ(CallerFindings.size(), 1u) << Callee.str();
    const llvm::json::Object &CallerFinding = *CallerFindings.front();
    EXPECT_EQ(CallerFinding.getString("class"), "heap_leak");
    EXPECT_EQ(CallerFinding.getString("verdict"), "UNKNOWN");
    EXPECT_EQ(CallerFinding.getString("name_source"),
              Spec.Format == "PE" ? "map" : "export");
    EXPECT_TRUE(CallerFinding.getString("detail").value_or("").contains(
        "callee may return heap ownership"));
  }
  for (const llvm::json::Value &FV : *Findings) {
    const llvm::json::Object *O = FV.getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_FALSE(O->getString("source").has_value())
        << O->getString("function").value_or("?").str();
  }
}

TEST(SafetyCAPI, LegacyOptionsPrefixAndNullOptionsRemainSupported) {
  const FixtureSpec Spec = fixtureSpecs().front();
  neverd_session_t Sess = neverd_session_create();
  ASSERT_TRUE(neverd_session_load(Sess, Spec.Path.c_str()));

  struct LegacyOptions {
    size_t struct_size;
    unsigned max_paths;
  } Legacy{sizeof(LegacyOptions), 8};
  const char *Hunt = neverd_session_hunt_json(
      Sess, reinterpret_cast<const neverd_safety_options *>(&Legacy));
  ASSERT_NE(Hunt, nullptr);
  auto HuntJson = llvm::json::parse(Hunt);
  neverd_free_string(Hunt);
  ASSERT_TRUE(static_cast<bool>(HuntJson));
  ASSERT_NE(HuntJson->getAsObject(), nullptr);
  EXPECT_TRUE(HuntJson->getAsObject()->getBoolean("ok").value_or(false));

  const char *Audit = neverd_session_audit_json(Sess, nullptr);
  ASSERT_NE(Audit, nullptr);
  auto AuditJson = llvm::json::parse(Audit);
  neverd_free_string(Audit);
  ASSERT_TRUE(static_cast<bool>(AuditJson));
  ASSERT_NE(AuditJson->getAsObject(), nullptr);
  EXPECT_TRUE(AuditJson->getAsObject()->getBoolean("ok").value_or(false));
  neverd_session_destroy(Sess);
}

TEST(SafetyCAPI, InterproceduralBudgetsAppendAfterTheLegacyOptionsPrefix) {
  struct LegacyOptions {
    size_t struct_size;
    unsigned max_paths;
    unsigned max_steps;
    unsigned max_loop;
    unsigned long long solver_conflicts;
    const char *sinks_path;
    const char *sources_path;
  };

#define EXPECT_PREFIX_OFFSET(Field)                                            \
  EXPECT_EQ(offsetof(neverd_safety_options, Field),                            \
            offsetof(LegacyOptions, Field))
  EXPECT_PREFIX_OFFSET(struct_size);
  EXPECT_PREFIX_OFFSET(max_paths);
  EXPECT_PREFIX_OFFSET(max_steps);
  EXPECT_PREFIX_OFFSET(max_loop);
  EXPECT_PREFIX_OFFSET(solver_conflicts);
  EXPECT_PREFIX_OFFSET(sinks_path);
  EXPECT_PREFIX_OFFSET(sources_path);
#undef EXPECT_PREFIX_OFFSET

  constexpr size_t LegacyBoundary =
      offsetof(neverd_safety_options, sources_path) + sizeof(const char *);
  constexpr size_t CallDepthBoundary =
      offsetof(neverd_safety_options, max_call_depth) + sizeof(unsigned);
  constexpr size_t SummaryIterationsBoundary =
      offsetof(neverd_safety_options, max_summary_iterations) +
      sizeof(unsigned);
  EXPECT_EQ(sizeof(LegacyOptions), LegacyBoundary);
  EXPECT_EQ(offsetof(neverd_safety_options, max_call_depth), LegacyBoundary);
  EXPECT_EQ(offsetof(neverd_safety_options, max_summary_iterations),
            CallDepthBoundary);
  EXPECT_EQ(sizeof(neverd_safety_options), SummaryIterationsBoundary);

  if constexpr (sizeof(void *) == 8) {
    EXPECT_EQ(offsetof(neverd_safety_options, struct_size), 0u);
    EXPECT_EQ(offsetof(neverd_safety_options, max_paths), 8u);
    EXPECT_EQ(offsetof(neverd_safety_options, max_steps), 12u);
    EXPECT_EQ(offsetof(neverd_safety_options, max_loop), 16u);
    EXPECT_EQ(offsetof(neverd_safety_options, solver_conflicts), 24u);
    EXPECT_EQ(offsetof(neverd_safety_options, sinks_path), 32u);
    EXPECT_EQ(offsetof(neverd_safety_options, sources_path), 40u);
    EXPECT_EQ(LegacyBoundary, 48u);
    EXPECT_EQ(CallDepthBoundary, 52u);
    EXPECT_EQ(SummaryIterationsBoundary, 56u);
  }
}

TEST(SafetyCAPI, InterproceduralBudgetFieldsHonorStructSizeBoundaries) {
  const FixtureSpec Spec = fixtureSpecs().front();
  neverd_session_t Sess = neverd_session_create();
  ASSERT_TRUE(neverd_session_load(Sess, Spec.Path.c_str()));

  const auto Hunt = [&](neverd_safety_options Options) {
    const std::string Json =
        takeOwned(neverd_session_hunt_json(Sess, &Options));
    llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Json);
    EXPECT_TRUE(static_cast<bool>(Parsed));
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      return llvm::json::Value(nullptr);
    }
    return std::move(*Parsed);
  };
  const auto InnerReachability = [](const llvm::json::Value &Report) {
    const llvm::json::Object *Root = Report.getAsObject();
    if (!Root)
      return static_cast<const llvm::json::Object *>(nullptr);
    const llvm::json::Object *Finding =
        findingNamed(*Root, "interproc_forward_inner");
    return Finding ? Finding->getObject("reachability") : nullptr;
  };

  constexpr size_t LegacyBoundary =
      offsetof(neverd_safety_options, sources_path) + sizeof(const char *);
  constexpr size_t CallDepthBoundary =
      offsetof(neverd_safety_options, max_call_depth) + sizeof(unsigned);
  constexpr size_t SummaryBoundary =
      offsetof(neverd_safety_options, max_summary_iterations) +
      sizeof(unsigned);

  neverd_safety_options Options{};
  Options.max_call_depth = 1;
  Options.struct_size = LegacyBoundary;
  llvm::json::Value LegacyDepth = Hunt(Options);
  const llvm::json::Object *LegacyDepthReach = InnerReachability(LegacyDepth);
  ASSERT_NE(LegacyDepthReach, nullptr);
  EXPECT_EQ(LegacyDepthReach->getString("status"), "REACHABLE");

  Options.struct_size = CallDepthBoundary;
  llvm::json::Value AppendedDepth = Hunt(Options);
  const llvm::json::Object *AppendedDepthReach =
      InnerReachability(AppendedDepth);
  ASSERT_NE(AppendedDepthReach, nullptr);
  EXPECT_EQ(AppendedDepthReach->getString("status"), "UNKNOWN");

  Options = {};
  Options.max_summary_iterations = 1;
  Options.struct_size = CallDepthBoundary;
  llvm::json::Value LegacySummary = Hunt(Options);
  const llvm::json::Object *LegacySummaryReach =
      InnerReachability(LegacySummary);
  ASSERT_NE(LegacySummaryReach, nullptr);
  EXPECT_EQ(LegacySummaryReach->getString("attacker_control"), "TAINTED");

  Options.struct_size = SummaryBoundary;
  llvm::json::Value AppendedSummary = Hunt(Options);
  const llvm::json::Object *AppendedSummaryReach =
      InnerReachability(AppendedSummary);
  ASSERT_NE(AppendedSummaryReach, nullptr);
  EXPECT_EQ(AppendedSummaryReach->getString("status"), "REACHABLE");
  EXPECT_EQ(AppendedSummaryReach->getString("attacker_control"), "UNKNOWN");
  EXPECT_TRUE(AppendedSummaryReach->getBoolean("budget_hit").value_or(false));

  neverd_session_destroy(Sess);
}

TEST(SafetyCAPI, NullSessionReturnsAnOwnedErrorReport) {
  for (auto Analyze : {neverd_session_hunt_json, neverd_session_audit_json}) {
    const char *Raw = Analyze(nullptr, nullptr);
    ASSERT_NE(Raw, nullptr);
    auto Parsed = llvm::json::parse(Raw);
    neverd_free_string(Raw);
    ASSERT_TRUE(static_cast<bool>(Parsed));
    const llvm::json::Object *Root = Parsed->getAsObject();
    ASSERT_NE(Root, nullptr);
    EXPECT_FALSE(Root->getBoolean("ok").value_or(true));
    EXPECT_EQ(Root->getString("verdict"), "UNKNOWN");
    EXPECT_TRUE(Root->getString("error").has_value());
  }
}

TEST(SafetyCAPI, NativeExceptionsNeverCrossTheCABI) {
  neverd_session_t Sess = neverd_session_create();
  ASSERT_NE(Sess, nullptr);
  auto *Internal = neverd::sdk::toSession(Sess);

  struct AnalyzeCase {
    const char *(*Analyze)(neverd_session_t, const neverd_safety_options *);
    llvm::StringRef Track;
  } Cases[] = {
      {neverd_session_audit_json, "audit"},
      {neverd_session_hunt_json, "hunt"},
  };

  const auto ExpectInternalError = [&](const AnalyzeCase &Case,
                                       llvm::StringRef ExpectedMessage) {
    const char *Raw = Case.Analyze(Sess, nullptr);
    ASSERT_NE(Raw, nullptr);
    llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Raw);
    neverd_free_string(Raw);
    ASSERT_TRUE(static_cast<bool>(Parsed));
    const llvm::json::Object *Root = Parsed->getAsObject();
    ASSERT_NE(Root, nullptr);
    EXPECT_EQ(Root->getInteger("schema_version"), 1);
    EXPECT_EQ(Root->getBoolean("ok"), false);
    EXPECT_EQ(Root->getString("track"), Case.Track);
    EXPECT_EQ(Root->getString("verdict"), "UNKNOWN");
    const llvm::StringRef Error =
        Root->getString("error").value_or(llvm::StringRef());
    EXPECT_TRUE(Error.contains("internal_error"));
    EXPECT_TRUE(Error.contains(ExpectedMessage));
  };

  for (const AnalyzeCase &Case : Cases) {
    Internal->SafetyBeforeRunForTesting = [] {
      throw std::runtime_error("injected standard safety exception");
    };
    ExpectInternalError(Case, "injected standard safety exception");

    Internal->SafetyBeforeRunForTesting = [] { throw 7; };
    ExpectInternalError(Case, "non-standard native exception");

    Internal->SafetyBeforeRunForTesting = [] { throw std::bad_alloc(); };
    const char *AllocationFailure = Case.Analyze(Sess, nullptr);
    EXPECT_EQ(AllocationFailure, nullptr);
    neverd_free_string(AllocationFailure);
    EXPECT_NE(takeOwned(neverd_last_error(Sess)).find("allocation failed"),
              std::string::npos);
  }

  neverd_session_destroy(Sess);
}

TEST(SafetyCAPI, ExplicitMissingDebugCompanionsFailTheLoad) {
  const FixtureSpec Spec = fixtureSpecs().front();
  for (bool PDB : {false, true}) {
    neverd_session_t Sess = neverd_session_create();
    const std::string Missing =
        Spec.Path + (PDB ? ".missing.pdb" : ".missing.map");
    if (PDB)
      neverd_session_set_pdb_path(Sess, Missing.c_str());
    else
      neverd_session_set_map_path(Sess, Missing.c_str());
    EXPECT_FALSE(neverd_session_load(Sess, Spec.Path.c_str()));
    EXPECT_FALSE(takeOwned(neverd_last_error(Sess)).empty());
    neverd_session_destroy(Sess);
  }
}

TEST(SafetyCAPI, SessionRenameDrivesSafetySinkIdentity) {
  const FixtureSpec Spec = fixtureSpecs()[4];
  TemporaryFixture Temp(Spec.Path);
  ASSERT_TRUE(Temp.valid()) << Temp.error();

  neverd_session_t Sess = neverd_session_create();
  neverd_session_set_debug_info_enabled(Sess, 0);
  ASSERT_TRUE(neverd_session_load(Sess, Temp.path().string().c_str()))
      << takeOwned(neverd_last_error(Sess));
  ASSERT_GE(neverd_func_find_by_name(Sess, "sub_140001000"), 0);
  ASSERT_EQ(neverd_rename_func(Sess, "sub_140001000", "memcpy"), 0)
      << takeOwned(neverd_last_error(Sess));

  neverd_safety_options Options{};
  Options.struct_size = sizeof(Options);
  const std::string Json = takeOwned(neverd_session_hunt_json(Sess, &Options));
  neverd_session_destroy(Sess);

  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Json);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  const llvm::json::Object *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));
  const llvm::json::Object *Renamed = findingWithNameSource(*Root, "rename");
  ASSERT_NE(Renamed, nullptr);
  EXPECT_EQ(Renamed->getString("name"), "memcpy");
  EXPECT_EQ(Renamed->getString("sink"), "memcpy");
}

TEST(SafetyCAPI, MapOnlyStrippedImageDrivesSafetySinkIdentity) {
  const FixtureSpec Spec = fixtureSpecs()[4];
  TemporaryFixture Temp(Spec.Path);
  ASSERT_TRUE(Temp.valid()) << Temp.error();

  const std::filesystem::path MapPath =
      Temp.path().parent_path() / "identity.map";
  std::ifstream Input(Spec.Path + ".map", std::ios::binary);
  std::ofstream Output(MapPath, std::ios::binary);
  ASSERT_TRUE(Input.good());
  ASSERT_TRUE(Output.good());
  bool Replaced = false;
  for (std::string Line; std::getline(Input, Line);) {
    if (const size_t Pos = Line.find("tainted_stack_overflow");
        Pos != std::string::npos) {
      Line.replace(Pos, std::string("tainted_stack_overflow").size(), "memcpy");
      Replaced = true;
    }
    Output << Line << '\n';
  }
  Output.close();
  ASSERT_TRUE(Replaced);
  ASSERT_TRUE(Output.good());

  neverd_session_t Sess = neverd_session_create();
  neverd_session_set_map_path(Sess, MapPath.string().c_str());
  ASSERT_TRUE(neverd_session_load(Sess, Temp.path().string().c_str()))
      << takeOwned(neverd_last_error(Sess));
  EXPECT_EQ(takeOwned(neverd_session_debug_info_kind(Sess)), "map");
  const int MappedIndex = neverd_func_find_by_name(Sess, "memcpy");
  ASSERT_GE(MappedIndex, 0);

  const neverd_va_t Entry = neverd_func_entry(Sess, MappedIndex);
  unsigned char Bytes[16]{};
  ASSERT_EQ(neverd_read_bytes(Sess, Entry, Bytes, sizeof(Bytes)),
            static_cast<int>(sizeof(Bytes)));
  std::string Pattern;
  for (unsigned char Byte : Bytes)
    Pattern += llvm::utohexstr(Byte, /*LowerCase=*/false, 2);
  Pattern += " 00 0000 0010 :0000 strcpy\n";
  const std::filesystem::path SignaturePath =
      Temp.path().parent_path() / "stated-name.pat";
  {
    std::ofstream Signature(SignaturePath, std::ios::binary);
    ASSERT_TRUE(Signature.good());
    Signature << Pattern;
    ASSERT_TRUE(Signature.good());
  }
  ASSERT_GT(neverd_apply_signature_file(Sess, SignaturePath.string().c_str()),
            0)
      << takeOwned(neverd_last_error(Sess));
  EXPECT_EQ(takeOwned(neverd_func_name(Sess, MappedIndex)), "memcpy");

  neverd_safety_options Options{};
  Options.struct_size = sizeof(Options);
  const std::string Json = takeOwned(neverd_session_hunt_json(Sess, &Options));
  neverd_session_destroy(Sess);

  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Json);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  const llvm::json::Object *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));
  const llvm::json::Object *Mapped = findingWithNameSource(*Root, "map");
  ASSERT_NE(Mapped, nullptr);
  EXPECT_EQ(Mapped->getString("name"), "memcpy");
  EXPECT_EQ(Mapped->getString("sink"), "memcpy");
}

TEST(SafetyCAPI, SignatureOnlyNameDrivesSafetySinkIdentity) {
  const FixtureSpec Spec = fixtureSpecs()[4];
  TemporaryFixture Temp(Spec.Path);
  ASSERT_TRUE(Temp.valid()) << Temp.error();

  neverd_session_t Sess = neverd_session_create();
  neverd_session_set_debug_info_enabled(Sess, 0);
  ASSERT_TRUE(neverd_session_load(Sess, Temp.path().string().c_str()))
      << takeOwned(neverd_last_error(Sess));

  const int Target = neverd_func_find_by_name(Sess, "sub_140001000");
  ASSERT_GE(Target, 0);
  const neverd_va_t Entry = neverd_func_entry(Sess, Target);
  unsigned char Bytes[16]{};
  ASSERT_EQ(neverd_read_bytes(Sess, Entry, Bytes, sizeof(Bytes)),
            static_cast<int>(sizeof(Bytes)));

  std::string Pattern;
  for (unsigned char Byte : Bytes)
    Pattern += llvm::utohexstr(Byte, /*LowerCase=*/false, 2);
  Pattern += " 00 0000 0010 :0000 memcpy\n";
  const std::filesystem::path SignaturePath =
      Temp.path().parent_path() / "identity.pat";
  {
    std::ofstream Output(SignaturePath, std::ios::binary);
    ASSERT_TRUE(Output.good());
    Output << Pattern;
    ASSERT_TRUE(Output.good());
  }

  ASSERT_GT(neverd_apply_signature_file(Sess, SignaturePath.string().c_str()),
            0)
      << takeOwned(neverd_last_error(Sess));
  ASSERT_GE(neverd_func_find_by_name(Sess, "memcpy"), 0);

  neverd_safety_options Options{};
  Options.struct_size = sizeof(Options);
  const std::string Json = takeOwned(neverd_session_hunt_json(Sess, &Options));
  neverd_session_destroy(Sess);

  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Json);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  const llvm::json::Object *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));
  const llvm::json::Object *Signed = findingWithNameSource(*Root, "sig");
  ASSERT_NE(Signed, nullptr);
  EXPECT_EQ(Signed->getString("name"), "memcpy");
  EXPECT_EQ(Signed->getString("sink"), "memcpy");
}

INSTANTIATE_TEST_SUITE_P(Fixtures, SafetyIntegration,
                         ::testing::ValuesIn(fixtureSpecs()),
                         [](const ::testing::TestParamInfo<FixtureSpec> &Info) {
                           std::string Name =
                               Info.param.Format + "_" + Info.param.Arch;
                           for (char &C : Name)
                             if (C == '-')
                               C = '_';
                           return Name;
                         });
