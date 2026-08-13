#include "PluginManager.h"
#include "gtest/gtest.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef NEVERD_PYTHON_RUNTIME_FIXTURE
#error "NEVERD_PYTHON_RUNTIME_FIXTURE is required"
#endif

#ifndef NEVERD_PYTHON_RAISING_FIXTURE
#error "NEVERD_PYTHON_RAISING_FIXTURE is required"
#endif
#ifndef NEVERD_PYTHON_STALE_PROBE_FIXTURE
#error "NEVERD_PYTHON_STALE_PROBE_FIXTURE is required"
#endif
#ifndef NEVERD_PYTHON_RELOAD_FIXTURE
#error "NEVERD_PYTHON_RELOAD_FIXTURE is required"
#endif
#ifndef NEVERD_NATIVE_PLUGIN_FIXTURE
#error "NEVERD_NATIVE_PLUGIN_FIXTURE is required"
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

std::string readFile(const fs::path &Path) {
  std::ifstream Stream(Path);
  std::ostringstream Contents;
  Contents << Stream.rdbuf();
  return Contents.str();
}

const fs::path &processTempDirectory() {
  struct DirectoryOwner {
    DirectoryOwner() {
      const auto Stamp =
          std::chrono::steady_clock::now().time_since_epoch().count();
      const fs::path Root = fs::temp_directory_path();
      for (unsigned Attempt = 0; Attempt != 1000; ++Attempt) {
        const fs::path Candidate =
            Root / ("neverd-python-runtime-process-" + std::to_string(Stamp) +
                    "-" + std::to_string(Attempt));
        std::error_code EC;
        if (fs::create_directory(Candidate, EC)) {
          Path = Candidate;
          return;
        }
        if (EC)
          throw fs::filesystem_error("could not create test directory",
                                     Candidate, EC);
      }
      throw std::runtime_error("could not create a unique test directory");
    }
    ~DirectoryOwner() {
      std::error_code EC;
      fs::remove_all(Path, EC);
    }
    fs::path Path;
  };
  static DirectoryOwner Owner;
  return Owner.Path;
}

class PythonRuntimeTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    TempDir = processTempDirectory() / ("case-" + std::to_string(Stamp));
    ASSERT_TRUE(fs::create_directories(TempDir));
    TracePath = processTempDirectory() / "lifecycle.trace";
    std::error_code EC;
    fs::remove(TracePath, EC);
    setEnvironment("NEVERD_PYTHON_PLUGIN_TRACE", TracePath.string());
  }

  void TearDown() override {
    Manager.termAll();
    std::error_code EC;
    fs::remove_all(TempDir, EC);
    fs::remove(TracePath, EC);
  }

  static neverd_session_t session() {
    return reinterpret_cast<neverd_session_t>(static_cast<uintptr_t>(0x1234));
  }

  PluginManager Manager;
  fs::path TempDir;
  fs::path TracePath;
};

TEST_F(PythonRuntimeTest, DispatchesLifecycleAndInvalidatesBeforeTerm) {
  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_PYTHON_RUNTIME_FIXTURE))
      << Manager.lastError();
  ASSERT_EQ(Manager.plugins().size(), 1U);
  EXPECT_STREQ(Manager.plugins()[0].Runtime->kind(), "python");

  Manager.initAll(session());
  ASSERT_TRUE(Manager.lastError().empty()) << Manager.lastError();
  EXPECT_EQ(Manager.runPlugin("Python Runtime Fixture", session(), 4), 13)
      << Manager.lastError();

  neverd_event_t Event{};
  Event.Type = NEVERD_EVT_BINARY_LOADED;
  Event.Session = session();
  Event.Data.BinaryLoaded.Path = "runtime.bin";
  Manager.dispatchEvent(Event);
  ASSERT_TRUE(Manager.lastError().empty()) << Manager.lastError();
  Manager.termAll();

  EXPECT_EQ(readFile(TracePath), "init\n"
                                 "run:4\n"
                                 "event:BINARY_LOADED:runtime.bin\n"
                                 "stale\n"
                                 "term\n");

  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_PYTHON_STALE_PROBE_FIXTURE))
      << Manager.lastError();
  EXPECT_EQ(Manager.runPlugin("Python Stale Probe", session(), 0), 0)
      << Manager.lastError();
}

TEST_F(PythonRuntimeTest, CapturesFullTracebackAtTheNativeBoundary) {
  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_PYTHON_RAISING_FIXTURE))
      << Manager.lastError();
  EXPECT_EQ(Manager.runPlugin("Python Raising Fixture", session(), 7), -1);
  EXPECT_NE(Manager.lastError().find("Traceback (most recent call last)"),
            std::string::npos)
      << Manager.lastError();
  EXPECT_NE(Manager.lastError().find("RaisingPlugin.py"), std::string::npos)
      << Manager.lastError();
  EXPECT_NE(Manager.lastError().find("ValueError: intentional failure 7"),
            std::string::npos)
      << Manager.lastError();
}

TEST_F(PythonRuntimeTest, PropagatesTerminationTraceback) {
  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_PYTHON_RAISING_FIXTURE))
      << Manager.lastError();
  Manager.termAll();
  EXPECT_NE(Manager.lastError().find("termination failed"), std::string::npos)
      << Manager.lastError();
  EXPECT_NE(Manager.lastError().find("Traceback (most recent call last)"),
            std::string::npos)
      << Manager.lastError();
  EXPECT_NE(Manager.lastError().find("intentional termination failure"),
            std::string::npos)
      << Manager.lastError();
}

TEST_F(PythonRuntimeTest, AcceptsCallbackFromAWorkerThread) {
  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_PYTHON_RUNTIME_FIXTURE))
      << Manager.lastError();
  std::thread Worker([&] { Manager.initAll(session()); });
  Worker.join();
  EXPECT_TRUE(Manager.lastError().empty()) << Manager.lastError();
  Manager.termAll();
  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_PYTHON_STALE_PROBE_FIXTURE))
      << Manager.lastError();
  EXPECT_EQ(Manager.runPlugin("Python Stale Probe", session(), 0), 0)
      << Manager.lastError();
}

TEST_F(PythonRuntimeTest, ReloadStartsWithFreshModuleAndInstanceState) {
  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_PYTHON_RELOAD_FIXTURE))
      << Manager.lastError();
  EXPECT_EQ(Manager.runPlugin("Python Reload State", session(), 0), 1);
  EXPECT_EQ(Manager.runPlugin("Python Reload State", session(), 0), 2);
  Manager.termAll();

  ASSERT_TRUE(Manager.loadPluginFile(NEVERD_PYTHON_RELOAD_FIXTURE))
      << Manager.lastError();
  EXPECT_EQ(Manager.runPlugin("Python Reload State", session(), 0), 1)
      << Manager.lastError();
}

TEST_F(PythonRuntimeTest, DiscoversNativeAndPythonPluginsInSortedOrder) {
  const fs::path PythonCopy = TempDir / "a_python.py";
  const fs::path NativeCopy =
      TempDir / (std::string("b_native") +
                 fs::path(NEVERD_NATIVE_PLUGIN_FIXTURE).extension().string());
  ASSERT_TRUE(fs::copy_file(NEVERD_PYTHON_RELOAD_FIXTURE, PythonCopy));
  ASSERT_TRUE(fs::copy_file(NEVERD_NATIVE_PLUGIN_FIXTURE, NativeCopy));

  EXPECT_EQ(Manager.loadPluginsFromDir(TempDir.string()), 2)
      << Manager.lastError();
  ASSERT_EQ(Manager.plugins().size(), 2U);
  EXPECT_STREQ(Manager.plugins()[0].descriptor().Name, "Python Reload State");
  EXPECT_STREQ(Manager.plugins()[1].descriptor().Name, "Native Fixture");
}

} // namespace
