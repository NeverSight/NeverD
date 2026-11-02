//===- X64PackedShiftTests.cpp - Checked SSE2 shift boundaries ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64VectorTestSupport.h"
#include "arch/x86_64/X64Exception.h"
#include "core/ExecutionDiagnostics.h"

#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <climits>
#include <stdexcept>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_SHIFT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SHIFT_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_SHIFT_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64PackedShiftCases.def"
#undef NEVERD_SHIFT_BYTES
#undef NEVERD_SHIFT_TEXT
#undef NEVERD_SHIFT_VALUE

enum class Operation {
#define NEVERD_SHIFT_LANES(Name, ...) Name,
#define NEVERD_SHIFT_BYTES_OPERATION(Name, ...) Name,
#include "X64PackedShiftCases.def"
#undef NEVERD_SHIFT_BYTES_OPERATION
#undef NEVERD_SHIFT_LANES
};
enum class Direction { Left, Right, Arithmetic };
enum class Form { Immediate, Register, Memory };
struct Shift {
  const char *Name;
  Operation Kind;
  unsigned Bits;
  Direction Way;
  llvm::ArrayRef<uint8_t> Variable, Immediate;
};
const Shift Shifts[] = {
#define NEVERD_SHIFT_LANES(Name, Bits, Way, ...)                               \
  {#Name,          Operation::Name, Bits,                                      \
   Direction::Way, Name##Variable,  Name##Immediate},
#define NEVERD_SHIFT_BYTES_OPERATION(Name, Way, ...)                           \
  {#Name, Operation::Name, VectorBits, Direction::Way, {}, Name##Immediate},
#include "X64PackedShiftCases.def"
#undef NEVERD_SHIFT_BYTES_OPERATION
#undef NEVERD_SHIFT_LANES
};
constexpr RegisterValue Inputs[] = {
#define NEVERD_SHIFT_INPUT(Low, High) {Low, High},
#include "X64PackedShiftCases.def"
#undef NEVERD_SHIFT_INPUT
};
constexpr uint64_t ImmediateCounts[] = {
#define NEVERD_SHIFT_IMMEDIATE(Value) Value,
#include "X64PackedShiftCases.def"
#undef NEVERD_SHIFT_IMMEDIATE
};
constexpr uint64_t VariableCounts[] = {
#define NEVERD_SHIFT_IMMEDIATE(Value) Value,
#define NEVERD_SHIFT_WIDE_COUNT(Value) Value,
#include "X64PackedShiftCases.def"
#undef NEVERD_SHIFT_WIDE_COUNT
#undef NEVERD_SHIFT_IMMEDIATE
};

RegisterValue scalarResult(const Shift &S, RegisterValue Input,
                           uint64_t Count) {
  llvm::APInt Source(VectorBits, llvm::ArrayRef(Input));
  llvm::APInt Result(VectorBits, 0);
  if (S.Variable.empty()) {
    Count = std::min(Count, VectorBytes) * CHAR_BIT;
    Result = S.Way == Direction::Left ? Source.shl(Count) : Source.lshr(Count);
  } else {
    for (unsigned Bit = 0; Bit != VectorBits; Bit += S.Bits) {
      auto Lane = Source.extractBits(S.Bits, Bit);
      if (S.Way == Direction::Arithmetic)
        Lane = Lane.ashr(std::min<uint64_t>(Count, S.Bits - 1));
      else if (Count >= S.Bits)
        Lane.clearAllBits();
      else
        Lane = S.Way == Direction::Left ? Lane.shl(Count) : Lane.lshr(Count);
      Result.insertBits(Lane, Bit);
    }
  }
  return {Result.getRawData()[0], Result.getRawData()[1]};
}

void checkHost(const Shift &S, const Input &I, const RegisterValue &Expected) {
#if defined(__x86_64__) || defined(_M_X64)
  const auto Left =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(I.Left.data()));
  const auto Count =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(I.Right.data()));
  __m128i Result{};
  switch (S.Kind) {
#define NEVERD_SHIFT_LANES(Name, Bits, Way, Intrinsic)                         \
  case Operation::Name:                                                        \
    Result = Intrinsic(Left, Count);                                           \
    break;
#include "X64PackedShiftCases.def"
#undef NEVERD_SHIFT_LANES
  case Operation::PSLLDQ:
    switch (I.Right[0]) {
#define NEVERD_SHIFT_IMMEDIATE(Value)                                          \
  case Value:                                                                  \
    Result = _mm_slli_si128(Left, Value);                                      \
    break;
#include "X64PackedShiftCases.def"
#undef NEVERD_SHIFT_IMMEDIATE
    default:
      ADD_FAILURE();
      return;
    }
    break;
  case Operation::PSRLDQ:
    switch (I.Right[0]) {
#define NEVERD_SHIFT_IMMEDIATE(Value)                                          \
  case Value:                                                                  \
    Result = _mm_srli_si128(Left, Value);                                      \
    break;
#include "X64PackedShiftCases.def"
#undef NEVERD_SHIFT_IMMEDIATE
    default:
      ADD_FAILURE();
      return;
    }
    break;
  }
  RegisterValue Actual;
  _mm_storeu_si128(reinterpret_cast<__m128i *>(Actual.data()), Result);
  EXPECT_EQ(Actual, Expected);
#else
  (void)S;
  (void)I;
  (void)Expected;
#endif
}

class X64PackedShift : public X64VectorTest {
protected:
  std::vector<uint8_t> instruction(const Shift &S, Form F, uint64_t Count) {
    auto Prefix = F == Form::Immediate ? S.Immediate : S.Variable;
    std::vector<uint8_t> Bytes(Prefix.begin(), Prefix.end());
    if (F == Form::Immediate)
      Bytes.push_back(Count);
    else if (F == Form::Memory)
      Bytes.back() = MemoryModRM;
    return Bytes;
  }
  void check(const Shift &S, RegisterValue Left, RegisterValue Count, Form F) {
    SCOPED_TRACE(S.Name);
    SCOPED_TRACE(unsigned(F));
    SCOPED_TRACE(Count[0]);
    const Input I{Left, Count};
    const auto Expected = scalarResult(S, Left, Count[0]);
    checkHost(S, I, Expected);
    const uint64_t Address = Alias + PageSize - VectorBytes;
    seed(I, Address);
    llvm::cantFail(CPU->writeInteger(Address, Count[0], WordBytes));
    llvm::cantFail(CPU->writeInteger(Address + WordBytes, Count[1], WordBytes));
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, VectorBytes);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
    const auto Bytes = instruction(S, F, Count[0]);
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, unsigned(F == Form::Memory));
    EXPECT_EQ(Writes, 0u);
    expectState(I, Expected, Code + Bytes.size(), Address);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)), Count[0]);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address + WordBytes, WordBytes)),
              Count[1]);
  }
};

TEST_P(X64PackedShift, ImmediateAndVectorCountsMatchHostAndScalarResults) {
  for (const auto &S : Shifts)
    for (const auto &Left : Inputs) {
      for (const auto Count : ImmediateCounts) {
        check(S, Left, {Count, SentinelHigh}, Form::Immediate);
        ASSERT_FALSE(HasFatalFailure());
      }
      if (S.Variable.empty())
        continue;
      for (const auto Count : VariableCounts)
        for (uint64_t High : {uint64_t(0), SentinelHigh})
          for (auto F : {Form::Register, Form::Memory}) {
            check(S, Left, {Count, High}, F);
            ASSERT_FALSE(HasFatalFailure());
          }
    }
}

TEST_P(X64PackedShift, SelfCountReadsTheOriginalDestination) {
  for (const auto &S : Shifts) {
    if (S.Variable.empty())
      continue;
    SCOPED_TRACE(S.Name);
    for (const auto &Left : Inputs) {
      const Input I{Left, {SentinelLow, SentinelHigh}};
      seed(I);
      auto Bytes = instruction(S, Form::Register, 0);
      Bytes.back() = SelfModRM;
      const auto Exit = run(Bytes);
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      expectState(I, scalarResult(S, Left, Left[0]), Code + Bytes.size());
    }
  }
}

TEST_P(X64PackedShift, ZeroAndOversizedCountsStillObserveAndValidateMemory) {
  enum class Action { Stop, Throw, Deny, Unmap };
  for (const auto &S : Shifts) {
    if (S.Variable.empty())
      continue;
    SCOPED_TRACE(S.Name);
    for (uint64_t Count : {uint64_t(0), LargeCount})
      for (auto A :
           {Action::Stop, Action::Throw, Action::Deny, Action::Unmap}) {
        reset();
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_TRUE(CPU);
        const Input I{Inputs[0], {Count, SentinelHigh}};
        seed(I);
        if (A == Action::Unmap)
          llvm::cantFail(CPU->addressSpace()->unmap(Data, PageSize));
        if (A == Action::Deny)
          llvm::cantFail(CPU->protect(Data, PageSize, UserAccessible));
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t Address, unsigned Size) {
          EXPECT_EQ(Address, Data);
          EXPECT_EQ(Size, VectorBytes);
          ++Reads;
          if (A == Action::Stop)
            CPU->stop();
          if (A == Action::Throw)
            throw std::runtime_error(ObserverFailure);
        };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
        Hooks.RecoverableFault = [](const BackendFault &) { return true; };
        const auto Bytes = instruction(S, Form::Memory, Count);
        const auto Exit = run(Bytes, std::move(Hooks));
        const bool Fault = A == Action::Deny || A == Action::Unmap;
        EXPECT_EQ(Exit.Kind, Fault ? ExecutionExitKind::RecoverableFault
                             : A == Action::Stop
                                 ? ExecutionExitKind::Stopped
                                 : ExecutionExitKind::BackendFailure)
            << Exit.Diagnostic;
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(Writes, 0u);
        expectState(I, I.Left, Code);
        if (!Fault)
          continue;
        ASSERT_TRUE(Exit.Fault);
        EXPECT_EQ(Exit.Fault->Address, Data);
        EXPECT_EQ(Exit.Fault->Size, VectorBytes);
        EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
        EXPECT_EQ(Exit.Fault->Kind, A == Action::Unmap
                                        ? BackendFaultKind::UnmappedMemory
                                        : BackendFaultKind::Protection);
        ASSERT_TRUE(CPU->takeRecoverableFault());
        EXPECT_FALSE(CPU->takeRecoverableFault());
        if (A == Action::Unmap)
          llvm::cantFail(CPU->mapAlias(Data, Alias, PageSize,
                                       Read | Write | UserAccessible));
        else
          llvm::cantFail(
              CPU->protect(Data, PageSize, Read | Write | UserAccessible));
        const auto Resumed = run(Bytes);
        ASSERT_EQ(Resumed.Kind, ExecutionExitKind::Stopped)
            << Resumed.Diagnostic;
        expectState(I, scalarResult(S, I.Left, Count), Code + Bytes.size());
      }
  }
}

TEST_P(X64PackedShift, InvalidFormsRejectWithoutObservationsOrStateChanges) {
  const Input I{Inputs[0], {1, SentinelHigh}};
  auto Reject = [&](std::vector<uint8_t> Bytes, uint64_t Address) {
    reset();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(CPU);
    seed(I, Address);
    unsigned Observations = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Observations; };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
    const auto Exit = run(Bytes, std::move(Hooks));
    EXPECT_EQ(Exit.Kind, Address % VectorBytes
                             ? ExecutionExitKind::GuestTrap
                             : ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    if (Address % VectorBytes) {
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Interrupt,
                unsigned(x64::ExceptionVector::GeneralProtection));
      EXPECT_EQ(Exit.Fault->ErrorCode, 0);
    }
    EXPECT_EQ(Observations, 0u);
    expectState(I, I.Left, Code, Address);
  };
  for (const auto &S : Shifts) {
    SCOPED_TRACE(S.Name);
    for (auto F : {Form::Immediate, Form::Register, Form::Memory}) {
      if (F != Form::Immediate && S.Variable.empty())
        continue;
      auto Bytes = instruction(S, F, I.Right[0]);
      Bytes.erase(Bytes.begin());
      Reject(Bytes, Data);
      ASSERT_FALSE(HasFatalFailure());
      Bytes = instruction(S, F, I.Right[0]);
      Bytes.insert(Bytes.begin(), Lock);
      Reject(Bytes, Data);
      ASSERT_FALSE(HasFatalFailure());
      if (F == Form::Memory)
        Reject(instruction(S, F, I.Right[0]), Data + 1);
      ASSERT_FALSE(HasFatalFailure());
    }
  }
  for (llvm::ArrayRef<uint8_t> Bytes :
       {llvm::ArrayRef(VexPSLLW), llvm::ArrayRef(EvexPSLLW)})
    Reject({Bytes.begin(), Bytes.end()}, Data);
}

TEST_P(X64PackedShift, DeviceCountsRejectBeforeAnyDeviceCallback) {
  for (const auto &S : Shifts) {
    if (S.Variable.empty())
      continue;
    SCOPED_TRACE(S.Name);
    reset();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(CPU);
    const Input I{Inputs[0], {0, SentinelHigh}};
    seed(I, Stack);
    unsigned Calls = 0;
    GuestMMIOCallbacks Device;
    Device.Validate = [&](uint64_t, uint64_t, bool) {
      ++Calls;
      return llvm::Error::success();
    };
    Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      ++Calls;
      return 0;
    };
    Device.Write = [&](uint64_t, unsigned, uint64_t) {
      ++Calls;
      return llvm::Error::success();
    };
    Device.PrepareRead =
        [&](uint64_t, unsigned) -> llvm::Expected<GuestMMIOPreparedRead> {
      ++Calls;
      return GuestMMIOPreparedRead{0, [&] {
                                     ++Calls;
                                     return llvm::Error::success();
                                   }};
    };
    const auto PC = llvm::cantFail(CPU->reg(X64Register::PC));
    auto Mapping = CPU->mapMMIO(Stack, PageSize, std::move(Device));
    if (GetParam().User) {
      EXPECT_EQ(llvm::toString(std::move(Mapping)), diagnostic::DeviceMapping);
      expectState(I, I.Left, PC, Stack);
    } else {
      ASSERT_EQ(llvm::toString(std::move(Mapping)), "");
      const auto Exit = run(instruction(S, Form::Memory, 0));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
          << Exit.Diagnostic;
      expectState(I, I.Left, Code, Stack);
    }
    EXPECT_EQ(Calls, 0u);
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64PackedShift,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
