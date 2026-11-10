//===- X64PagedReturnContinuationTests.inc - Retained return accesses ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included after the paged source fixture. Backing repair must continue the
/// original return even when descriptor writes have changed its frame inputs.
//===----------------------------------------------------------------------===//

TEST_F(X64PagedExecutionTest, PagedReturnResumeRetainsAliasedSelector) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0xcf};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
    reset(Code);
    auto &Core = State.Core.Core.Core.Core;
    Core.GPR[4] = 0x17fe0;
    State.NMIBlocked = 1;
    State.GDTR = {0xd00b, 0xffff};
    put(0x17fe0, 0x1800);
    // CS == 0xafda selects this DPL2 code descriptor. Its accessed byte is
    // also the low byte of saved CS. The write changes RPL2 to RPL3 in RAM.
    put(0x17fe3, 0x00afda000000ffffULL);
    put(0x17ff0, 2);
    put(0x17ff8, 0xe800);
    put(0x18000, 0x32);
    put(0xd03b, 0x00cfd3000000ffffULL);
    put(0x130c0, 0x50007); // Outer SS has no physical backing yet.
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecutionOptions Options;
    Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
    X64PagedExecution Context(*Memory, Options);
    uint64_t Budget = 2;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              18u);
    EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
    EXPECT_EQ(Budget, 1u);
    EXPECT_EQ(State.NMIBlocked, 0u);
    EXPECT_EQ(Memory->physicalMemory()
                  .loadScalar(0x17fe8, GuestScalarWidth::I16)
                  .Value,
              0xafdbu);
    ASSERT_EQ(Memory->physicalMemory()
                  .storeScalar(0x130c0, GuestScalarWidth::I64, 0x18007)
                  .Status,
              GuestMemoryAccessStatus::Completed);
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              6u);
    EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
    EXPECT_EQ(Budget, 1u);
    EXPECT_EQ(Core.PC, 0x000000ffff001800ULL);
    EXPECT_EQ(Core.GPR[4], 0xe800u);
    EXPECT_EQ(State.Core.CS.Selector, 0xafdau);
    EXPECT_TRUE(Context.lastResult().InstructionCompleted);
    EXPECT_FALSE(Context.lastResult().InstructionAttempted);
  }
}
