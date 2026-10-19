//===- KernelReferenceInitializerTests.cpp - Defined native inputs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defined native inputs.
///

#include "gtest/gtest.h"
#include "os/windows/kernel/KernelReferenceImage.h"
#include "os/windows/kernel/KernelReferenceInitializer.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
class KernelReferenceInitialization : public testing::Test {
protected:
  static constexpr uint64_t Output = 0x50000000, Input = 0x50001000;
  KernelReferenceImage Reference;
  KernelInitializerPlan Plan;

  void SetUp() override {
    const auto Root = std::filesystem::path(NEVERD_DRIVER_FIXTURES);
    Reference = llvm::cantFail(KernelReferenceImage::load(
        Root / "driver_kernel_debugger_reference.sys", 4 * 1024 * 1024,
        Root / "driver_kernel_debugger_reference.pdb"));
    Plan.MemoryLimit = 8 * 1024 * 1024;
    Plan.StackBase = 0x51000000;
    Plan.StackSize = 4096;
    Plan.ReturnPC = 0x52000000;
    Plan.Writable = {{Output, 16}};
    Plan.Inputs = {word(Input, 0x11223344)};
    Plan.Outputs = {{Output, 16}};
    Plan.Calls = {{entry("ReferenceInitCopy"), {Output, Input}, true},
                  {entry("ReferenceInitNext"), {Output}, true}};
  }
  uint64_t entry(const char *Name) const {
    return Reference.exports().at(Name);
  }
  static KernelInitializerBytes word(uint64_t Address, uint64_t Value) {
    KernelInitializerBytes R{Address, std::vector<uint8_t>(8)};
    llvm::support::endian::write64le(R.Bytes.data(), Value);
    return R;
  }
  std::string failure() {
    auto Result = initializeFromKernelReference(Reference, Plan);
    if (Result) {
      ADD_FAILURE() << "original initialization unexpectedly succeeded";
      return {};
    }
    return llvm::toString(Result.takeError());
  }
};

TEST_F(KernelReferenceInitialization,
       OriginalCallsShareOnlyPrivateDefinedState) {
  auto R = initializeFromKernelReference(Reference, Plan);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Outputs.size(), 1u);
  EXPECT_EQ(R->ReturnValues, (std::vector<uint64_t>{0, 0}));
  EXPECT_GT(R->Instructions, 1u);
  EXPECT_EQ(R->ServiceCalls, 0u);
  const auto &Bytes = R->Outputs[0].Bytes;
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), 0x11223347u);
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 8), 0x1122334eu);
  // A second run starts from its own inputs, not the previous CPU's memory.
  auto Again = llvm::cantFail(initializeFromKernelReference(Reference, Plan));
  EXPECT_EQ(Again.Outputs[0].Bytes, Bytes);
  EXPECT_EQ(Again.Instructions, R->Instructions);
  EXPECT_EQ(Plan.Inputs[0].Bytes, word(Input, 0x11223344).Bytes);
}

TEST_F(KernelReferenceInitialization, ResidentZeroBackingIsNotAnInputValue) {
  Plan.Inputs.clear();
  Plan.Writable.push_back({Input, 8});
  EXPECT_NE(failure().find("undefined input"), std::string::npos);
  Plan.Calls = {{entry("ReferenceInitUndefined"), {Output}, true}};
  EXPECT_NE(failure().find("undefined input"), std::string::npos);
}

TEST_F(KernelReferenceInitialization, RejectsWritesAndIncompleteOutputs) {
  Plan.Writable.clear();
  Plan.Inputs.push_back(word(Output, 0));
  EXPECT_NE(failure().find("outside declared storage"), std::string::npos);
  Plan.Writable = {{Output, 16}};
  Plan.Inputs.pop_back();
  Plan.Calls.resize(1);
  EXPECT_NE(failure().find("output contains undefined"), std::string::npos);
}

TEST_F(KernelReferenceInitialization,
       AdjacentInputsCoverWideReadButHolesDoNot) {
  Plan.Calls = {{entry("ReferenceInitPair"), {Output, Input}, false}};
  Plan.Inputs.push_back(word(Input + 8, 0xaabbccdd));
  auto R = llvm::cantFail(initializeFromKernelReference(Reference, Plan));
  EXPECT_EQ(llvm::support::endian::read64le(R.Outputs[0].Bytes.data() + 8),
            0xaabbccddu);
  Plan.Inputs[1] = word(Input + 9, 0xaabbccdd);
  EXPECT_NE(failure().find("undefined input"), std::string::npos);
}

TEST_F(KernelReferenceInitialization,
       AtomicUpdatesRequireAnInitializedOperand) {
  Plan.Calls = {{entry("ReferenceInitAtomic"), {Output}, true}};
  Plan.Outputs = {{Output, 8}};
  EXPECT_NE(failure().find("undefined input"), std::string::npos);
  Plan.Inputs.push_back(word(Output, 5));
  auto R = llvm::cantFail(initializeFromKernelReference(Reference, Plan));
  EXPECT_EQ(llvm::support::endian::read64le(R.Outputs[0].Bytes.data()), 6u);
}

TEST_F(KernelReferenceInitialization,
       ServicesPublishOnlyPreparedPrivateWrites) {
  Plan.Calls = {{entry("ReferenceInitViaService"), {Output, 0x1234}, true}};
  Plan.Services = {{entry("ReferenceInitResource"), 2,
                    [&](const KernelInitializerServiceInput &Input)
                        -> llvm::Expected<KernelInitializerServiceResult> {
                      auto A = Input.Arguments;
                      EXPECT_EQ(A.size(), 2u);
                      EXPECT_EQ(A[0], Output);
                      EXPECT_EQ(A[1], 0x1234u);
                      return KernelInitializerServiceResult{
                          A[1] + 2, {word(A[0], A[1] + 1)}};
                    }}};
  auto R = llvm::cantFail(initializeFromKernelReference(Reference, Plan));
  EXPECT_EQ(R.ServiceCalls, 1u);
  EXPECT_EQ(llvm::support::endian::read64le(R.Outputs[0].Bytes.data()),
            0x1235u);
  EXPECT_EQ(llvm::support::endian::read64le(R.Outputs[0].Bytes.data() + 8),
            0x123au);
  Plan.Services[0].Prepare = [&](const KernelInitializerServiceInput &)
      -> llvm::Expected<KernelInitializerServiceResult> {
    return KernelInitializerServiceResult{0, {word(Input, 1)}};
  };
  EXPECT_NE(failure().find("service write exceeds"), std::string::npos);
  Plan.Services.clear();
  EXPECT_NE(failure().find("int3"), std::string::npos);
}

TEST_F(KernelReferenceInitialization,
       ServicesReadDefinedStateAndRevokeTheirReader) {
  Plan.Calls = {{entry("ReferenceInitCopy"), {Output, Input}, true},
                {entry("ReferenceInitViaService"), {Output, Input}, true}};
  decltype(KernelInitializerServiceInput::Read) Retained;
  uint64_t ReadAddress = Output, ReadSize = 8;
  Plan.Services = {{entry("ReferenceInitResource"), 2,
                    [&](const KernelInitializerServiceInput &In)
                        -> llvm::Expected<KernelInitializerServiceResult> {
                      Retained = In.Read;
                      EXPECT_EQ(In.Arguments[1], Input);
                      auto Bytes = In.Read(ReadAddress, ReadSize);
                      if (!Bytes)
                        return Bytes.takeError();
                      const auto Value =
                          llvm::support::endian::read64le(Bytes->data());
                      return KernelInitializerServiceResult{Value, {}};
                    }}};
  auto R = initializeFromKernelReference(Reference, Plan);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(llvm::support::endian::read64le(R->Outputs[0].Bytes.data() + 8),
            0x1122334bu);
  auto Expired = Retained(Output, 8);
  ASSERT_FALSE(bool(Expired));
  EXPECT_NE(llvm::toString(Expired.takeError()).find("expired"),
            std::string::npos);
  ReadAddress =
      Output + 4; // The second DWORD is still undefined before service.
  EXPECT_NE(failure().find("undefined input"), std::string::npos);
  ReadAddress = UINT64_MAX - 3;
  EXPECT_NE(failure().find("read range"), std::string::npos);
  ReadAddress = Input;
  ReadSize = Plan.MemoryLimit + 1;
  EXPECT_NE(failure().find("read range"), std::string::npos);
  ReadSize = 0;
  EXPECT_NE(failure().find("read range"), std::string::npos);
  Expired = Retained(Input, 8);
  ASSERT_FALSE(bool(Expired));
  EXPECT_NE(llvm::toString(Expired.takeError()).find("expired"),
            std::string::npos);
}

TEST_F(KernelReferenceInitialization, OwnsPrivateIRQLButRejectsUnboundClock) {
  Plan.Outputs = {{Output, 8}};
  Plan.Calls = {{entry("ReferenceInitIRQL"), {Output, 2}, false},
                {entry("ReferenceInitIRQL"), {Output, 0}, false}};
  auto R = llvm::cantFail(initializeFromKernelReference(Reference, Plan));
  EXPECT_EQ(R.ReturnValues, (std::vector<uint64_t>{0, 2}));
  EXPECT_EQ(R.FinalIRQL, 0u);
  EXPECT_NE(llvm::support::endian::read64le(R.Outputs[0].Bytes.data()) & 1, 0u);
  Plan.Calls = {{entry("ReferenceInitIRQL"), {Output, 16}, false}};
  EXPECT_NE(failure().find("reserved bits"), std::string::npos);
  Plan.Calls = {{entry("ReferenceInitClock"), {}, false}};
  EXPECT_NE(failure().find("rdtsc"), std::string::npos);
}

TEST_F(KernelReferenceInitialization, PrivateInterruptActionsChangeOnlyIF) {
  Plan.Calls = {{entry("ReferenceInitInterrupts"), {Output}, false}};
  auto Result = initializeFromKernelReference(Reference, Plan);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  const auto *Bytes = Result->Outputs[0].Bytes.data();
  const auto Disabled = llvm::support::endian::read64le(Bytes);
  const auto Enabled = llvm::support::endian::read64le(Bytes + 8);
  EXPECT_NE(Disabled & 1, 0u);
  EXPECT_EQ(Disabled & (1u << 9), 0u);
  EXPECT_EQ(Enabled, Disabled | (1u << 9));
}

TEST_F(KernelReferenceInitialization, FrozenTimestampRequiresExplicitInput) {
  Plan.Writable = {{Output, 40}};
  Plan.Outputs = Plan.Writable;
  Plan.Calls = {{entry("ReferenceInitTimestamp"), {Output}, false}};
  EXPECT_NE(failure().find("rdtsc"), std::string::npos);
  Plan.TimestampCounter = 0x1234567887654321ULL;
  auto Result = initializeFromKernelReference(Reference, Plan);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  const auto *B = Result->Outputs[0].Bytes.data();
  EXPECT_EQ(llvm::support::endian::read64le(B), 0x87654321u);
  EXPECT_EQ(llvm::support::endian::read64le(B + 8), 0x12345678u);
  EXPECT_EQ(llvm::support::endian::read64le(B + 24), 0x87654321u);
  EXPECT_EQ(llvm::support::endian::read64le(B + 32), 0x12345678u);
  EXPECT_NE(llvm::support::endian::read64le(B + 16) & 1, 0u);
  Plan.TimestampCounter = 0;
  Result = initializeFromKernelReference(Reference, Plan);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(llvm::support::endian::read64le(Result->Outputs[0].Bytes.data()),
            0u);
  Plan.TimestampCounter.reset();
  EXPECT_NE(failure().find("rdtsc"), std::string::npos);
}

TEST_F(KernelReferenceInitialization, HandoffCapturesWithoutInventingAReturn) {
  Plan.Calls = {{entry("ReferenceInitCopy"), {Output, Input}, true},
                {entry("ReferenceInitViaService"), {Output, Input}, false}};
  Plan.Outputs = {{Output, 8}};
  Plan.Services = {{entry("ReferenceInitResource"), 2,
                    [](const KernelInitializerServiceInput &)
                        -> llvm::Expected<KernelInitializerServiceResult> {
                      return KernelInitializerServiceResult{0, {}, true};
                    }}};
  auto Result = initializeFromKernelReference(Reference, Plan);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_TRUE(Result->Handoff);
  EXPECT_EQ(Result->ReturnValues, (std::vector<uint64_t>{0}));
  EXPECT_EQ(Result->Handoff->Entry, entry("ReferenceInitResource"));
  EXPECT_EQ(Result->Handoff->Arguments, (std::vector<uint64_t>{Output, Input}));
  EXPECT_EQ(Result->Handoff->Registers.at(X64Register::CX), Output);
  EXPECT_EQ(Result->Handoff->Registers.at(X64Register::PC),
            entry("ReferenceInitResource"));
  Plan.Outputs = {{Output, 16}};
  EXPECT_NE(failure().find("undefined bytes"), std::string::npos);
  Plan.Outputs = {{Output, 8}};
  Plan.Calls.back().RequireZeroResult = true;
  EXPECT_NE(failure().find("handoff"), std::string::npos);
  Plan.Calls.back().RequireZeroResult = false;
  Plan.Calls.push_back({entry("ReferenceInitNext"), {Output}, true});
  EXPECT_NE(failure().find("handoff"), std::string::npos);
  Plan.Calls.pop_back();
  Plan.Services[0].Prepare = [&](const KernelInitializerServiceInput &)
      -> llvm::Expected<KernelInitializerServiceResult> {
    return KernelInitializerServiceResult{0, {word(Output, 99)}, true};
  };
  EXPECT_NE(failure().find("handoff"), std::string::npos);
}

TEST_F(KernelReferenceInitialization, EnforcesReturnsStatusAndExecutionBudget) {
  Plan.Calls = {{entry("ReferenceInitBadReturn"), {}, false}};
  EXPECT_NE(failure().find("invalid stack pointer"), std::string::npos);
  Plan.Calls = {{entry("ReferenceInitStatus"), {}, true}};
  EXPECT_NE(failure().find("unsuccessful status"), std::string::npos);
  Plan.Calls = {{entry("ReferenceInitSpin"), {}, false}};
  Plan.InstructionLimit = 32;
  EXPECT_NE(failure().find("instruction limit"), std::string::npos);
  Plan.InstructionLimit = 1000000;
  Plan.TimeoutMicroseconds = 1;
  EXPECT_NE(failure().find("deadline"), std::string::npos);
}

TEST_F(KernelReferenceInitialization, RejectsMalformedPlansAndCodeReplacement) {
  const auto Valid = Plan;
  Plan.Inputs.push_back(word(Input + 4, 0));
  EXPECT_NE(failure().find("overlapping"), std::string::npos);
  Plan = Valid;
  Plan.Inputs = {word(entry("ReferenceInitCopy"), 0)};
  EXPECT_NE(failure().find("original code"), std::string::npos);
  Plan = Valid;
  Plan.Writable.push_back({Plan.StackBase, 8});
  EXPECT_NE(failure().find("overlaps its helper"), std::string::npos);
  Plan = Valid;
  Plan.Writable.push_back({UINT64_MAX - 7, 8});
  EXPECT_NE(failure().find("overflows"), std::string::npos);
  Plan = Valid;
  Plan.Calls[0].Entry = Reference.base(); // PE headers are data, not code.
  EXPECT_NE(failure().find("executable code"), std::string::npos);
  Plan = Valid;
  Plan.Services = {{entry("ReferenceInitResource"), 0, {}}};
  EXPECT_NE(failure().find("invalid or duplicate"), std::string::npos);
  Plan = Valid;
  Plan.TimeoutMicroseconds = 0;
  EXPECT_NE(failure().find("invalid bounded"), std::string::npos);
}
} // namespace
} // namespace neverd::emulation
