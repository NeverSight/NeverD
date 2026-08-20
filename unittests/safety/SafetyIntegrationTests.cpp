//===- SafetyIntegrationTests.cpp - End-to-end audit / hunt --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Loads the host-native memory-safety fixture and drives the public C API,
/// asserting that the hunt and audit verdicts hold on a real lifted binary.
/// The fixture is compiled for whatever format the build host produces (Mach-O,
/// ELF, or PE), so the same expectations are checked against each format across
/// the CI matrix.  Additional cross-compiled copies are tried when a toolchain
/// is present.  A missing extra-format binary, or a lift the host cannot
/// complete for that extra copy, is a skip; the host-native fixture must load.
///
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sdk/NeverDCAPISafety.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include "gtest/gtest.h"

#include <optional>
#include <string>
#include <vector>

#ifndef SAFETY_FIXTURE_NATIVE
#define SAFETY_FIXTURE_NATIVE ""
#endif
#ifndef SAFETY_FIXTURE_EXTRA
#define SAFETY_FIXTURE_EXTRA ""
#endif

namespace {

std::vector<std::string> fixturePaths() {
  std::vector<std::string> Out;
  if (llvm::StringRef Native = SAFETY_FIXTURE_NATIVE; !Native.empty())
    Out.emplace_back(Native);
  llvm::SmallVector<llvm::StringRef, 4> Extra;
  llvm::SplitString(SAFETY_FIXTURE_EXTRA, Extra, ",");
  for (llvm::StringRef P : Extra)
    if (!P.empty())
      Out.emplace_back(P);
  return Out;
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

// Extra-format copies are compiled when a cross toolchain exists; a lift that
// the host cannot complete is a skip, not a failure of the safety engine.
std::optional<std::string> extraLiftFailure(const llvm::json::Object *Root,
                                            const std::string &Path) {
  if (Root && Root->getBoolean("ok").value_or(false))
    return std::nullopt;
  if (Path == SAFETY_FIXTURE_NATIVE)
    return std::nullopt;
  if (Root)
    if (auto E = Root->getString("error"))
      return E->str();
  return std::string("lift failed");
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

bool namesMatch(llvm::StringRef Have, llvm::StringRef Want) {
  return Have == Want || Have == ("_" + Want).str();
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

class SafetyIntegration : public ::testing::TestWithParam<std::string> {};

} // namespace

TEST_P(SafetyIntegration, HuntReportsTaintedOverflowWithImportIdentity) {
  llvm::json::Value V(nullptr);
  if (!runTrack(GetParam(), /*Hunt=*/true, V)) {
    ASSERT_NE(GetParam(), std::string(SAFETY_FIXTURE_NATIVE))
        << "host-native fixture could not be loaded: " << GetParam();
    GTEST_SKIP() << "fixture could not be loaded: " << GetParam();
  }
  const llvm::json::Object *Root = V.getAsObject();
  if (auto Skip = extraLiftFailure(Root, GetParam()))
    GTEST_SKIP() << *Skip;
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));

  // At least one copy is proven unsafe, and at least one is proven safe.
  EXPECT_GE(Root->getInteger("unsafe").value_or(0), 1);
  EXPECT_GE(Root->getInteger("safe").value_or(0), 1);

  // At least one unsafe copy resolves its libc callee through the import table
  // and carries a concrete witness.  Extra copies inside a runtime library
  // may use a different identity origin and must not fail the fixture.
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

  if (const llvm::json::Object *O = findingNamed(*Root, "bounded_copy_is_safe"))
    EXPECT_EQ(O->getString("verdict"), "SAFE");
  if (const llvm::json::Object *O = findingNamed(*Root, "strlen_guarded_copy"))
    EXPECT_NE(O->getString("verdict"), "UNSAFE");
}

TEST_P(SafetyIntegration, AuditReportsHeapDefects) {
  llvm::json::Value V(nullptr);
  if (!runTrack(GetParam(), /*Hunt=*/false, V)) {
    ASSERT_NE(GetParam(), std::string(SAFETY_FIXTURE_NATIVE))
        << "host-native fixture could not be loaded: " << GetParam();
    GTEST_SKIP() << "fixture could not be loaded: " << GetParam();
  }
  const llvm::json::Object *Root = V.getAsObject();
  if (auto Skip = extraLiftFailure(Root, GetParam()))
    GTEST_SKIP() << *Skip;
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));

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
  if (!runTrack(GetParam(), /*Hunt=*/true, V)) {
    ASSERT_NE(GetParam(), std::string(SAFETY_FIXTURE_NATIVE))
        << "host-native fixture could not be loaded: " << GetParam();
    GTEST_SKIP() << "fixture could not be loaded: " << GetParam();
  }
  const llvm::json::Object *Root = V.getAsObject();
  if (auto Skip = extraLiftFailure(Root, GetParam()))
    GTEST_SKIP() << *Skip;
  ASSERT_NE(Root, nullptr);
  ASSERT_TRUE(Root->getBoolean("ok").value_or(false));
  const llvm::json::Array *Findings = Root->getArray("findings");
  ASSERT_NE(Findings, nullptr);
  const int64_t Total = Root->getInteger("unsafe").value_or(0) +
                        Root->getInteger("safe").value_or(0) +
                        Root->getInteger("unknown").value_or(0);
  EXPECT_EQ(Total, static_cast<int64_t>(Findings->size()));
}

INSTANTIATE_TEST_SUITE_P(Fixtures, SafetyIntegration,
                         ::testing::ValuesIn(fixturePaths()));
