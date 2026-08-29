#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sdk/NeverDPlugin.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef NEVERD_PYTHON_LIFECYCLE_FIXTURE
#error "NEVERD_PYTHON_LIFECYCLE_FIXTURE is required"
#endif
#ifndef NEVERD_PYTHON_RAISING_FIXTURE
#error "NEVERD_PYTHON_RAISING_FIXTURE is required"
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

class PythonPluginTest : public ::testing::Test {
protected:
  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    TracePath = fs::temp_directory_path() /
                ("neverd-python-plugin-" + std::to_string(Stamp) + ".trace");
    std::error_code EC;
    fs::remove(TracePath, EC);
    setEnvironment("NEVERD_PYTHON_PLUGIN_TRACE", TracePath.string());
  }

  void TearDown() override {
    if (Session)
      neverd_session_destroy(Session);
    std::error_code EC;
    fs::remove(TracePath, EC);
  }

  std::string takeString(const char *Value) {
    if (!Value)
      return {};
    std::string Result(Value);
    neverd_free_string(Value);
    return Result;
  }

  neverd_session_t Session = nullptr;
  fs::path TracePath;
};

TEST_F(PythonPluginTest, DispatchesCompleteLifecycleThroughPublicCAPI) {
  ASSERT_EQ(neverd_plugins_load_file(Session, NEVERD_PYTHON_LIFECYCLE_FIXTURE),
            1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_plugins_count(Session), 1);
  const std::string Listing = takeString(neverd_plugins_list_json(Session));
  EXPECT_NE(Listing.find("\"name\":\"Python Lifecycle Fixture\""),
            std::string::npos)
      << Listing;
  EXPECT_NE(Listing.find("\"kind\":\"python\""), std::string::npos) << Listing;

  neverd_plugins_init(Session);
  ASSERT_TRUE(takeString(neverd_last_error(Session)).empty());
  EXPECT_EQ(neverd_plugins_run(Session, "Python Lifecycle Fixture", 5), 8)
      << takeString(neverd_last_error(Session));

  neverd_event_t Event{};
  Event.Type = NEVERD_EVT_BINARY_LOADED;
  Event.Session = Session;
  Event.Data.BinaryLoaded.Path = "input.bin";
  neverd_plugins_dispatch_event(Session, &Event);
  ASSERT_TRUE(takeString(neverd_last_error(Session)).empty());
  neverd_plugins_term(Session);

  EXPECT_EQ(readFile(TracePath), "init:NeverD:3389.0.1\n"
                                 "run:5\n"
                                 "event:BINARY_LOADED:input.bin\n"
                                 "term\n");
}

TEST_F(PythonPluginTest, ReportsTerminationTracebackThroughPublicCAPI) {
  ASSERT_EQ(neverd_plugins_load_file(Session, NEVERD_PYTHON_RAISING_FIXTURE), 1)
      << takeString(neverd_last_error(Session));
  neverd_plugins_init(Session);
  ASSERT_TRUE(takeString(neverd_last_error(Session)).empty());
  neverd_plugins_term(Session);
  const std::string Error = takeString(neverd_last_error(Session));
  EXPECT_NE(Error.find("termination failed"), std::string::npos) << Error;
  EXPECT_NE(Error.find("Traceback (most recent call last)"), std::string::npos)
      << Error;
  EXPECT_NE(Error.find("intentional termination failure"), std::string::npos)
      << Error;
}

} // namespace
