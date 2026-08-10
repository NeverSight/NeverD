#include "PluginManager.h"
#include "gtest/gtest.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef NEVERD_NATIVE_PLUGIN_FIXTURE
#error "NEVERD_NATIVE_PLUGIN_FIXTURE is required"
#endif
#ifndef NEVERD_NO_EXPORT_PLUGIN_FIXTURE
#error "NEVERD_NO_EXPORT_PLUGIN_FIXTURE is required"
#endif
#ifndef NEVERD_NULL_NAME_PLUGIN_FIXTURE
#error "NEVERD_NULL_NAME_PLUGIN_FIXTURE is required"
#endif
#ifndef NEVERD_PYTHON_FILE_FIXTURE
#error "NEVERD_PYTHON_FILE_FIXTURE is required"
#endif
#ifndef NEVERD_TEST_PYTHON_PLUGINS_ENABLED
#error "NEVERD_TEST_PYTHON_PLUGINS_ENABLED is required"
#endif

namespace fs = std::filesystem;

namespace {

class PluginRuntimeTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    TempDir = fs::temp_directory_path() /
              ("neverd-plugin-manager-" + std::to_string(Stamp));
    ASSERT_TRUE(fs::create_directories(TempDir));
  }

  void TearDown() override {
    Manager.termAll();
    std::error_code EC;
    fs::remove_all(TempDir, EC);
  }

  static neverd_session_t session() {
    return reinterpret_cast<neverd_session_t>(static_cast<uintptr_t>(0x1234));
  }

  PluginManager Manager;
  fs::path TempDir;
};

TEST_F(PluginRuntimeTest, LoadsInitializesRunsAndRejectsTheSamePath) {
  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_NATIVE_PLUGIN_FIXTURE))
      << Manager.lastError();
  ASSERT_EQ(Manager.plugins().size(), 1U);
  EXPECT_STREQ(Manager.plugins()[0].descriptor().Name, "Native Fixture");
  EXPECT_STREQ(Manager.plugins()[0].Runtime->kind(), "native");
  Manager.initAll(session());
  EXPECT_TRUE(Manager.lastError().empty()) << Manager.lastError();
  EXPECT_EQ(Manager.runPlugin("Native Fixture", session(), 5), 12);

  EXPECT_FALSE(Manager.loadPluginFile(NEVERD_NATIVE_PLUGIN_FIXTURE));
  EXPECT_NE(Manager.lastError().find("already loaded"), std::string::npos)
      << Manager.lastError();
}

TEST_F(PluginRuntimeTest, RejectsMissingWrongAndMalformedPluginFiles) {
  EXPECT_FALSE(Manager.loadPluginFile((TempDir / "missing.so").string()));
  EXPECT_NE(Manager.lastError().find("not found"), std::string::npos);

  const fs::path TextFile = TempDir / "plugin.txt";
  std::ofstream(TextFile) << "not a plugin";
  EXPECT_FALSE(Manager.loadPluginFile(TextFile.string()));
  EXPECT_NE(Manager.lastError().find("unsupported plugin file extension"),
            std::string::npos)
      << Manager.lastError();

  EXPECT_FALSE(Manager.loadPluginFile(NEVERD_NO_EXPORT_PLUGIN_FIXTURE));
  EXPECT_NE(Manager.lastError().find("does not export 'neverd_plugin'"),
            std::string::npos)
      << Manager.lastError();

  EXPECT_FALSE(Manager.loadPluginFile(NEVERD_NULL_NAME_PLUGIN_FIXTURE));
  EXPECT_NE(Manager.lastError().find("no non-empty name"), std::string::npos)
      << Manager.lastError();
}

TEST_F(PluginRuntimeTest, RejectsDuplicateNameFromADifferentCanonicalPath) {
  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_NATIVE_PLUGIN_FIXTURE));
  const fs::path Copy =
      TempDir / fs::path(NEVERD_NATIVE_PLUGIN_FIXTURE).filename();
  ASSERT_TRUE(fs::copy_file(NEVERD_NATIVE_PLUGIN_FIXTURE, Copy));
  EXPECT_FALSE(Manager.loadPluginFile(Copy.string()));
  EXPECT_NE(Manager.lastError().find("name is already loaded"),
            std::string::npos)
      << Manager.lastError();
  EXPECT_EQ(Manager.plugins().size(), 1U);
}

#if !NEVERD_TEST_PYTHON_PLUGINS_ENABLED
TEST_F(PluginRuntimeTest, ReportsPythonFeatureIsDisabled) {
  EXPECT_FALSE(Manager.loadPluginFile(NEVERD_PYTHON_FILE_FIXTURE));
  EXPECT_NE(Manager.lastError().find("NEVERD_ENABLE_PYTHON_PLUGINS=ON"),
            std::string::npos)
      << Manager.lastError();
}
#endif

} // namespace
