//===- X86LegacyStatusTests.inc - Protected implicit scalar state --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Implicit sign extensions and exact AH/carry/direction flag ownership.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, AccumulatorSignExtensionRetainsItsOtherBits) {
  for (uint32_t Value : {0u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u, 0xffffu}) {
    S.Core.GPR[0] = 0xabcd0000 | Value;
    S.Core.GPR[2] = 0x87654321;
    S.EFlags = 0x10ed7;
    ASSERT_TRUE(run({0x66, 0x98}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0xabcd0000u | uint16_t(int8_t(Value)));
    EXPECT_EQ(S.Core.GPR[2], 0x87654321u);
    EXPECT_EQ(S.EFlags, 0xed7u);
    S.Core.GPR[0] = 0xabcd0000 | Value;
    ASSERT_TRUE(run({0x98}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], uint32_t(int16_t(Value)));
    EXPECT_EQ(S.Core.GPR[2], 0x87654321u);
    EXPECT_EQ(S.EFlags, 0xed7u);
  }
}

TEST_F(X86LegacyExecutionTest, DividendSignExtensionWritesOnlyTheHighHalf) {
  for (uint32_t Value :
       {0u, 0x7fffu, 0x8000u, 0xffffu, 0x7fffffffu, 0x80000000u, UINT32_MAX}) {
    S.Core.GPR[0] = Value;
    S.Core.GPR[2] = 0x12345678;
    S.EFlags = 0x10ed7;
    ASSERT_TRUE(run({0x66, 0x99}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], Value);
    EXPECT_EQ(S.Core.GPR[2], 0x12340000u | (Value & 0x8000 ? 0xffffu : 0));
    EXPECT_EQ(S.EFlags, 0xed7u);
    ASSERT_TRUE(run({0x99}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], Value);
    EXPECT_EQ(S.Core.GPR[2], Value & 0x80000000 ? UINT32_MAX : 0u);
    EXPECT_EQ(S.EFlags, 0xed7u);
  }
}

TEST_F(X86LegacyExecutionTest, LoadStatusFlagsPublishesOnlyCanonicalAH) {
  for (unsigned Status = 0; Status != 256; ++Status) {
    S.Core.GPR[0] = 0x1234ff5a;
    S.EFlags = 0x10e02 | (Status & 0xd5);
    ASSERT_TRUE(run({0x9f}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0x1234005au | ((Status & 0xd5) | 2) << 8);
    EXPECT_EQ(S.EFlags, 0xe02u | (Status & 0xd5));
  }
}

TEST_F(X86LegacyExecutionTest, StoreAHFlagsKeepsOverflowDirectionAndPrivilege) {
  for (unsigned AH = 0; AH != 256; ++AH) {
    S.Core.GPR[0] = 0xabcd005a | (AH << 8);
    S.EFlags = 0x13ed7;
    ASSERT_TRUE(run({0x9e}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0xabcd005au | (AH << 8));
    EXPECT_EQ(S.EFlags, 0x3e02u | (AH & 0xd5));
  }
}

TEST_F(X86LegacyExecutionTest, CarryAndDirectionControlsTouchOnlyTheirFlag) {
  for (uint32_t Flags :
       {0x202u, 0x203u, 0x602u, 0x603u, 0xad6u, 0xad7u, 0xed6u, 0xed7u}) {
    for (uint8_t Opcode : {0xf5, 0xf8, 0xf9, 0xfc, 0xfd}) {
      S.EFlags = Flags | 0x10000;
      S.Core.GPR[0] = 0x12345678;
      const uint32_t Mask = Opcode >= 0xfc ? 0x400 : 1;
      const uint32_t Expected = Opcode == 0xf5 ? Flags ^ Mask
                                : Opcode & 1   ? Flags | Mask
                                               : Flags & ~Mask;
      ASSERT_TRUE(run({0x66, 0x67, 0x64, Opcode}).InstructionCompleted);
      EXPECT_EQ(S.EFlags, Expected);
      EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
    }
  }
}

TEST_F(X86LegacyExecutionTest, ImplicitScalarsRequireAnExplicitBinding) {
  O.Scalars.reset();
  for (uint8_t Opcode :
       {0x98, 0x99, 0x9e, 0x9f, 0xf5, 0xf8, 0xf9, 0xfc, 0xfd}) {
    auto R = executeX86LegacyInstruction(S, *M, {Opcode}, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.EFlags, 0x10202u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}

TEST_F(X86LegacyExecutionTest, ImplicitScalarsAuthenticateBeforePublication) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0xf0, 0x98}, {0xf0, 0x9e}, {0xf3, 0x9f}, {0xf8, 0}, {0x66}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
  }
  S.EIP = 0x1fff;
  S.Core.GPR[0] = UINT32_MAX;
  S.Core.GPR[2] = 0x12345678;
  S.EFlags = 0x10ed7;
  put(0x11008, 0x2006, GuestScalarWidth::I32);
  const auto R = run({0x66, 0x99});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.Core.GPR[0], UINT32_MAX);
  EXPECT_EQ(S.Core.GPR[2], 0x12345678u);
  EXPECT_EQ(S.CR2, 0x2000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1fffu);
  EXPECT_EQ(get(S.Core.GPR[4] + 12), 0x10ed7u);
}
