//===- BunProjectTests.cpp - Bun reconstruction export tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source/asset reconstruction candidates retain exact evidence and safe names.
///
//===----------------------------------------------------------------------===//

#include "BunFixture.h"
#include "BunProject.h"
#include "Internal.h"
#include "gtest/gtest.h"

#include "llvm/Support/FileSystem.h"

#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;
using namespace neverd::web::test;

uint64_t diagnosticTotal(const llvm::json::Object &Manifest) {
  uint64_t Count = 0;
  auto Add = [&](const llvm::json::Object &Row, llvm::StringRef Key) {
    if (const auto *Items = Row.getArray(Key))
      Count += Items->size();
  };
  if (const auto *Providers = Manifest.getArray("require_provider_inventory"))
    for (const auto &Provider : *Providers)
      if (const auto *Row = Provider.getAsObject())
        Add(*Row, "diagnostics");
  if (const auto *Modules = Manifest.getArray("modules"))
    for (const auto &Module : *Modules)
      if (const auto *Row = Module.getAsObject()) {
        for (const auto Key : {"diagnostics", "require_alias_diagnostics",
                               "readable_diagnostics"})
          Add(*Row, Key);
        if (Row->getString("loader_status") ==
            "unsupported_reconstruction_loader")
          ++Count;
      }
  return Count;
}

class WebBunProject : public ::testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-bun-project", Directory));
    Root = Directory.str().str();
  }
  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  BunExtraction extraction(std::string_view Bytes) {
    const auto Path = Root / "input.bin";
    {
      std::ofstream F(Path, std::ios::binary);
      F.write(Bytes.data(), Bytes.size());
      if (!F)
        throw std::runtime_error("fixture_write");
    }
    return extractBun(capture(Path.string(), Limits{}).Artifacts.at(0));
  }
  std::string read(std::string_view Name) {
    std::ifstream F(Root / "project" / Name, std::ios::binary);
    return {std::istreambuf_iterator<char>(F), {}};
  }
  void source0(BunFixture &F, std::string_view Text) {
    const auto At = get(F.Graph, F.Table + 8, 4);
    const auto Size = get(F.Graph, F.Table + 12, 4);
    ASSERT_LE(Text.size(), Size);
    F.Graph.replace(At, Size,
                    std::string(Text) + std::string(Size - Text.size(), ' '));
  }
  void assetAsSource(BunFixture &F) {
    put(F.Graph, F.Table + 2 * 52 + 48, 1, 1);
    put(F.Graph, F.Table + 2 * 52 + 49, 1, 1);
    put(F.Graph, F.Table + 2 * 52 + 50, 1, 1);
  }
};

TEST_F(WebBunProject, ExactSourcesAndAssetsUseFixedNamesAndNoExecutable) {
  const BunFixture F;
  const auto E = extraction(F.Bytes);
  auto Result =
      llvm::json::parse(exportBunProject(E, (Root / "project").string()));
  ASSERT_TRUE(bool(Result));
  const auto *O = Result->getAsObject();
  ASSERT_NE(O, nullptr);
  EXPECT_EQ(O->getInteger("source_count"), 2);
  EXPECT_EQ(O->getInteger("asset_count"), 1);
  EXPECT_EQ(O->getInteger("failed_source_count"), 0);
  EXPECT_EQ(O->getBoolean("includes_original_executable"), false);
  EXPECT_EQ(O->getBoolean("executes_input"), false);
  EXPECT_EQ(read("m00000.js"), bunSourceBytes(E, E.Modules[0], 4096));
  EXPECT_EQ(read("m00001.js"), bunSourceBytes(E, E.Modules[1], 4096));
  EXPECT_EQ(read("m00002.bin"),
            E.Regions[E.Modules[2].Contents].Content.read(0, 16));
  EXPECT_FALSE(std::filesystem::exists(Root / "project" / "original.bin"));
  EXPECT_FALSE(std::filesystem::exists(Root / "opaque.asset"));
  EXPECT_EQ(read("entry.js"),
            "import \"./m00000.js\";\nimport \"./m00001.js\";\n");
  EXPECT_THROW(exportBunProject(E, (Root / "project").string()), Error);
}

TEST_F(WebBunProject, TextAssetsDecodeWithPreservedRawEvidence) {
  BunFixture F(true, "map", std::string_view("caf\xe9", 4));
  put(F.Graph, F.Table + 2 * 52 + 48, 1, 1);
  put(F.Graph, F.Table + 2 * 52 + 49, 13, 1);
  const auto E = extraction(bunELF(F.Graph));
  exportBunProject(E, (Root / "project").string());
  EXPECT_EQ(read("m00002.txt"), "caf\xc3\xa9");
  EXPECT_EQ(read("m00002.storage.bin"), std::string("caf\xe9", 4));
}

TEST_F(WebBunProject, MetadataRecordBudgetIsSharedAcrossModules) {
  const BunFixture F;
  const auto E = extraction(F.Bytes);
  BunProjectLimits Budget;
  Budget.Records = 1;
  try {
    exportBunProject(E, (Root / "project").string(), Budget);
    FAIL() << "Expected the second module to exceed the project record budget";
  } catch (const Error &Error) {
    EXPECT_STREQ(Error.what(), "bun_project_metadata_budget_exceeded");
  }
  EXPECT_TRUE(std::filesystem::exists(Root / "project" / "m00000.js"));
  EXPECT_FALSE(std::filesystem::exists(Root / "project" / "m00001.js"));
  EXPECT_FALSE(std::filesystem::exists(Root / "project" / "manifest.json"));
}

TEST_F(WebBunProject, MetadataStringBudgetIsSharedAcrossSourceInventories) {
  const std::string Name = "/$bunfs/root/missing";
  const std::string Source = "const p='" + Name + "';";
  BunFixture F(true, "map", Source);
  source0(F, Source);
  assetAsSource(F);
  const auto E = extraction(bunELF(F.Graph));
  BunProjectLimits Budget;
  Budget.UTF16Units = Name.size();
  try {
    exportBunProject(E, (Root / "project").string(), Budget);
    FAIL() << "Expected the second path inventory to exceed the shared budget";
  } catch (const Error &Error) {
    EXPECT_STREQ(Error.what(), "bun_project_metadata_budget_exceeded");
  }
  EXPECT_TRUE(std::filesystem::exists(Root / "project" / "m00000.js"));
  EXPECT_FALSE(std::filesystem::exists(Root / "project" / "m00002.js"));
  EXPECT_FALSE(std::filesystem::exists(Root / "project" / "manifest.json"));
}

TEST_F(WebBunProject, MetadataLimitsCannotRaiseHardCeilings) {
  const BunFixture F;
  const auto E = extraction(F.Bytes);
  for (const BunProjectLimits Budget :
       {BunProjectLimits{BunProjectLimits::HardRecords + 1,
                         BunProjectLimits::HardUTF16Units},
        BunProjectLimits{BunProjectLimits::HardRecords,
                         BunProjectLimits::HardUTF16Units + 1}}) {
    try {
      exportBunProject(E, (Root / "project").string(), Budget);
      FAIL() << "Expected an invalid raised metadata limit";
    } catch (const Error &Error) {
      EXPECT_STREQ(Error.what(), "bun_project_invalid_limits");
    }
  }
  EXPECT_FALSE(std::filesystem::exists(Root / "project"));
}

TEST_F(WebBunProject, DiagnosticTotalIncludesProviderAndUnsupportedLoader) {
  BunFixture F;
  source0(F, "var r=import.meta.require;export{r};r=0;");
  put(F.Graph, F.Table + 2 * 52 + 49, 4, 1);
  const auto E = extraction(bunELF(F.Graph));
  auto Summary =
      llvm::json::parse(exportBunProject(E, (Root / "project").string()));
  ASSERT_TRUE(bool(Summary));
  auto Manifest = llvm::json::parse(read("manifest.json"));
  ASSERT_TRUE(bool(Manifest));
  const auto *M = Manifest->getAsObject();
  ASSERT_NE(M, nullptr);
  const auto *Providers = M->getArray("require_provider_inventory");
  ASSERT_NE(Providers, nullptr);
  ASSERT_EQ(Providers->size(), 1);
  const auto *Provider = Providers->front().getAsObject();
  ASSERT_NE(Provider, nullptr);
  const auto *Problems = Provider->getArray("diagnostics");
  ASSERT_NE(Problems, nullptr);
  EXPECT_EQ(Problems->size(), 1);
  EXPECT_EQ(diagnosticTotal(*M), 2);
  EXPECT_EQ(M->getInteger("diagnostic_count"), diagnosticTotal(*M));
  EXPECT_EQ(Summary->getAsObject()->getInteger("diagnostic_count"),
            diagnosticTotal(*M));
  EXPECT_EQ(M->getInteger("readable_source_count"), 2);
  EXPECT_EQ(M->getInteger("unavailable_readable_count"), 0);
}

TEST_F(WebBunProject, DiagnosticTotalIncludesConsumerProofProblems) {
  const std::string Consumer = "import {r} from '/$bunfs/root/SECRET.js';"
                               "const p='/$bunfs/root/UNICODE.js';r(p);";
  BunFixture F(true, "map", Consumer);
  source0(F, "var r=import.meta.require;export{r};");
  assetAsSource(F);
  const auto E = extraction(bunELF(F.Graph));
  exportBunProject(E, (Root / "project").string());
  auto Manifest = llvm::json::parse(read("manifest.json"));
  ASSERT_TRUE(bool(Manifest));
  const auto *M = Manifest->getAsObject();
  ASSERT_NE(M, nullptr);
  const auto *Modules = M->getArray("modules");
  ASSERT_NE(Modules, nullptr);
  ASSERT_EQ(Modules->size(), 3);
  const auto *Row = (*Modules)[2].getAsObject();
  ASSERT_NE(Row, nullptr);
  const auto *Problems = Row->getArray("require_alias_diagnostics");
  ASSERT_NE(Problems, nullptr);
  EXPECT_EQ(Problems->size(), 1);
  EXPECT_EQ(Row->getString("relocation_status"),
            "verified_literal_relocations");
  EXPECT_EQ(diagnosticTotal(*M), 2);
  EXPECT_EQ(M->getInteger("diagnostic_count"), diagnosticTotal(*M));
  EXPECT_EQ(M->getInteger("readable_source_count"), 3);
  EXPECT_EQ(M->getInteger("unavailable_readable_count"), 0);
}

TEST_F(WebBunProject, ProviderImportWithOnlyExternalCallRetainsExactReference) {
  const std::string Consumer = "import {r} from '/$bunfs/root/SECRET.js';"
                               "r('child_process');";
  BunFixture F(true, "map", Consumer);
  source0(F, "var r=import.meta.require;export{r};");
  assetAsSource(F);
  const auto E = extraction(bunELF(F.Graph));
  exportBunProject(E, (Root / "project").string());
  auto Manifest = llvm::json::parse(read("manifest.json"));
  ASSERT_TRUE(bool(Manifest));
  const auto *M = Manifest->getAsObject();
  ASSERT_NE(M, nullptr);
  const auto *Modules = M->getArray("modules");
  ASSERT_NE(Modules, nullptr);
  ASSERT_EQ(Modules->size(), 3);
  const auto *Row = (*Modules)[2].getAsObject();
  ASSERT_NE(Row, nullptr);
  EXPECT_EQ(Row->getString("require_alias_status"), "ok");
  const auto *Problems = Row->getArray("require_alias_diagnostics");
  ASSERT_NE(Problems, nullptr);
  EXPECT_TRUE(Problems->empty());
  const auto *References = Row->getArray("external_module_references");
  ASSERT_NE(References, nullptr);
  ASSERT_EQ(References->size(), 1);
  const auto *Reference = References->front().getAsObject();
  ASSERT_NE(Reference, nullptr);
  const auto Start = Consumer.find("'child_process'");
  EXPECT_EQ(Reference->getString("start"), std::to_string(Start));
  EXPECT_EQ(Reference->getString("end"), std::to_string(Start + 15));
  const auto *Units = Reference->getArray("target_utf16");
  ASSERT_NE(Units, nullptr);
  ASSERT_EQ(Units->size(), 13);
  for (size_t I = 0; I < Units->size(); ++I)
    EXPECT_EQ((*Units)[I].getAsInteger(), int64_t(u"child_process"[I]));
  EXPECT_EQ(M->getInteger("external_module_reference_count"), 1);
  EXPECT_EQ(M->getInteger("unresolved_runtime_reference_count"), 0);
  EXPECT_EQ(M->getInteger("require_alias_relocation_count"), 0);
  EXPECT_EQ(M->getInteger("diagnostic_count"), 0);
  EXPECT_NE(read("m00002.js").find("r('child_process');"), std::string::npos);
  BunProjectLimits Budget;
  const auto UsedUnits = M->getInteger("metadata_utf16_unit_count");
  ASSERT_TRUE(UsedUnits);
  ASSERT_GT(*UsedUnits, 0);
  Budget.UTF16Units = uint64_t(*UsedUnits) - 1;
  try {
    exportBunProject(E, (Root / "limited-project").string(), Budget);
    FAIL() << "External alias names must consume the shared metadata budget";
  } catch (const Error &Error) {
    EXPECT_STREQ(Error.what(), "bun_project_metadata_budget_exceeded");
  }
  EXPECT_FALSE(
      std::filesystem::exists(Root / "limited-project" / "manifest.json"));
}

TEST_F(WebBunProject, ProviderImportWithOnlyDynamicCallRetainsDiagnostic) {
  const std::string Consumer = "import {r} from '/$bunfs/root/SECRET.js';"
                               "r(name);";
  BunFixture F(true, "map", Consumer);
  source0(F, "var r=import.meta.require;export{r};");
  assetAsSource(F);
  const auto E = extraction(bunELF(F.Graph));
  exportBunProject(E, (Root / "project").string());
  auto Manifest = llvm::json::parse(read("manifest.json"));
  ASSERT_TRUE(bool(Manifest));
  const auto *M = Manifest->getAsObject();
  ASSERT_NE(M, nullptr);
  const auto *Modules = M->getArray("modules");
  ASSERT_NE(Modules, nullptr);
  ASSERT_EQ(Modules->size(), 3);
  const auto *Row = (*Modules)[2].getAsObject();
  ASSERT_NE(Row, nullptr);
  EXPECT_EQ(Row->getString("require_alias_status"), "ok");
  const auto *Problems = Row->getArray("require_alias_diagnostics");
  ASSERT_NE(Problems, nullptr);
  ASSERT_EQ(Problems->size(), 1);
  const auto *Problem = Problems->front().getAsObject();
  ASSERT_NE(Problem, nullptr);
  EXPECT_EQ(Problem->getString("code"), "bun_require_nonliteral_or_arity");
  EXPECT_EQ(Problem->getInteger("byte_offset"), Consumer.find("r(name)"));
  EXPECT_EQ(M->getInteger("external_module_reference_count"), 0);
  EXPECT_EQ(M->getInteger("unresolved_runtime_reference_count"), 0);
  EXPECT_EQ(M->getInteger("require_alias_relocation_count"), 0);
  EXPECT_EQ(M->getInteger("diagnostic_count"), 1);
  EXPECT_EQ(M->getInteger("diagnostic_count"), diagnosticTotal(*M));
  EXPECT_NE(read("m00002.js").find("r(name);"), std::string::npos);
}

TEST_F(WebBunProject, FailedRelocationHasSeparateReadableAvailability) {
  BunFixture F;
  source0(F, "const = ;");
  const auto E = extraction(bunELF(F.Graph));
  exportBunProject(E, (Root / "project").string());
  auto Manifest = llvm::json::parse(read("manifest.json"));
  ASSERT_TRUE(bool(Manifest));
  const auto *M = Manifest->getAsObject();
  ASSERT_NE(M, nullptr);
  EXPECT_EQ(M->getInteger("failed_source_count"), 1);
  EXPECT_EQ(M->getInteger("readable_source_count"), 1);
  EXPECT_EQ(M->getInteger("unavailable_readable_count"), 1);
  EXPECT_EQ(M->getInteger("diagnostic_count"), diagnosticTotal(*M));
  EXPECT_TRUE(std::filesystem::exists(Root / "project" / "m00000.original.js"));
  EXPECT_FALSE(std::filesystem::exists(Root / "project" / "m00000.js"));
}
} // namespace
