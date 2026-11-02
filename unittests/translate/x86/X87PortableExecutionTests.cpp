//===- X87PortableExecutionTests.cpp - Explicit portable runtime calls ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// C-call ABI, explicit incoming state and single-consumption scalar results.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/X86/X87PortableExecution.h"

#include <cstdint>

using namespace neverd;
using namespace neverd::translate;

extern "C" uint32_t nvd_x87_c_status_probe(void *, uint64_t *);

namespace {

TEST(X87PortableExecution, PlainCBridgeRequiresExplicitIncomingState) {
  auto Guest =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  auto Memory = llvm::cantFail(GuestMemoryRuntime::create(Guest));
  PortableX87StateV1 State;
  PortableX87ExecutionV1 Execution(State, *Memory);
  uint64_t SourceReturn = 0xfeed;
  EXPECT_EQ(nvd_x87_c_status_probe(&Execution, &SourceReturn), 2u);
  EXPECT_EQ(SourceReturn, 0xfeedu);
  EXPECT_FALSE(Execution.lastStep().InstructionCompleted);
  EXPECT_EQ(Execution.lastInstructionAddress(), 0x4000u);

  State.EnvironmentKnown = 1;
  State.Image[0] = 0x7f;
  State.Image[1] = 3;
  State.Image[3] = 5u << 3;
  EXPECT_EQ(nvd_x87_c_status_probe(&Execution, &SourceReturn), 0u);
  EXPECT_EQ(SourceReturn, 5u << 11);
  EXPECT_TRUE(Execution.lastStep().InstructionCompleted);
  EXPECT_TRUE(Execution.lastStep().HasScalarResult);
  EXPECT_EQ(State.KnownPayload, 0);

  SourceReturn = 0xfeed;
  EXPECT_EQ(nvd_x87_c_status_probe(nullptr, &SourceReturn), 1u);
  EXPECT_EQ(SourceReturn, 0xfeedu);
}

#if GTEST_HAS_DEATH_TEST
TEST(X87PortableExecution, ResultMustBelongToTheLatestUnconsumedProducer) {
  auto Guest =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  auto Memory = llvm::cantFail(GuestMemoryRuntime::create(Guest));
  PortableX87StateV1 State;
  State.EnvironmentKnown = 1;
  State.Image[0] = 0x7f;
  State.Image[1] = 3;
  PortableX87ExecutionV1 Execution(State, *Memory);
  EXPECT_DEATH((void)nvd_x87_v1_result(&Execution), "unconsumed successful");
  ASSERT_EQ(nvd_x87_v1_step(&Execution, 0xe0df, 0x4000, 0), 0u);
  EXPECT_EQ(nvd_x87_v1_result(&Execution), 0u);
  EXPECT_DEATH((void)nvd_x87_v1_result(&Execution), "unconsumed successful");
  ASSERT_EQ(nvd_x87_v1_step(&Execution, 0xe0df, 0x4000, 0), 0u);
  ASSERT_EQ(nvd_x87_v1_step(&Execution, 0xe3db, 0x4002, 0), 0u);
  EXPECT_DEATH((void)nvd_x87_v1_result(&Execution), "unconsumed successful");
  ASSERT_EQ(nvd_x87_v1_step(&Execution, 0xe0df, 0x4000, 0), 0u);
  EXPECT_NE(nvd_x87_v1_step(&Execution, 0xfad9, 0x4002, 0), 0u);
  EXPECT_DEATH((void)nvd_x87_v1_result(&Execution), "unconsumed successful");
}
#endif

} // namespace
