//===- X64ProbeExecutionTests.cpp - Original native startup execution ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "arch/x86_64/X64MachineProbe.h"
#include "backends/MachineFactories.h"
#include "core/ExecutionDiagnostics.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

#include <cstring>

namespace neverd::emulation {
namespace {
#define NEVERD_X64_PROBE_TEST_VALUE(Name, Value)                               \
  constexpr uint64_t Name = Value;
#define NEVERD_X64_PROBE_TEST_CODE(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64ProbeCases.def"
#undef NEVERD_X64_PROBE_TEST_CODE
#undef NEVERD_X64_PROBE_TEST_VALUE
class OriginalTransport : public X64Machine {
public:
  X64Machine &Native;
  MemoryProjection &Memory;
  unsigned Entries = 0;
  std::optional<std::chrono::steady_clock::time_point> Deadline;
  OriginalTransport(X64Machine &Native, MemoryProjection &Memory)
      : Native(Native), Memory(Memory) {}
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    if (!Entries) {
      Deadline = Control.Deadline;
      auto *Code =
          Memory.data() + x64::gateway::CodeGPA + x64::probe::CodeOffset;
      EXPECT_EQ(llvm::ArrayRef<uint8_t>(Code, sizeof(Program)),
                llvm::ArrayRef<uint8_t>(Program));
      // The independent assembler bytes, not host-side projected results,
      // execute on the actual CPU inside the private supervisor mapping.
      std::memcpy(Code, Program, sizeof(Program));
    }
    EXPECT_EQ(Control.Deadline, *Deadline);
    EXPECT_EQ(Control.Stop, nullptr);
    ++Entries;
    return Native.step(State, Root, Control);
  }
};
struct ProbeBackend {
  ExecutionBackendKind Backend;
};
void PrintTo(const ProbeBackend &Value, std::ostream *OS) {
  *OS << executionBackendName(Value.Backend);
}
class X64ProbeExecution : public testing::TestWithParam<ProbeBackend> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> Machine;
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto Created = GetParam().Backend == ExecutionBackendKind::KVM
                       ? createKvmMachine(*Memory)
                   : GetParam().Backend == ExecutionBackendKind::HVF
                       ? createHvfX64Machine(*Memory)
                       : createWhpMachine(*Memory);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(GetParam().Backend, GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Machine = std::move(*Created);
  }
};
TEST_P(X64ProbeExecution, OriginalProgramChecksFullStateAndOneDeadline) {
  static_assert(sizeof(Program) == x64::probe::programBytes());
  OriginalTransport Original(*Machine, *Memory);
  ASSERT_EQ(llvm::toString(verifyX64Machine(Original, *Memory)), "");
  EXPECT_EQ(Original.Entries, Instructions);
  EXPECT_EQ(llvm::toString(Memory->mutableMemory()), "");
}
TEST_P(X64ProbeExecution, ExistingGuestMappingAndRAMSurviveInitialization) {
  std::vector<uint8_t> Before(x64::gateway::Bytes, Fill);
  llvm::cantFail(Memory->map(x64::KernelMin, Before.size(), Read | Write));
  llvm::cantFail(Memory->write(x64::KernelMin, Before));
  OriginalTransport Original(*Machine, *Memory);
  ASSERT_EQ(llvm::toString(verifyX64Machine(Original, *Memory)), "");
  EXPECT_GE(x64ExceptionMonitorBase(*Memory), x64::KernelMin + Before.size());
  std::vector<uint8_t> After(Before.size());
  ASSERT_EQ(llvm::toString(Memory->read(x64::KernelMin, After)), "");
  EXPECT_EQ(After, Before);
  EXPECT_EQ(Original.Entries, Instructions);
}
#define NEVERD_X64_MASK_TEST_VALUE(Name, Value) constexpr uint32_t Name = Value;
#include "X64ProbeCases.def"
#undef NEVERD_X64_MASK_TEST_VALUE
class MaskTransport final : public OriginalTransport {
public:
  using OriginalTransport::OriginalTransport;
  std::optional<uint32_t> Raw;
  bool CorruptDAZ = false, FailDAZ = false;
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    if (FailDAZ && Entries == Instructions + 1)
      return diagnostic::error(diagnostic::KvmState);
    if (auto E = OriginalTransport::step(State, Root, Control))
      return E;
    if (Raw && Entries == Instructions + 1) {
      auto *Saved = Memory.data() + x64::gateway::DataGPA +
                    State.reg(X64Register::R11) -
                    x64ExceptionMonitorBase(Memory);
      llvm::support::endian::write32le(Saved + MaskOffset, *Raw);
    }
    if (CorruptDAZ && Entries == Instructions + 2)
      State.MXCSR |= DenormalStatus;
    return llvm::Error::success();
  }
};
class X64MXCSRProbe : public X64ProbeExecution {};
class SIMDTransport final : public OriginalTransport {
public:
  using OriginalTransport::OriginalTransport;
  enum class Failure { None, MissingFault, WrongVector, ChangedDestination };
  Failure Mode = Failure::None;
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    return llvm::handleErrors(
        OriginalTransport::step(State, Root, Control),
        [&](const X64ExceptionError &E) -> llvm::Error {
          auto Fault = E.exception();
          if (Mode == Failure::MissingFault)
            return llvm::Error::success();
          if (Mode == Failure::WrongVector)
            Fault.Vector = unsigned(x64::ExceptionVector::InvalidOpcode);
          if (Mode == Failure::ChangedDestination)
            State.Xmm[0][0] ^= CorruptBit;
          return llvm::make_error<X64ExceptionError>(Fault);
        });
  }
};
TEST_P(X64MXCSRProbe, SIMDDeliveryAndRetryAreRequiredBeforePublication) {
  using Failure = SIMDTransport::Failure;
  for (auto Mode : {Failure::None, Failure::MissingFault, Failure::WrongVector,
                    Failure::ChangedDestination}) {
    SCOPED_TRACE(unsigned(Mode));
    SIMDTransport Original(*Machine, *Memory);
    Original.Mode = Mode;
    uint32_t Mask = MaskSentinel;
    auto E = verifyX64Machine(Original, *Memory, &Mask, true);
    EXPECT_EQ(bool(E), Mode != Failure::None);
    llvm::consumeError(std::move(E));
    EXPECT_EQ(Mask,
              Mode == Failure::None ? Machine->mxcsrMask() : MaskSentinel);
    EXPECT_EQ(llvm::toString(Memory->mutableMemory()), "");
  }
}
TEST_P(X64MXCSRProbe, NativeSaveAndDAZExecutionAuthenticateCapability) {
  MaskTransport Original(*Machine, *Memory);
  uint32_t Mask = MaskSentinel;
  ASSERT_EQ(llvm::toString(verifyX64Machine(Original, *Memory, &Mask)), "");
  EXPECT_EQ(Mask, Machine->mxcsrMask());
  EXPECT_TRUE(Mask == SupportedMask || Mask == FallbackMask);
  EXPECT_EQ(Original.Entries, Instructions + (Mask == SupportedMask ? 2 : 1));
}
TEST_P(X64MXCSRProbe, ZeroMaskUsesBaselineAndRejectedProbesPublishNothing) {
  for (const auto Raw : {uint32_t(0), uint32_t(1), SupportedMask}) {
    MaskTransport Original(*Machine, *Memory);
    Original.Raw = Raw;
    Original.CorruptDAZ = Raw == SupportedMask;
    uint32_t Mask = MaskSentinel;
    auto E = verifyX64Machine(Original, *Memory, &Mask);
    EXPECT_EQ(bool(E), Raw != 0);
    llvm::consumeError(std::move(E));
    EXPECT_EQ(Mask, Raw ? MaskSentinel : FallbackMask);
    EXPECT_EQ(llvm::toString(Memory->mutableMemory()), "");
  }
  MaskTransport Failure(*Machine, *Memory);
  Failure.FailDAZ = true;
  uint32_t Mask = MaskSentinel;
  EXPECT_EQ(llvm::toString(verifyX64Machine(Failure, *Memory, &Mask)),
            diagnostic::KvmState);
  EXPECT_EQ(Mask, MaskSentinel);
  EXPECT_EQ(llvm::toString(Memory->mutableMemory()), "");
}
class X64BranchProbe : public X64ProbeExecution {};
class BranchTransport : public OriginalTransport {
public:
  using OriginalTransport::OriginalTransport;
  std::optional<unsigned> CorruptAt;
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    const auto Index = Entries;
    if (auto E = OriginalTransport::step(State, Root, Control))
      return E;
    if (CorruptAt == Index)
      State.reg(X64Register::AX) ^= CorruptBit;
    return llvm::Error::success();
  }
};
TEST_P(X64BranchProbe, OriginalBranchesDiscoverOneImmutableDecoderModel) {
  BranchTransport Original(*Machine, *Memory);
  const auto Expected = Machine->branchModel();
  auto Model = Expected == X64BranchModel::Intel ? X64BranchModel::AMD
                                                 : X64BranchModel::Intel;
  auto E = verifyX64Machine(Original, *Memory, nullptr, false, &Model);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  EXPECT_EQ(Model, Expected);
  EXPECT_EQ(Original.Entries,
            std::size(x64::probe::Program) + BranchProbeCount);
  EXPECT_EQ(llvm::toString(Memory->mutableMemory()), "");
}
TEST_P(X64BranchProbe, BranchStateFailureCannotPublishTheDiscoveredModel) {
  const auto Sentinel = Machine->branchModel() == X64BranchModel::Intel
                            ? X64BranchModel::AMD
                            : X64BranchModel::Intel;
  for (unsigned I = 0; I < BranchProbeCount; ++I) {
    BranchTransport Original(*Machine, *Memory);
    Original.CorruptAt = std::size(x64::probe::Program) + I;
    auto Model = Sentinel;
    auto E = verifyX64Machine(Original, *Memory, nullptr, false, &Model);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(Model, Sentinel);
    EXPECT_EQ(Original.Entries, *Original.CorruptAt + 1);
    EXPECT_EQ(llvm::toString(Memory->mutableMemory()), "");
  }
}
INSTANTIATE_TEST_SUITE_P(
    Native, X64BranchProbe,
    testing::Values(ProbeBackend{ExecutionBackendKind::KVM},
                    ProbeBackend{ExecutionBackendKind::WHP}),
    [](const auto &Info) { return executionBackendName(Info.param.Backend); });

INSTANTIATE_TEST_SUITE_P(
    Native, X64MXCSRProbe,
    testing::Values(ProbeBackend{ExecutionBackendKind::KVM},
                    ProbeBackend{ExecutionBackendKind::WHP}),
    [](const auto &Info) { return executionBackendName(Info.param.Backend); });

INSTANTIATE_TEST_SUITE_P(
    Native, X64ProbeExecution,
    testing::Values(ProbeBackend{ExecutionBackendKind::KVM},
                    ProbeBackend{ExecutionBackendKind::WHP},
                    ProbeBackend{ExecutionBackendKind::HVF}),
    [](const auto &Info) { return executionBackendName(Info.param.Backend); });
} // namespace
} // namespace neverd::emulation
