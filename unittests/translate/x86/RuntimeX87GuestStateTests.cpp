//===- RuntimeX87GuestStateTests.cpp - Additive x87 state contract --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/X86/RuntimeX87GuestState.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <utility>

using namespace neverd::translate;

namespace {

constexpr RegisterID X87ID = kFirstExtensionRegisterID + 17;

void expectError(llvm::Error Error) {
  EXPECT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
}

TEST(RuntimeX87GuestState, ExplicitExtensionRetainsEveryByteAndProvenance) {
  auto Logical =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  const auto Baseline = llvm::cantFail(serializeGuestState(Logical));
  auto Missing = createRuntimeGuestStateX86_64X87V1(Logical);
  ASSERT_FALSE(static_cast<bool>(Missing));
  llvm::consumeError(Missing.takeError());
  EXPECT_EQ(Baseline, llvm::cantFail(serializeGuestState(Logical)));
  llvm::cantFail(setRegisterValue(Logical, X87ID + 1, llvm::APInt(19, 0x12345),
                                  "caller.other-state"));
  Logical.Memory.push_back({0x9000, MemoryPermission::Write, 3, {0x78, 0x56}});
  for (unsigned Top = 0; Top != 8; ++Top) {
    PortableX87StateV1 X87;
    X87.EnvironmentKnown = Top & 1;
    X87.KnownPayload = static_cast<uint8_t>(0xa5u ^ Top);
    for (unsigned Index = 0; Index != X87.Image.size(); ++Index)
      X87.Image[Index] = static_cast<uint8_t>(Index * 17 + Top);
    llvm::support::endian::write16le(X87.Image.data() + 2,
                                     static_cast<uint16_t>(Top << 11));
    llvm::cantFail(setPortableX87GuestStateV1(Logical, X87ID, X87));
    const auto *Encoded = findRegisterValue(Logical, X87ID);
    ASSERT_NE(Encoded, nullptr);
    EXPECT_EQ(Encoded->Value.extractBitsAsZExtValue(32, 0),
              kPortableX87StateMagicV1);
    EXPECT_EQ(Encoded->Value.extractBitsAsZExtValue(16, 32),
              kPortableX87StateVersionV1);
    EXPECT_EQ(Encoded->Value.extractBitsAsZExtValue(16, 48),
              kPortableX87StateSizeV1);
    auto Wire = llvm::cantFail(serializeGuestState(Logical));
    auto Restored = llvm::cantFail(deserializeGuestState(Wire));
    auto Runtime = llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Restored));
    EXPECT_EQ(Runtime.X87.Image, X87.Image);
    EXPECT_EQ(Runtime.X87.KnownPayload, X87.KnownPayload);
    EXPECT_EQ(Runtime.X87.EnvironmentKnown, X87.EnvironmentKnown);
    EXPECT_EQ(Runtime.LastStep.Attempted, 0);
    Runtime.Integer.GPR[0] = 0x1122334455667788;
    Runtime.Integer.RIP = 0x1234;
    Runtime.X87.Image[511] ^= 0x80;
    Runtime.X87.KnownPayload ^= 0xff;
    llvm::cantFail(applyRuntimeGuestStateX86_64X87V1(Runtime, Restored));
    const auto RoundTrip =
        llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Restored));
    EXPECT_EQ(RoundTrip.X87.Image, Runtime.X87.Image);
    EXPECT_EQ(RoundTrip.X87.KnownPayload, Runtime.X87.KnownPayload);
    EXPECT_EQ(RoundTrip.Integer.GPR[0], Runtime.Integer.GPR[0]);
    EXPECT_EQ(RoundTrip.Integer.RIP, Runtime.Integer.RIP);
    EXPECT_EQ(findRegisterValue(Restored, X87ID + 1)->Value,
              llvm::APInt(19, 0x12345));
    ASSERT_EQ(Restored.Memory.size(), 1u);
    EXPECT_EQ(Restored.Memory[0].Bytes, Logical.Memory[0].Bytes);
    EXPECT_EQ(Restored.Memory[0].Permissions, MemoryPermission::Write);
    EXPECT_EQ(Restored.Memory[0].Generation, 3u);
  }
}

TEST(RuntimeX87GuestState, InvalidImportAndCommitPreserveLogicalState) {
  auto Logical =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  llvm::cantFail(setRegisterValue(Logical, X87ID + 1, llvm::APInt(8, 42),
                                  "caller.occupied"));
  PortableX87StateV1 X87;
  auto Before = llvm::cantFail(serializeGuestState(Logical));
  expectError(setPortableX87GuestStateV1(Logical, X87ID + 1, X87));
  expectError(setPortableX87GuestStateV1(Logical, 0, X87));
  X87.Reserved[0] = 1;
  expectError(setPortableX87GuestStateV1(Logical, X87ID, X87));
  EXPECT_EQ(Before, llvm::cantFail(serializeGuestState(Logical)));
  X87.Reserved[0] = 0;
  llvm::cantFail(setPortableX87GuestStateV1(Logical, X87ID, X87));
  Before = llvm::cantFail(serializeGuestState(Logical));
  auto Runtime = llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Logical));
  Runtime.Integer.GPR[0] = 42;
  Runtime.LastStep.Attempted = 1;
  expectError(applyRuntimeGuestStateX86_64X87V1(Runtime, Logical));
  EXPECT_EQ(Before, llvm::cantFail(serializeGuestState(Logical)));
  auto Bad = Logical;
  findRegisterValue(Bad, X87ID)->Value.flipBit(0);
  auto BadHeader = createRuntimeGuestStateX86_64X87V1(Bad);
  ASSERT_FALSE(static_cast<bool>(BadHeader));
  llvm::consumeError(BadHeader.takeError());
  findRegisterValue(Bad, X87ID)->Value = llvm::APInt(8, 0);
  auto BadWidth = createRuntimeGuestStateX86_64X87V1(Bad);
  ASSERT_FALSE(static_cast<bool>(BadWidth));
  llvm::consumeError(BadWidth.takeError());
  expectError(setPortableX87GuestStateV1(Logical, X87ID + 2, X87));
  EXPECT_EQ(Before, llvm::cantFail(serializeGuestState(Logical)));
}

TEST(RuntimeX87GuestState, DiagnosticsSeparateCompletionAndScalarValidity) {
  RuntimeX87LastStepV1 Step;
  llvm::cantFail(validateRuntimeX87LastStepV1(Step));
  Step.Attempted = 1;
  Step.Selector = 0xe0df; // FNSTSW AX.
  Step.PC = 0x1000;
  Step.NextPC = 0x1002;
  Step.Status = PortableX87StepStatusV1::PendingException;
  llvm::cantFail(validateRuntimeX87LastStepV1(Step));
  Step.HasScalarResult = 1;
  expectError(validateRuntimeX87LastStepV1(Step));
  Step.Status = PortableX87StepStatusV1::Completed;
  Step.InstructionCompleted = 1;
  Step.ScalarResult = 0x2800;
  llvm::cantFail(validateRuntimeX87LastStepV1(Step));
  Step.HasScalarResult = 0;
  expectError(validateRuntimeX87LastStepV1(Step));
  Step.HasScalarResult = 1;
  Step.Status = static_cast<PortableX87StepStatusV1>(999);
  expectError(validateRuntimeX87LastStepV1(Step));
  Step.Status = PortableX87StepStatusV1::Completed;
  Step.NextPC = Step.PC;
  expectError(validateRuntimeX87LastStepV1(Step));
}

} // namespace
