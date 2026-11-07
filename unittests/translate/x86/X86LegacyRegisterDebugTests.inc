//===- X86LegacyRegisterDebugTests.inc - Debug bank transactions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Software coverage for unbound writes and original GD frame policies.
//===----------------------------------------------------------------------===//

class X86LegacyDebugRegisterTest : public X86LegacyDebugTest {
protected:
  void SetUp() override {
    X86LegacyDebugTest::SetUp();
    S.Core.CPL = 0;
    S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
    S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
    S.EFlags = 0xed7;
    O.Debug = X86LegacyDebugProfile::DebugRegisters;
    O.GeneralDetectFrame = X86GeneralDetectFrame::FaultResume;
  }
};

TEST_F(X86LegacyDebugRegisterTest, FullDwordTransferIgnoresOperandAttribute) {
  for (bool Code32 : {false, true})
    for (bool Prefix : {false, true})
      for (unsigned General = 0; General != 8; ++General)
        for (unsigned Debug = 0; Debug != 8; ++Debug) {
          SetUp();
          if (!Code32)
            S.Core.Segments[1].Attributes &= ~0x4000;
          S.Debug->DR = {0xabcdef12, 0x87654321, 0xffffffff, 0x80000000};
          S.Core.GPR[General] = 0x12345678;
          std::vector<uint8_t> Bytes{0x0f, 0x21,
                                     uint8_t((Debug << 3) | General)};
          if (Prefix)
            Bytes.insert(Bytes.begin(), 0x66);
          const auto R = run(Bytes);
          ASSERT_TRUE(R.InstructionCompleted);
          const uint32_t Want = Debug < 4                  ? S.Debug->DR[Debug]
                                : Debug == 4 || Debug == 6 ? S.Debug->DR6
                                                           : S.Debug->DR7;
          EXPECT_EQ(S.Core.GPR[General], Want);
          EXPECT_EQ(S.EFlags, 0xed7u);
        }
}

TEST_F(X86LegacyDebugRegisterTest, UnboundWriteDoesNotInventGuestFault) {
  const std::vector<uint8_t> Bytes{0x0f, 0x23, 0xf8};
  for (uint32_t Value : {0x10001, 0x800}) {
    SetUp();
    S.Core.GPR[0] = Value;
    for (unsigned I = 0; I != Bytes.size(); ++I)
      put(S.EIP + I, Bytes[I], GuestScalarWidth::I8);
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.Debug->DR7, 0x400u);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.EFlags, 0xed7u);
    EXPECT_FALSE(S.PendingEvent);
  }
}

TEST_F(X86LegacyDebugRegisterTest, GeneralDetectFrameSurvivesBackingStop) {
  for (auto Frame : {X86GeneralDetectFrame::FaultResume,
                     X86GeneralDetectFrame::ClearResume}) {
    SetUp();
    O.GeneralDetectFrame = Frame;
    S.Debug->DR7 = 0x2400;
    S.SSBlocked = true;
    S.EFlags |= 0x10000;
    put(0x11018, 0x20007, GuestScalarWidth::I32);
    const auto R = run({0x0f, 0x21, 0xc0});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    ASSERT_TRUE(S.PendingEvent);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(S.Debug->DR6, 0xffff2ff0u);
    EXPECT_EQ(S.Debug->DR7, 0x400u);
    EXPECT_EQ(S.Core.GPR[0], 35u);
    EXPECT_FALSE(S.SSBlocked);
    put(0x11018, 0x6007, GuestScalarWidth::I32);
    const auto Continued = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
    ASSERT_EQ(Continued.DeliveredVector, 1);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
    EXPECT_EQ(get(S.Core.GPR[4] + 8),
              Frame == X86GeneralDetectFrame::FaultResume ? 0x10ed7u : 0xed7u);
    EXPECT_EQ(S.Debug->DR6, 0xffff2ff0u);
  }
}

TEST_F(X86LegacyDebugRegisterTest, AcceptedBreakpointClearsGDOnce) {
  S.Debug->DR7 = 0x2401;
  S.Debug->DR[0] = S.EIP;
  put(0x11018, 0x20007, GuestScalarWidth::I32);
  const auto R = run({0x90});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_EQ(S.Debug->DR7, 0x401u);
  EXPECT_EQ(S.Debug->DR6, 0xffff0ff1u);
  EXPECT_FALSE(R.InstructionCompleted);
  put(0x11018, 0x6007, GuestScalarWidth::I32);
  const auto Continued = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  EXPECT_EQ(Continued.DeliveredVector, 1);
  EXPECT_EQ(S.Debug->DR7, 0x401u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
}
