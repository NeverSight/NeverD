//===- AArch64StateTests.cpp - ARM64 state capture tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64State.h"
#include "core/ExecutionDiagnostics.h"
#include "gtest/gtest.h"

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_STATE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_AARCH64_STATE_TEXT(Name, Value) constexpr char Name[] = Value;
#include "AArch64StateCases.def"
#undef NEVERD_AARCH64_STATE_TEXT
#undef NEVERD_AARCH64_STATE_VALUE

uint64_t captured(AArch64Register Register) {
  return Register == AArch64Register::NZCV
             ? RawPState
             : CapturedScalar + unsigned(Register);
}
class AArch64CompleteState : public testing::TestWithParam<bool> {
protected:
  AArch64MachineState State;
  void SetUp() override {
    State.UserMode = GetParam();
    State.Registers.fill(InitialScalar);
    State.Vectors.fill({InitialVectorLow, InitialVectorHigh});
  }
  static llvm::Expected<uint64_t> readScalar(AArch64Register Register) {
    return captured(Register);
  }
  static llvm::Expected<RegisterValue> readVector(unsigned Index) {
    return RegisterValue{CapturedScalar + Index, ~CapturedScalar - Index};
  }
  void expectPreserved(const AArch64MachineState &Before) {
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
  }
  void expectCaptured() {
    EXPECT_EQ(State.UserMode, GetParam());
    for (unsigned Index = 0; Index < State.Registers.size(); ++Index) {
      const auto Register = AArch64Register(Index);
      const uint64_t Expected = Register == AArch64Register::NZCV ? CapturedNZCV
                                : Register == AArch64Register::FPCR ||
                                        Register == AArch64Register::FPSR
                                    ? captured(Register) & Scalar32Mask
                                    : captured(Register);
      EXPECT_EQ(State.reg(Register), Expected) << Index;
    }
    for (unsigned Index = 0; Index < State.Vectors.size(); ++Index)
      EXPECT_EQ(State.Vectors[Index], llvm::cantFail(readVector(Index)))
          << Index;
  }
};
TEST_P(AArch64CompleteState, PublishesAllScalarsAndVectorsAtOneBoundary) {
  ASSERT_EQ(llvm::toString(captureAArch64State(State, readScalar, readVector)),
            "");
  expectCaptured();
}
TEST_P(AArch64CompleteState, CompletePacketsNormalizeWithoutChangingPrivilege) {
  for (unsigned Index = 0; Index < State.Registers.size(); ++Index)
    State.Registers[Index] = captured(AArch64Register(Index));
  for (unsigned Index = 0; Index < State.Vectors.size(); ++Index)
    State.Vectors[Index] = llvm::cantFail(readVector(Index));
  normalizeAArch64State(State);
  expectCaptured();
  const auto Normalized = State;
  normalizeAArch64State(State);
  expectPreserved(Normalized);
}
TEST_P(AArch64CompleteState, ReadersCannotObservePartialPublication) {
  const auto Before = State;
  unsigned Reads = 0;
  ASSERT_EQ(llvm::toString(captureAArch64State(
                State,
                [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
                  expectPreserved(Before);
                  ++Reads;
                  return readScalar(Register);
                },
                [&](unsigned Index) -> llvm::Expected<RegisterValue> {
                  expectPreserved(Before);
                  ++Reads;
                  return readVector(Index);
                })),
            "");
  EXPECT_EQ(Reads, State.Registers.size() + State.Vectors.size());
  expectCaptured();
}
TEST_P(AArch64CompleteState, EveryScalarAndVectorFailureRetainsCompleteInput) {
  const auto Before = State;
  const unsigned Total = State.Registers.size() + State.Vectors.size();
  for (unsigned FailedRead = 0; FailedRead < Total; ++FailedRead) {
    SCOPED_TRACE(FailedRead);
    State = Before;
    unsigned Reads = 0;
    auto E = captureAArch64State(
        State,
        [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
          if (Reads++ == FailedRead)
            return diagnostic::error(ReadFailure);
          return readScalar(Register);
        },
        [&](unsigned Index) -> llvm::Expected<RegisterValue> {
          if (Reads++ == FailedRead)
            return diagnostic::error(ReadFailure);
          return readVector(Index);
        });
    EXPECT_EQ(llvm::toString(std::move(E)), ReadFailure);
    EXPECT_EQ(Reads, FailedRead + 1);
    expectPreserved(Before);
    ASSERT_EQ(
        llvm::toString(captureAArch64State(State, readScalar, readVector)), "");
    expectCaptured();
  }
}
TEST_P(AArch64CompleteState, MissingEitherReaderCannotPublishState) {
  const auto Before = State;
  EXPECT_EQ(llvm::toString(captureAArch64State(State, {}, readVector)),
            diagnostic::Register);
  expectPreserved(Before);
  EXPECT_EQ(llvm::toString(captureAArch64State(State, readScalar, {})),
            diagnostic::Register);
  expectPreserved(Before);
  EXPECT_EQ(llvm::toString(captureAArch64State(State, {}, {})),
            diagnostic::Register);
  expectPreserved(Before);
}
INSTANTIATE_TEST_SUITE_P(Privileges, AArch64CompleteState, testing::Bool());
} // namespace
} // namespace neverd::emulation
