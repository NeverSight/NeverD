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

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sdk/NeverDCAPISafety.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

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
bool runTrack(const std::string &Path, bool Hunt, llvm::json::Value &Out) {
  if (Path.empty())
    return false;

  neverd_session_t Sess = neverd_session_create();
  if (!neverd_session_load(Sess, Path.c_str())) {
    neverd_session_destroy(Sess);
    return false;
  }
  neverd_safety_options Options{};
  Options.struct_size = sizeof(Options);
  const char *Json = Hunt ? neverd_session_hunt_json(Sess, &Options)
                          : neverd_session_audit_json(Sess, &Options);
  bool Ok = false;
  if (Json) {
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

  EXPECT_EQ(Root->getInteger("scanned").value_or(-1), 4);
  EXPECT_EQ(Root->getInteger("skipped").value_or(-1), 1);
  EXPECT_EQ(Root->getInteger("unsafe").value_or(-1), 2);
  EXPECT_EQ(Root->getInteger("safe").value_or(0) +
                Root->getInteger("unknown").value_or(0),
            2);

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

TEST_P(SafetyIntegration, AuditReportsHeapDefects) {
  llvm::json::Value V(nullptr);
  const FixtureSpec &Spec = GetParam();
  ASSERT_TRUE(runTrack(Spec.Path, /*Hunt=*/false, V)) << Spec.Path;
  const llvm::json::Object *Root = V.getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));
  expectFixtureIdentity(*Root, Spec);

  EXPECT_EQ(Root->getInteger("scanned").value_or(-1), 7);
  EXPECT_EQ(Root->getInteger("unsafe").value_or(-1), 4);
  EXPECT_EQ(Root->getInteger("unknown").value_or(-1), 0);
  EXPECT_TRUE(hasClass(*Root, "heap_leak"));
  EXPECT_TRUE(hasClass(*Root, "double_free"));
  EXPECT_TRUE(hasClass(*Root, "use_after_free"));

  if (const llvm::json::Object *O = findingNamed(*Root, "leaks_memory"))
    EXPECT_EQ(O->getString("class"), "heap_leak");
  if (const llvm::json::Object *O = findingNamed(*Root, "leak_on_one_path"))
    EXPECT_EQ(O->getString("class"), "heap_leak");
  EXPECT_EQ(findingNamed(*Root, "guarded_free"), nullptr);
  EXPECT_EQ(findingNamed(*Root, "balanced_alloc"), nullptr);
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

    for (const llvm::json::Value &FV : *Findings) {
      const llvm::json::Object *O = FV.getAsObject();
      ASSERT_NE(O, nullptr);
      const std::string Where = Spec.Format + " " + Spec.Arch + " " +
                                O->getString("name").value_or("?").str() +
                                " @ " +
                                O->getString("call_va").value_or("?").str();

      // Each sink the fixtures reach is a runtime import, so the IAT, the PLT,
      // and the dyld bind table must every one of them yield the callee name.
      // A signature guess or a placeholder here means that format's import
      // path was never consulted, which is how a whole format silently stops
      // being analysed while the counts still look right.
      EXPECT_EQ(O->getString("name_source"), "import") << Where;

      // The enclosing function is named by the companion, not by discovery.
      const std::optional<llvm::StringRef> Fn = O->getString("function");
      ASSERT_TRUE(Fn.has_value()) << Where;
      EXPECT_TRUE(isFixtureFunction(*Fn)) << Where;

      // DWARF states the line a call sits on; a PDB carries names and
      // addresses only, so the report omits the key rather than inventing one.
      const std::optional<llvm::StringRef> Loc = O->getString("source");
      if (Spec.Format == "PE") {
        EXPECT_FALSE(Loc.has_value()) << Where;
      } else {
        ASSERT_TRUE(Loc.has_value()) << Where;
        EXPECT_TRUE(Loc->contains("safety_cases.c:"))
            << Where << " -> " << Loc->str();
      }
    }
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
  EXPECT_TRUE(hasClass(*Root, "heap_leak"));
  EXPECT_TRUE(hasClass(*Root, "double_free"));
  EXPECT_TRUE(hasClass(*Root, "use_after_free"));
  EXPECT_EQ(findingNamed(*Root, "guarded_free"), nullptr);

  const llvm::json::Array *Findings = Root->getArray("findings");
  ASSERT_NE(Findings, nullptr);
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
