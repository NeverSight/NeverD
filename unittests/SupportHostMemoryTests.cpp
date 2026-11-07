//===- SupportHostMemoryTests.cpp - The host's physical memory ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The host says how much physical memory it has, which no machine that runs
/// these tests lacks a gibibyte of.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/HostMemory.h"

namespace {
TEST(HostMemory, TheHostSaysHowMuchPhysicalMemoryItHas) {
  const auto Bytes = neverd::hostPhysicalMemoryBytes();
  ASSERT_TRUE(Bytes.has_value());
  EXPECT_GE(*Bytes, uint64_t{1} << 30);
}
} // namespace
