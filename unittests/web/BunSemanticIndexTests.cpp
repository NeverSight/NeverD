//===- BunSemanticIndexTests.cpp - Source navigation evidence tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "BunFixture.h"
#include "BunSemanticIndex.h"
#include "Internal.h"
#include "gtest/gtest.h"

#include "llvm/Support/FileSystem.h"

#include <filesystem>
#include <fstream>
#include <set>

using namespace neverd::web;

TEST(WebBunSemanticIndex,
     UsesSharedOwnersAndExactOriginalSpansForAllSourceDomains) {
  const std::string Source =
      "import {readFile as r} from '/$bunfs/root/helper.js';"
      "const tool={name:'Read',inputSchema:{},call:async(p)=>p};"
      "registerTool('Read',{},tool.call);"
      "const "
      "settings={permissions:{allow:[]},hooks:{PreToolUse:[]},mcpServers:{}};"
      "client.setRequestHandler(schema,()=>{});"
      "const state={sessionId:'s'};ui.createElement('div',null);"
      "export {tool as T};";
  const std::vector<BunSemanticModuleName> Names{
      {7, u"/$bunfs/root/helper.js"}};
  const auto R = inspectBunSemanticSource("source", Source, "module", Names);
  ASSERT_EQ(R.Status, "bounded_syntax_inventory");
  EXPECT_EQ(R.ParseStatus, "parsed");
  EXPECT_EQ(R.BindingStatus, "ok");
  EXPECT_EQ(R.ModuleStatus, "ok");
  std::set<std::string> Domains;
  bool Linked = false, ExportName = false;
  for (const auto &Item : R.Records) {
    const auto *Row = Item.getAsObject();
    ASSERT_NE(Row, nullptr);
    const auto Domain = Row->getString("domain");
    ASSERT_TRUE(Domain);
    Domains.insert(Domain->str());
    const auto Start = Row->getString("start"), End = Row->getString("end");
    ASSERT_TRUE(Start && End);
    uint64_t A = 0, B = 0;
    ASSERT_FALSE(Start->getAsInteger(10, A));
    ASSERT_FALSE(End->getAsInteger(10, B));
    ASSERT_LE(A, B);
    ASSERT_LE(B, Source.size());
    EXPECT_EQ(Row->getString("exact_excerpt"),
              Source.substr(A, std::min<uint64_t>(B - A, 160)));
    if (Row->getString("role") == "esm_import") {
      EXPECT_EQ(Row->getInteger("target_module_index"), 7);
      Linked = true;
    }
    if (Row->getString("role") == "export") {
      EXPECT_EQ(Row->getString("original_name_display"), "T");
      EXPECT_EQ(Row->getString("original_binding_display"), "tool");
      ExportName = true;
    }
    if (*Domain != "modules")
      EXPECT_EQ(Row->getString("attribution"), "semantic_alias_inference");
  }
  EXPECT_TRUE(Linked);
  EXPECT_TRUE(ExportName);
  const std::set<std::string> Expected{"modules",    "tool", "configuration",
                                       "permission", "hook", "mcp",
                                       "session",    "ui"};
  EXPECT_EQ(Domains, Expected);
}

TEST(WebBunSemanticIndex, DoesNotTurnCommentOrSubstringMatchesIntoASTEvidence) {
  const std::string Source =
      "// sessionId createElement registerTool('Read',{},x)\n"
      "const note='description mentioning createElement(sessionId) and Bash';";
  const auto R = inspectBunSemanticSource("source", Source, "module", {});
  ASSERT_EQ(R.Status, "bounded_syntax_inventory");
  EXPECT_TRUE(R.Records.empty());
}

TEST(WebBunSemanticIndex, ExactUTF16NamesSurvivePrivateDisclosure) {
  const std::string Source =
      R"JS(const t={name:'\u00e9\uD800',inputSchema:{},call:()=>1};)JS";
  const auto R = inspectBunSemanticSource("source", Source, "module", {});
  ASSERT_EQ(R.Status, "bounded_syntax_inventory");
  ASSERT_EQ(R.Records.size(), 1);
  const auto *Row = R.Records.front().getAsObject();
  ASSERT_NE(Row, nullptr);
  const auto *Units = Row->getArray("original_name_utf16");
  ASSERT_NE(Units, nullptr);
  ASSERT_EQ(Units->size(), 2);
  EXPECT_EQ((*Units)[0].getAsInteger(), 0xe9);
  EXPECT_EQ((*Units)[1].getAsInteger(), 0xd800);
}

TEST(WebBunSemanticIndex, BudgetRefusalsDoNotClaimNoFeatures) {
  const std::string Source = "import '/$bunfs/root/a.js';export const x=1;";
  BunSemanticLimits Budget;
  Budget.Records = 1;
  auto R = inspectBunSemanticSource("source", Source, "module", {}, Budget);
  EXPECT_EQ(R.Status, "bun_semantic_record_budget_exceeded");
  EXPECT_LE(R.Records.size(), 1);
  Budget = {};
  Budget.Units = 1;
  R = inspectBunSemanticSource("source", Source, "module", {}, Budget);
  EXPECT_EQ(R.Status, "bun_semantic_name_budget_exceeded");
  Budget = {};
  Budget.OutputBytes = 2047;
  R = inspectBunSemanticSource("source", Source, "module", {}, Budget);
  EXPECT_EQ(R.Status, "bun_semantic_output_budget_exceeded");
  Budget = {};
  Budget.Records = BunSemanticLimits::HardRecords + 1;
  EXPECT_THROW(inspectBunSemanticSource("source", Source, "module", {}, Budget),
               Error);
}

TEST(WebBunSemanticIndex, NeverAdmitsRecoveryOnlySourceToSemanticConsumers) {
  const auto R = inspectBunSemanticSource(
      "source", std::string(8 * 1024 * 1024 + 1, ' '), "module", {});
  EXPECT_EQ(R.Status, "source_byte_budget_exceeded");
  EXPECT_TRUE(R.Records.empty());
  EXPECT_EQ(R.ModuleStatus, "not_run");
}

TEST(WebBunSemanticIndex,
     ExportBindsNavigationToReadBackVerifiedOriginalFiles) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-bun-semantic", Directory));
  const std::filesystem::path Root(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Path;
    ~Cleanup() {
      std::error_code EC;
      std::filesystem::remove_all(Path, EC);
    }
  } Cleanup{Root};
  const neverd::web::test::BunFixture F;
  {
    std::ofstream Out(Root / "input", std::ios::binary);
    Out.write(F.Bytes.data(), F.Bytes.size());
    ASSERT_TRUE(bool(Out));
  }
  const auto Snapshot = capture((Root / "input").string(), Limits{});
  const auto E = extractBun(Snapshot.Artifacts.front());
  auto Reply =
      llvm::json::parse(exportBunSemanticIndex(E, (Root / "index").string()));
  ASSERT_TRUE(bool(Reply));
  auto Read = [&](std::string_view Name) {
    std::ifstream In(Root / "index" / Name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(In), {});
  };
  auto Manifest = llvm::json::parse(Read("manifest.json"));
  ASSERT_TRUE(bool(Manifest));
  const auto *M = Manifest->getAsObject();
  ASSERT_NE(M, nullptr);
  EXPECT_EQ(M->getBoolean("source_execution"), false);
  EXPECT_EQ(M->getBoolean("binding_rewrites"), false);
  EXPECT_EQ(M->getString("erased_types"), "unavailable");
  EXPECT_EQ(M->getString("index_sha256"), sha256(Read("index.html")));
  const auto *Modules = M->getArray("modules");
  ASSERT_NE(Modules, nullptr);
  ASSERT_EQ(Modules->size(), 3);
  const auto *First = Modules->front().getAsObject();
  ASSERT_NE(First, nullptr);
  EXPECT_EQ(First->getString("original_sha256"),
            sha256(Read("m00000.original.js")));
  EXPECT_EQ(First->getString("readable_sha256"),
            sha256(Read("m00000.readable.js")));
  EXPECT_EQ(Read("m00000.original.js"), bunSourceBytes(E, E.Modules[0], 4096));
  EXPECT_NE(Read("index.html").find("captured_entry_module"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Root / "index" / "original.bin"));
  EXPECT_FALSE(std::filesystem::exists(Root / "opaque.asset"));
  BunSemanticLimits Budget;
  Budget.Records = 2;
  EXPECT_THROW(
      exportBunSemanticIndex(E, (Root / "record-refusal").string(), Budget),
      Error);
  EXPECT_FALSE(
      std::filesystem::exists(Root / "record-refusal" / "manifest.json"));
  Budget = {};
  Budget.Units = 1;
  EXPECT_THROW(
      exportBunSemanticIndex(E, (Root / "name-refusal").string(), Budget),
      Error);
  EXPECT_FALSE(std::filesystem::exists(Root / "name-refusal"));
}
