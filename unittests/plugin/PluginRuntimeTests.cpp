#include "../TestProcess.h"
#include "PluginManager.h"
#include "gtest/gtest.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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
#ifndef NEVERD_LIFECYCLE_PROBE_FIXTURE
#error "NEVERD_LIFECYCLE_PROBE_FIXTURE is required"
#endif
#ifndef NEVERD_PYTHON_FILE_FIXTURE
#error "NEVERD_PYTHON_FILE_FIXTURE is required"
#endif
#ifndef NEVERD_TEST_PYTHON_PLUGINS_ENABLED
#error "NEVERD_TEST_PYTHON_PLUGINS_ENABLED is required"
#endif

namespace fs = std::filesystem;

namespace {

void setEnvironment(const char *Name, const std::string &Value) {
#ifdef _WIN32
  ASSERT_EQ(_putenv_s(Name, Value.c_str()), 0);
#else
  ASSERT_EQ(setenv(Name, Value.c_str(), 1), 0);
#endif
}

void unsetEnvironment(const char *Name) {
#ifdef _WIN32
  ASSERT_EQ(_putenv_s(Name, ""), 0);
#else
  ASSERT_EQ(unsetenv(Name), 0);
#endif
}

std::string readFile(const fs::path &Path) {
  std::ifstream Stream(Path);
  std::ostringstream Contents;
  Contents << Stream.rdbuf();
  return Contents.str();
}

bool isLibraryLoaded(const fs::path &Path) {
#ifdef _WIN32
  return GetModuleHandleW(Path.filename().c_str()) != nullptr;
#else
  void *Handle = dlopen(Path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (!Handle)
    return false;
  dlclose(Handle);
  return true;
#endif
}

class PluginRuntimeTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path Root = fs::temp_directory_path();
    const std::string Prefix =
        "neverd-plugin-manager-" +
        std::to_string(neverd::test::currentProcessId()) + "-" +
        std::to_string(Stamp) + "-";
    // CTest may launch each discovered GoogleTest in a separate process.
    // Claim the directory atomically so one fixture never cleans up another's.
    for (unsigned Attempt = 0; Attempt != 1000; ++Attempt) {
      const fs::path Candidate = Root / (Prefix + std::to_string(Attempt));
      std::error_code EC;
      if (fs::create_directory(Candidate, EC)) {
        TempDir = Candidate;
        break;
      }
      if (EC)
        FAIL() << "could not create test directory " << Candidate << ": "
               << EC.message();
    }
    ASSERT_FALSE(TempDir.empty()) << "could not create a unique test directory";
    TracePath = TempDir / "lifecycle.trace";
    setEnvironment("NEVERD_LIFECYCLE_PROBE_TRACE", TracePath.string());
    unsetEnvironment("NEVERD_LIFECYCLE_PROBE_FAIL_INIT");
  }

  void TearDown() override {
    Manager.termAll();
    unsetEnvironment("NEVERD_LIFECYCLE_PROBE_FAIL_INIT");
    unsetEnvironment("NEVERD_LIFECYCLE_PROBE_TRACE");
    std::error_code EC;
    fs::remove_all(TempDir, EC);
  }

  static neverd_session_t session() {
    return reinterpret_cast<neverd_session_t>(static_cast<uintptr_t>(0x1234));
  }

  PluginManager Manager;
  fs::path TempDir;
  fs::path TracePath;
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

TEST_F(PluginRuntimeTest, DoesNotTerminateAPluginThatWasNeverInitialized) {
  const fs::path PluginPath = NEVERD_LIFECYCLE_PROBE_FIXTURE;
  ASSERT_TRUE(Manager.loadPluginFile(PluginPath.string()))
      << Manager.lastError();
  ASSERT_TRUE(isLibraryLoaded(PluginPath));

  Manager.termAll();

  EXPECT_EQ(readFile(TracePath), "");
  EXPECT_FALSE(isLibraryLoaded(PluginPath));
}

TEST_F(PluginRuntimeTest, DoesNotTerminateAPluginWhoseInitializationFailed) {
  const fs::path PluginPath = NEVERD_LIFECYCLE_PROBE_FIXTURE;
  setEnvironment("NEVERD_LIFECYCLE_PROBE_FAIL_INIT", "1");
  ASSERT_TRUE(Manager.loadPluginFile(PluginPath.string()))
      << Manager.lastError();
  ASSERT_TRUE(isLibraryLoaded(PluginPath));

  Manager.initAll(session());
  ASSERT_NE(Manager.lastError().find("initialization failed"),
            std::string::npos)
      << Manager.lastError();
  Manager.termAll();

  EXPECT_EQ(readFile(TracePath), "init:failed\n");
  EXPECT_FALSE(isLibraryLoaded(PluginPath));
}

TEST_F(PluginRuntimeTest,
       TerminatesOnceAfterSuccessfulInitializationAndUnloads) {
  const fs::path PluginPath = NEVERD_LIFECYCLE_PROBE_FIXTURE;
  ASSERT_TRUE(Manager.loadPluginFile(PluginPath.string()))
      << Manager.lastError();
  ASSERT_TRUE(isLibraryLoaded(PluginPath));

  Manager.initAll(session());
  ASSERT_TRUE(Manager.lastError().empty()) << Manager.lastError();
  Manager.termAll();
  Manager.termAll();

  EXPECT_EQ(readFile(TracePath), "init:success\nterm\n");
  EXPECT_FALSE(isLibraryLoaded(PluginPath));
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
