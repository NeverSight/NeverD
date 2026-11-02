//===- X64PackedIntegerTests.cpp - SSE2 integer state and RAM effects ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64VectorTestSupport.h"
#include "arch/x86_64/X64Exception.h"
#include "core/ExecutionDiagnostics.h"

#include <stdexcept>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_PACKED_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PACKED_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_PACKED_OPERATION(Name, Intrinsic, ...)                          \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_OPERATION
#undef NEVERD_PACKED_TEXT
#undef NEVERD_PACKED_VALUE

enum class Operation {
#define NEVERD_PACKED_OPERATION(Name, ...) Name,
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_OPERATION
};
struct Instruction {
  const char *Name;
  Operation Kind;
  llvm::ArrayRef<uint8_t> Bytes;
};
constexpr Instruction Instructions[] = {
#define NEVERD_PACKED_OPERATION(Name, ...) {#Name, Operation::Name, Name},
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_OPERATION
};
#define NEVERD_PACKED_INPUT(Name, AL, AH, BL, BH)                              \
  constexpr Input Name{{AL, AH}, {BL, BH}};
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_INPUT
struct Result {
  Operation Kind;
  Input Arguments;
  RegisterValue Value;
};
constexpr Result Results[] = {
#define NEVERD_PACKED_RESULT(Name, Input, Low, High)                           \
  {Operation::Name, Input, {Low, High}},
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_RESULT
};
void checkHost(const Result &R) {
#if defined(__x86_64__) || defined(_M_X64)
  const auto Left = _mm_loadu_si128(
      reinterpret_cast<const __m128i *>(R.Arguments.Left.data()));
  const auto Right = _mm_loadu_si128(
      reinterpret_cast<const __m128i *>(R.Arguments.Right.data()));
  __m128i Value{};
  switch (R.Kind) {
#define NEVERD_PACKED_OPERATION(Name, Intrinsic, ...)                          \
  case Operation::Name:                                                        \
    Value = Intrinsic(Left, Right);                                            \
    break;
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_OPERATION
  }
  RegisterValue Actual;
  _mm_storeu_si128(reinterpret_cast<__m128i *>(Actual.data()), Value);
  EXPECT_EQ(Actual, R.Value);
#else
  (void)R;
#endif
}

class X64PackedInteger : public X64VectorTest {
protected:
  std::vector<uint8_t> memory(const Instruction &I) {
    std::vector<uint8_t> Bytes(I.Bytes.begin(), I.Bytes.end());
    Bytes.back() = MemoryModRM;
    return Bytes;
  }
};

TEST_P(X64PackedInteger, ArithmeticMatchesGoldenVectorsAndHostInstructions) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (const auto &R : Results) {
      if (R.Kind != I.Kind)
        continue;
      checkHost(R);
      for (bool Memory : {false, true}) {
        SCOPED_TRACE(Memory);
        const uint64_t Address = Alias + PageSize - VectorBytes;
        seed(R.Arguments, Address);
        llvm::cantFail(
            CPU->writeInteger(Address, R.Arguments.Right[0], WordBytes));
        llvm::cantFail(CPU->writeInteger(Address + WordBytes,
                                         R.Arguments.Right[1], WordBytes));
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t A, unsigned Size) {
          EXPECT_EQ(A, Address);
          EXPECT_EQ(Size, VectorBytes);
          ++Reads;
        };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
        const auto Bytes =
            Memory ? memory(I)
                   : std::vector<uint8_t>(I.Bytes.begin(), I.Bytes.end());
        const auto Exit = run(Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        EXPECT_EQ(Reads, unsigned(Memory));
        EXPECT_EQ(Writes, 0u);
        expectState(R.Arguments, R.Value, Code + Bytes.size(), Address);
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)),
                  R.Arguments.Right[0]);
        EXPECT_EQ(
            llvm::cantFail(CPU->readInteger(Address + WordBytes, WordBytes)),
            R.Arguments.Right[1]);
      }
    }
  }
}

TEST_P(X64PackedInteger, SourceObserversStopOrFailBeforeStateChanges) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (bool Throw : {false, true}) {
      reset();
      ASSERT_FALSE(HasFatalFailure());
      ASSERT_TRUE(CPU);
      seed(Mixed);
      unsigned Reads = 0, Writes = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t A, unsigned Size) {
        EXPECT_EQ(A, Data);
        EXPECT_EQ(Size, VectorBytes);
        ++Reads;
        if (Throw)
          throw std::runtime_error(ObserverFailure);
        CPU->stop();
      };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
      const auto Exit = run(memory(I), std::move(Hooks));
      EXPECT_EQ(Exit.Kind, Throw ? ExecutionExitKind::BackendFailure
                                 : ExecutionExitKind::Stopped)
          << Exit.Diagnostic;
      EXPECT_EQ(Reads, 1u);
      EXPECT_EQ(Writes, 0u);
      expectState(Mixed, Mixed.Left, Code);
    }
  }
}

TEST_P(X64PackedInteger, SourceFaultsPreserveStateAndResumeAfterRepair) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (bool Unmapped : {false, true}) {
      seed(Mixed);
      if (Unmapped)
        llvm::cantFail(CPU->addressSpace()->unmap(Data, PageSize));
      else
        llvm::cantFail(CPU->protect(Data, PageSize, UserAccessible));
      unsigned Reads = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t Address, unsigned Size) {
        EXPECT_EQ(Address, Data);
        EXPECT_EQ(Size, VectorBytes);
        ++Reads;
      };
      Hooks.RecoverableFault = [](const BackendFault &) { return true; };
      const auto Bytes = memory(I);
      const auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
          << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Kind, Unmapped ? BackendFaultKind::UnmappedMemory
                                           : BackendFaultKind::Protection);
      EXPECT_EQ(Exit.Fault->Address, Data);
      EXPECT_EQ(Exit.Fault->Size, VectorBytes);
      EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
      // Ordinary read observers report the attempted operand before its
      // permission fault, but still precede every instruction effect.
      EXPECT_EQ(Reads, 1u);
      expectState(Mixed, Mixed.Left, Code);
      ASSERT_TRUE(CPU->takeRecoverableFault());
      EXPECT_FALSE(CPU->takeRecoverableFault());
      if (Unmapped)
        llvm::cantFail(CPU->mapAlias(Data, Alias, PageSize,
                                     Read | Write | UserAccessible));
      else
        llvm::cantFail(
            CPU->protect(Data, PageSize, Read | Write | UserAccessible));
      const auto Resumed = run(Bytes);
      ASSERT_EQ(Resumed.Kind, ExecutionExitKind::Stopped) << Resumed.Diagnostic;
      const auto &Expected = Results[unsigned(I.Kind) * InputCount];
      ASSERT_EQ(Expected.Kind, I.Kind);
      expectState(Mixed, Expected.Value, Code + Bytes.size());
    }
  }
}

TEST_P(X64PackedInteger, InvalidFormsRejectBeforeObservations) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (unsigned Form = 0; Form < InvalidFormCount; ++Form) {
      reset();
      ASSERT_FALSE(HasFatalFailure());
      ASSERT_TRUE(CPU);
      auto Bytes = memory(I);
      uint64_t Address = Data;
      if (Form == MMXForm)
        Bytes.erase(Bytes.begin());
      else if (Form == LockedForm)
        Bytes.insert(Bytes.begin(), Lock);
      else
        ++Address;
      seed(Mixed, Address);
      unsigned Reads = 0, Writes = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
      const auto Exit = run(Bytes, std::move(Hooks));
      EXPECT_EQ(Exit.Kind, Form == MisalignedForm
                               ? ExecutionExitKind::GuestTrap
                               : ExecutionExitKind::UnsupportedOperation)
          << Exit.Diagnostic;
      if (Form == MisalignedForm) {
        ASSERT_TRUE(Exit.Fault);
        EXPECT_EQ(Exit.Fault->Interrupt,
                  unsigned(x64::ExceptionVector::GeneralProtection));
        EXPECT_EQ(Exit.Fault->ErrorCode, 0);
      }
      EXPECT_EQ(Reads, 0u);
      EXPECT_EQ(Writes, 0u);
      expectState(Mixed, Mixed.Left, Code, Address);
    }
  }
}

TEST_P(X64PackedInteger, DeviceSourcesRejectWithoutCallingTheDevice) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    reset();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(CPU);
    seed(Mixed, Stack);
    unsigned Calls = 0;
    GuestMMIOCallbacks Device;
    Device.Validate = [&](uint64_t, uint64_t, bool) {
      ++Calls;
      return llvm::Error::success();
    };
    Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      ++Calls;
      return SentinelLow;
    };
    Device.Write = [&](uint64_t, unsigned, uint64_t) {
      ++Calls;
      return llvm::Error::success();
    };
    Device.PrepareRead =
        [&](uint64_t, unsigned) -> llvm::Expected<GuestMMIOPreparedRead> {
      ++Calls;
      return GuestMMIOPreparedRead{SentinelLow, [&] {
                                     ++Calls;
                                     return llvm::Error::success();
                                   }};
    };
    const auto BeforePC = llvm::cantFail(CPU->reg(X64Register::PC));
    auto Mapping = CPU->mapMMIO(Stack, PageSize, std::move(Device));
    if (GetParam().User) {
      // User profiles reject the mapping itself. Check that result rather
      // than running an unmapped address after ignoring the setup error.
      EXPECT_EQ(llvm::toString(std::move(Mapping)), diagnostic::DeviceMapping);
      EXPECT_EQ(Calls, 0u);
      expectState(Mixed, Mixed.Left, BeforePC, Stack);
      llvm::cantFail(CPU->setReg(X64Register::CX, Data));
      const auto Exit = run(memory(I));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      const auto &Expected = Results[unsigned(I.Kind) * InputCount];
      expectState(Mixed, Expected.Value, Code + I.Bytes.size());
      continue;
    }
    ASSERT_EQ(llvm::toString(std::move(Mapping)), "");
    const auto Exit = run(memory(I));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Calls, 0u);
    expectState(Mixed, Mixed.Left, Code, Stack);
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64PackedInteger,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
