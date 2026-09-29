//===- HardwareBackendPublicTests.cpp - Public selection tests-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/DriverReportFields.h"
#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/sdk/NeverDCAPIEmulation.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/JSON.h"

#include <filesystem>

namespace {
#define NEVERD_HARDWARE_DRIVER_CASE(Name, Text) constexpr char Name[] = Text;
#include "HardwareBackendCases.def"
#undef NEVERD_HARDWARE_DRIVER_CASE
TEST(HardwareBackendPublic, ValidatesSelectionAndPreservesV1Contract) {
  using namespace neverd::emulation;
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Destroy = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  const auto Path =
      (std::filesystem::path(NEVERD_DRIVER_FIXTURES) / Success).string();
  EXPECT_EQ(neverd_emulate_driver_backend_json(Session, Path.c_str(), nullptr,
                                               nullptr, nullptr,
                                               execution::Legacy),
            nullptr);
  EXPECT_EQ(neverd_emulate_driver_backend_json(Session, Path.c_str(), nullptr,
                                               nullptr, execution::KVM,
                                               execution::Legacy),
            nullptr);
  const char *Text = neverd_emulate_driver_backend_json(
      Session, Path.c_str(), nullptr, nullptr, execution::Auto,
      execution::Legacy);
  ASSERT_NE(Text, nullptr);
  auto Report = llvm::json::parse(Text);
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  auto *Root = Report->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getString(execution::RequestedBackend), execution::Auto);
  EXPECT_EQ(Root->getString(execution::SelectedBackend), execution::Unicorn);
  EXPECT_EQ(Root->getString(execution::ExecutionContract), execution::Legacy);
  EXPECT_EQ(Root->getString(field::StopReason), stop::Returned);
  Text = neverd_emulate_driver_backend_json(Session, Path.c_str(), nullptr,
                                            nullptr, execution::Unicorn,
                                            execution::Legacy);
  ASSERT_NE(Text, nullptr);
  Report = llvm::json::parse(Text);
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  EXPECT_EQ(Report->getAsObject()->getString(execution::SelectedBackend),
            execution::Unicorn);
  Text = neverd_emulate_driver_json(Session, Path.c_str(), nullptr);
  ASSERT_NE(Text, nullptr);
  Report = llvm::json::parse(Text);
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  EXPECT_FALSE(Report->getAsObject()->get(execution::SelectedBackend));
}
} // namespace
