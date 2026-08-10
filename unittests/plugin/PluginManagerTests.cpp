#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include <string>

#ifndef NEVERD_NATIVE_PLUGIN_FIXTURE
#error "NEVERD_NATIVE_PLUGIN_FIXTURE is required"
#endif

namespace {

class PluginManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }

  void TearDown() override {
    if (Session)
      neverd_session_destroy(Session);
  }

  std::string takeString(const char *Value) {
    if (!Value)
      return {};
    std::string Result(Value);
    neverd_free_string(Value);
    return Result;
  }

  neverd_session_t Session = nullptr;
};

TEST_F(PluginManagerTest, LoadsRunsAndListsOnePluginFile) {
  ASSERT_EQ(neverd_plugins_load_file(Session, NEVERD_NATIVE_PLUGIN_FIXTURE), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(neverd_plugins_count(Session), 1);

  const std::string Listing = takeString(neverd_plugins_list_json(Session));
  EXPECT_NE(Listing.find("\"name\":\"Native Fixture\""), std::string::npos)
      << Listing;
  EXPECT_NE(Listing.find("\"version\":\"1.2.3\""), std::string::npos)
      << Listing;

  neverd_plugins_init(Session);
  EXPECT_EQ(neverd_plugins_run(Session, "Native Fixture", 5), 12);
  neverd_plugins_term(Session);
}

TEST_F(PluginManagerTest, RejectsDuplicateCanonicalPluginPath) {
  ASSERT_EQ(neverd_plugins_load_file(Session, NEVERD_NATIVE_PLUGIN_FIXTURE), 1);
  EXPECT_EQ(neverd_plugins_load_file(Session, NEVERD_NATIVE_PLUGIN_FIXTURE), 0);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("already loaded"),
            std::string::npos);
  EXPECT_EQ(neverd_plugins_count(Session), 1);
}

} // namespace
