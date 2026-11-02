//===- X64FloatIntegerTests.cpp - Scalar SSE to signed integer ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64DAZTestSupport.h"
#include "arch/x86_64/X64Machine.h"
#include "core/ExecutionDiagnostics.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <climits>
#include <cstring>
#include <stdexcept>

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_FLOAT_INT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_FLOAT_INT_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_FLOAT_INT_INVALID(Name, ...)                                    \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FloatIntegerCases.def"
#undef NEVERD_FLOAT_INT_INVALID
#undef NEVERD_FLOAT_INT_TEXT
#undef NEVERD_FLOAT_INT_VALUE
struct Conversion {
  const char *Name;
  uint8_t Prefix;
  uint8_t Opcode;
  unsigned SourceBits;
  bool Truncate;
};
constexpr Conversion Conversions[] = {
#define NEVERD_FLOAT_INT_OPERATION(Name, Prefix, Opcode, Bits, Truncate)       \
  {#Name, Prefix, Opcode, Bits, Truncate},
#include "X64FloatIntegerCases.def"
#undef NEVERD_FLOAT_INT_OPERATION
};
struct Rounding {
  const char *Name;
  uint64_t Control;
  llvm::APFloat::roundingMode Mode;
};
constexpr Rounding Roundings[] = {
#define NEVERD_FLOAT_INT_ROUND(Name, Bits, Mode)                               \
  {#Name, Bits, llvm::APFloat::Mode},
#include "X64FloatIntegerCases.def"
#undef NEVERD_FLOAT_INT_ROUND
};
struct NumericInput {
  const char *Name;
  uint64_t Single, Double;
};
#define NEVERD_FLOAT_INT_INPUT(Name, Single, Double)                           \
  constexpr NumericInput Name = {#Name, Single, Double};
#include "X64FloatIntegerCases.def"
#undef NEVERD_FLOAT_INT_INPUT
constexpr NumericInput Inputs[] = {
#define NEVERD_FLOAT_INT_INPUT(Name, Single, Double) Name,
#include "X64FloatIntegerCases.def"
#undef NEVERD_FLOAT_INT_INPUT
};
uint64_t inputValue(const Conversion &C, const NumericInput &Input) {
  return C.SourceBits == SingleBits ? Input.Single : Input.Double;
}
// The independent mask fixture lists the GPRs in machine encoding order.
constexpr CPURegister GeneralRegisters[] = {
#define NEVERD_MASK_REGISTER(Name) CPURegister::X64##Name,
#include "X64VectorMaskCases.def"
#undef NEVERD_MASK_REGISTER
};
unsigned sourceBytes(const Conversion &C) { return C.SourceBits / CHAR_BIT; }
RegisterValue sourceVector(const Conversion &C, uint64_t Input) {
  return {C.SourceBits == SingleBits
              ? (SentinelLow & ~uint64_t(UINT32_MAX)) | Input
              : Input,
          SentinelHigh};
}

struct Result {
  uint64_t Integer, MXCSR;
};
Result reference(const Conversion &C, bool Wide, uint64_t Input,
                 const Rounding &R, uint64_t Control) {
  const auto Value = daz_test::operand(
      C.SourceBits == SingleBits ? llvm::APFloat::IEEEsingle()
                                 : llvm::APFloat::IEEEdouble(),
      llvm::APInt(C.SourceBits, Input), Control);
  llvm::APSInt Integer(Wide ? DoubleBits : SingleBits, false);
  bool Exact;
  const auto Status = Value.convertToInteger(
      Integer, C.Truncate ? llvm::APFloat::rmTowardZero : R.Mode, &Exact);
  EXPECT_EQ(unsigned(Status) & ~unsigned(llvm::APFloat::opInvalidOp |
                                         llvm::APFloat::opInexact),
            0u);
  // APFloat saturates invalid integer conversions. SSE instead publishes the
  // signed integer indefinite and raises invalid without raising precision.
  if (Status & llvm::APFloat::opInvalidOp)
    return {
        llvm::APInt::getSignedMinValue(Integer.getBitWidth()).getZExtValue(),
        Control | InvalidStatus};
  return {Integer.getZExtValue(),
          Control |
              ((Status & llvm::APFloat::opInexact) ? PrecisionStatus : 0)};
}

std::vector<uint8_t> instruction(const Conversion &C, bool Wide,
                                 unsigned Dest = 0, unsigned Source = 0,
                                 bool Memory = false) {
  std::vector<uint8_t> Bytes{C.Prefix};
  const uint8_t Extension = (Wide ? RexWide : 0) |
                            (Dest > RegisterMask ? RexDestination : 0) |
                            (!Memory && Source > RegisterMask ? RexSource : 0);
  if (Extension)
    Bytes.push_back(Rex | Extension);
  Bytes.push_back(OpcodeMap);
  Bytes.push_back(C.Opcode);
  Bytes.push_back(
      ((Dest & RegisterMask) << RegisterShift) |
      (Memory ? MemoryModRM : RegisterModRM | (Source & RegisterMask)));
  return Bytes;
}

void nativeOracle(uint64_t DAZ) {
#if defined(__x86_64__) || defined(_M_X64)
  const auto Mask = daz_test::hostMXCSRMask();
  if (DAZ && !(Mask & DAZ))
    GTEST_SKIP() << daz_test::HostUnavailable;
#define NEVERD_FP_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FPCases.def"
#undef NEVERD_FP_BYTES
#define NEVERD_FLOAT_INT_BYTES(Name, ...)                                      \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FloatIntegerCases.def"
#undef NEVERD_FLOAT_INT_BYTES
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (bool Memory : {false, true}) {
#ifdef _WIN32
        std::vector<uint8_t> Bytes(std::begin(Win64Before),
                                   std::end(Win64Before));
        const auto After = llvm::ArrayRef<uint8_t>(Win64After);
#else
        std::vector<uint8_t> Bytes(std::begin(SysVBefore),
                                   std::end(SysVBefore));
        const auto After = llvm::ArrayRef<uint8_t>(SysVAfter);
#endif
        Bytes.insert(Bytes.end(), std::begin(PoisonResult),
                     std::end(PoisonResult));
        const auto Convert = instruction(C, Wide, 0, 0, Memory);
        Bytes.insert(Bytes.end(), Convert.begin(), Convert.end());
        Bytes.insert(Bytes.end(), After.begin(), After.end());
        std::error_code EC;
        auto Block = llvm::sys::Memory::allocateMappedMemory(
            PageSize, nullptr,
            llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
        ASSERT_FALSE(bool(EC)) << EC.message();
        auto Release = llvm::scope_exit(
            [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
        std::memcpy(Block.base(), Bytes.data(), Bytes.size());
        EC = llvm::sys::Memory::protectMappedMemory(
            Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
        ASSERT_FALSE(bool(EC)) << EC.message();
        llvm::sys::Memory::InvalidateInstructionCache(Block.base(),
                                                      Bytes.size());
        auto Execute = reinterpret_cast<uint64_t (*)(
            void *, void *, void *, const void *)>(Block.base());
        for (const auto &Input : Inputs)
          for (const auto &R : Roundings)
            for (auto Sticky :
                 {uint64_t(0), ExistingStatus,
                  ExistingStatus | InvalidStatus | PrecisionStatus})
              for (auto Flush : {uint64_t(0), FlushToZero}) {
                SCOPED_TRACE(C.Name);
                SCOPED_TRACE(Wide);
                SCOPED_TRACE(Memory);
                SCOPED_TRACE(Input.Name);
                SCOPED_TRACE(R.Name);
                const auto Raw = inputValue(C, Input);
                X64MachineState Seed;
                for (unsigned N = 0; N < XmmCount; ++N)
                  Seed.Xmm[N] = {SentinelLow + N, SentinelHigh - N};
                Seed.Xmm[0] = sourceVector(C, Raw);
                Seed.MXCSR = InitialMXCSR | DAZ | R.Control | Sticky | Flush;
                alignas(x64::fp::RegisterSlotBytes)
                    std::array<uint8_t, x64::fp::LegacyBytes>
                        Before{}, After{}, Host{};
                ASSERT_EQ(llvm::toString(encodeX64FXState(Seed, Before, Mask)),
                          "");
                const auto Integer =
                    Execute(Before.data(), After.data(), Host.data(), &Raw);
                X64MachineState Actual;
                ASSERT_EQ(llvm::toString(decodeX64FXState(Actual, After)), "");
                const auto Expected = reference(C, Wide, Raw, R, Seed.MXCSR);
                EXPECT_EQ(Integer, Expected.Integer);
                EXPECT_EQ(Actual.Xmm, Seed.Xmm);
                EXPECT_EQ(Actual.MXCSR, Expected.MXCSR);
              }
      }
#else
  GTEST_SKIP();
#endif
}

TEST(X64FloatIntegerOracle, AllRoundingModesMatchOriginalNativeInstructions) {
  nativeOracle(0);
}
TEST(X64FloatIntegerDAZOracle,
     AllRoundingModesMatchOriginalNativeInstructions) {
  nativeOracle(daz_test::DenormalsAreZero);
}

class X64FloatInteger : public X64VectorTest {
protected:
  void SetUp() override { resetMemory(); }
  void resetMemory() {
    reset();
    if (!CPU)
      return;
    ASSERT_EQ(llvm::toString(CPU->map(Data + PageSize, PageSize,
                                      Read | Write | UserAccessible)),
              "");
    ASSERT_EQ(
        llvm::toString(CPU->mapAlias(Alias + PageSize, Data + PageSize,
                                     PageSize, Read | Write | UserAccessible)),
        "");
  }
  void initialize(const Conversion &C, uint64_t Input, uint64_t Control,
                  unsigned Dest = 0, unsigned Source = 0, bool Memory = false,
                  uint64_t Address = Data) {
    const auto Vector = sourceVector(C, Input);
    seed({Vector, {SentinelLow, SentinelHigh}}, Address);
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::PC, Code)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::MXCSR, Control)), "");
    ASSERT_EQ(llvm::toString(CPU->setXmm(Source, Vector)), "");
    if (!Memory || GeneralRegisters[Dest] != CPURegister::X64CX)
      ASSERT_EQ(llvm::toString(CPU->writeRegister(GeneralRegisters[Dest],
                                                  {SentinelLow, 0})),
                "");
    ASSERT_EQ(llvm::toString(CPU->writeRegister(
                  CPURegister::X64FP0, {SentinelLow, PhysicalExponent})),
              "");
    ASSERT_EQ(llvm::toString(CPU->writeRegister(CPURegister::X64FPTag, {1, 0})),
              "");
    std::array<uint8_t, 2 * PageSize> RAM{};
    for (size_t N = 0; N < RAM.size(); ++N)
      RAM[N] = N * PatternStride + N / PageSize;
    ASSERT_EQ(llvm::toString(CPU->write(Alias, RAM)), "");
    if (Memory && Address != Stack)
      ASSERT_EQ(
          llvm::toString(CPU->writeInteger(Address, Input, sourceBytes(C))),
          "");
  }
  auto backing(bool Observing = false) {
    std::array<uint8_t, 2 * PageSize> RAM{};
    EXPECT_EQ(llvm::toString(Observing ? CPU->read(Alias, RAM)
                                       : CPU->snapshotBacking(Alias, RAM)),
              "");
    return RAM;
  }
  void check(const Conversion &C, bool Wide, uint64_t Input, const Rounding &R,
             uint64_t OtherControl, unsigned Dest = 0, unsigned Source = 0,
             bool Memory = false, uint64_t Address = Data) {
    SCOPED_TRACE(C.Name);
    SCOPED_TRACE(Wide);
    SCOPED_TRACE(Input);
    SCOPED_TRACE(R.Name);
    SCOPED_TRACE(Dest);
    SCOPED_TRACE(Source);
    SCOPED_TRACE(Memory);
    const uint64_t Control = InitialMXCSR | R.Control | OtherControl;
    initialize(C, Input, Control, Dest, Source, Memory, Address);
    ASSERT_FALSE(HasFatalFailure());
    auto Expected = snapshot();
    const auto RAM = backing();
    const auto Value = reference(C, Wide, Input, R, Control);
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, sourceBytes(C));
      EXPECT_EQ(snapshot(), Expected);
      EXPECT_EQ(backing(true), RAM);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
    const auto Bytes = instruction(C, Wide, Dest, Source, Memory);
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    Expected[GeneralRegisters[Dest]] = {Value.Integer, 0};
    Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
    Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(backing(), RAM);
    EXPECT_EQ(Reads, unsigned(Memory));
    EXPECT_EQ(Writes, 0u);
  }
  void rounding(const Conversion &C, uint64_t DAZ = 0) {
    for (bool Wide : {false, true})
      for (const auto &Input : Inputs)
        for (const auto &R : Roundings)
          for (auto Sticky : {uint64_t(0), ExistingStatus,
                              ExistingStatus | InvalidStatus | PrecisionStatus})
            for (auto Flush : {uint64_t(0), FlushToZero}) {
              check(C, Wide, inputValue(C, Input), R, DAZ | Sticky | Flush);
              ASSERT_FALSE(HasFatalFailure());
              check(C, Wide, inputValue(C, Input), R, DAZ | Sticky | Flush, 0,
                    0, true, Alias + 1);
              ASSERT_FALSE(HasFatalFailure());
              check(C, Wide, inputValue(C, Input), R, DAZ | Sticky | Flush, 0,
                    0, true, Alias + PageSize - sourceBytes(C) / 2);
              ASSERT_FALSE(HasFatalFailure());
            }
  }
};

#define NEVERD_FLOAT_INT_OPERATION(Name, Prefix, Opcode, Bits, Truncate)       \
  TEST_P(X64FloatInteger, Name##PreservesStateAcrossRoundingModes) {           \
    rounding({#Name, Prefix, Opcode, Bits, Truncate});                         \
  }
#include "X64FloatIntegerCases.def"
#undef NEVERD_FLOAT_INT_OPERATION

TEST_P(X64FloatInteger, EveryVectorSourceAndIntegerDestination) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (unsigned Dest = 0; Dest < std::size(GeneralRegisters); ++Dest) {
        for (unsigned Source = 0; Source < XmmCount; ++Source) {
          check(C, Wide,
                inputValue(C, Inputs[(Dest + Source) % std::size(Inputs)]),
                Roundings[(Dest + Source) % std::size(Roundings)],
                ExistingStatus, Dest, Source);
          ASSERT_FALSE(HasFatalFailure());
        }
        // Include RCX as both memory address and integer destination, and RSP.
        check(C, Wide, inputValue(C, NegativeOneHalf), Roundings[0],
              ExistingStatus, Dest, 0, true);
        ASSERT_FALSE(HasFatalFailure());
      }
}

TEST_P(X64FloatInteger, CrossPageFaultsPreserveStatusAndRetry) {
  enum class Mapping { Denied, Absent, SupervisorOnly };
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (const auto &Input : {OneHalf, QuietNaN})
        for (unsigned Page : {0u, 1u})
          for (auto Kind :
               {Mapping::Denied, Mapping::Absent, Mapping::SupervisorOnly}) {
            if (Kind == Mapping::SupervisorOnly && !GetParam().User)
              continue;
            resetMemory();
            ASSERT_FALSE(HasFatalFailure());
            const auto Width = sourceBytes(C);
            const uint64_t Address = Data + PageSize - Width / 2;
            const uint64_t Control = InitialMXCSR | ExistingStatus;
            initialize(C, inputValue(C, Input), Control, XmmCount - 1, 0, true,
                       Address);
            ASSERT_FALSE(HasFatalFailure());
            auto Expected = snapshot();
            const auto RAM = backing();
            const auto PageAddress = Data + Page * PageSize;
            if (Kind == Mapping::Absent)
              ASSERT_EQ(llvm::toString(
                            CPU->addressSpace()->unmap(PageAddress, PageSize)),
                        "");
            else
              ASSERT_EQ(llvm::toString(CPU->protect(PageAddress, PageSize,
                                                    Kind == Mapping::Denied
                                                        ? Write | UserAccessible
                                                        : Read | Write)),
                        "");
            unsigned Reads = 0, Writes = 0;
            BackendHooks Hooks;
            Hooks.Read = [&](uint64_t A, unsigned Size) {
              EXPECT_EQ(A, Address);
              EXPECT_EQ(Size, Width);
              ++Reads;
            };
            Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
            Hooks.RecoverableFault = [](const BackendFault &) { return true; };
            const auto Bytes = instruction(C, Wide, XmmCount - 1, 0, true);
            const auto Exit = run(Bytes, std::move(Hooks));
            ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
                << Exit.Diagnostic;
            ASSERT_TRUE(Exit.Fault);
            EXPECT_EQ(Exit.Fault->Address, Page ? PageAddress : Address);
            EXPECT_EQ(Exit.Fault->Size, Width / 2);
            EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
            EXPECT_EQ(Exit.Fault->Kind, Kind == Mapping::Absent
                                            ? BackendFaultKind::UnmappedMemory
                                            : BackendFaultKind::Protection);
            EXPECT_EQ(Reads, 1u);
            EXPECT_EQ(Writes, 0u);
            EXPECT_EQ(snapshot(), Expected);
            EXPECT_EQ(backing(), RAM);
            ASSERT_TRUE(CPU->takeRecoverableFault());
            EXPECT_FALSE(CPU->takeRecoverableFault());
            if (Kind == Mapping::Absent)
              ASSERT_EQ(llvm::toString(CPU->mapAlias(
                            PageAddress, Alias + Page * PageSize, PageSize,
                            Read | Write | UserAccessible)),
                        "");
            else
              ASSERT_EQ(
                  llvm::toString(CPU->protect(PageAddress, PageSize,
                                              Read | Write | UserAccessible)),
                  "");
            const auto Retry = run(Bytes);
            ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped)
                << Retry.Diagnostic;
            const auto Value =
                reference(C, Wide, inputValue(C, Input), Roundings[0], Control);
            Expected[GeneralRegisters[XmmCount - 1]] = {Value.Integer, 0};
            Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
            Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
            EXPECT_EQ(snapshot(), Expected);
            EXPECT_EQ(backing(), RAM);
          }
}

TEST_P(X64FloatInteger, ReadObserversStopBeforeConversionAndStatus) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (const auto &Input : {OneHalf, QuietNaN})
        for (bool Fail : {false, true}) {
          resetMemory();
          ASSERT_FALSE(HasFatalFailure());
          initialize(C, inputValue(C, Input), InitialMXCSR, 0, 0, true,
                     Data + 1);
          ASSERT_FALSE(HasFatalFailure());
          auto Expected = snapshot();
          const auto RAM = backing();
          unsigned Reads = 0, Writes = 0;
          BackendHooks Hooks;
          Hooks.Read = [&](uint64_t A, unsigned Size) {
            EXPECT_EQ(A, Data + 1);
            EXPECT_EQ(Size, sourceBytes(C));
            EXPECT_EQ(snapshot(), Expected);
            EXPECT_EQ(backing(true), RAM);
            ++Reads;
            if (Fail)
              throw std::runtime_error(ObserverError);
            CPU->stop();
          };
          Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
          const auto Bytes = instruction(C, Wide, 0, 0, true);
          const auto Exit = run(Bytes, std::move(Hooks));
          EXPECT_EQ(Exit.Kind, Fail ? ExecutionExitKind::BackendFailure
                                    : ExecutionExitKind::Stopped)
              << Exit.Diagnostic;
          EXPECT_EQ(Reads, 1u);
          EXPECT_EQ(Writes, 0u);
          EXPECT_EQ(snapshot(), Expected);
          EXPECT_EQ(backing(), RAM);
          if (!Fail) {
            const auto Retry = run(Bytes);
            ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped)
                << Retry.Diagnostic;
            const auto Value = reference(C, Wide, inputValue(C, Input),
                                         Roundings[0], InitialMXCSR);
            Expected[CPURegister::X64AX] = {Value.Integer, 0};
            Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
            Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
            EXPECT_EQ(snapshot(), Expected);
            EXPECT_EQ(backing(), RAM);
          }
        }
}

TEST_P(X64FloatInteger, InstructionObserversStopBeforeRegisterEffects) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (const auto &Input : {OneHalf, QuietNaN})
        for (bool Fail : {false, true}) {
          resetMemory();
          ASSERT_FALSE(HasFatalFailure());
          initialize(C, inputValue(C, Input), InitialMXCSR);
          ASSERT_FALSE(HasFatalFailure());
          const auto Before = snapshot();
          const auto RAM = backing();
          const auto Bytes = instruction(C, Wide);
          ASSERT_EQ(llvm::toString(CPU->write(Code, Bytes)), "");
          unsigned Instructions = 0, Accesses = 0;
          BackendHooks Hooks;
          Hooks.Instruction = [&](uint64_t PC, unsigned Size) {
            EXPECT_EQ(PC, Code);
            EXPECT_EQ(Size, Bytes.size());
            ++Instructions;
            CPU->stop();
            if (Fail)
              throw std::runtime_error(ObserverError);
          };
          Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
          Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
          ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
          const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
          EXPECT_EQ(Exit.Kind, Fail ? ExecutionExitKind::BackendFailure
                                    : ExecutionExitKind::Stopped)
              << Exit.Diagnostic;
          EXPECT_EQ(Instructions, 1u);
          EXPECT_EQ(Accesses, 0u);
          EXPECT_EQ(snapshot(), Before);
          EXPECT_EQ(backing(), RAM);
          if (!Fail) {
            const auto Retry = run(Bytes);
            ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped)
                << Retry.Diagnostic;
            const auto Value = reference(C, Wide, inputValue(C, Input),
                                         Roundings[0], InitialMXCSR);
            auto Expected = Before;
            Expected[CPURegister::X64AX] = {Value.Integer, 0};
            Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
            Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
            EXPECT_EQ(snapshot(), Expected);
            EXPECT_EQ(backing(), RAM);
          }
        }
}

TEST_P(X64FloatInteger, PageEndInputsUseOnlyTheFloatingSourceWidth) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true}) {
      resetMemory();
      ASSERT_FALSE(HasFatalFailure());
      const auto Width = sourceBytes(C);
      initialize(C, inputValue(C, OneHalf), InitialMXCSR, 0, 0, true,
                 Data + PageSize - Width);
      ASSERT_FALSE(HasFatalFailure());
      auto Expected = snapshot();
      const auto RAM = backing();
      ASSERT_EQ(
          llvm::toString(CPU->addressSpace()->unmap(Data + PageSize, PageSize)),
          "");
      const auto Bytes = instruction(C, Wide, 0, 0, true);
      const auto Exit = run(Bytes);
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      const auto Value = reference(C, Wide, inputValue(C, OneHalf),
                                   Roundings[0], InitialMXCSR);
      Expected[CPURegister::X64AX] = {Value.Integer, 0};
      Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
      Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
      EXPECT_EQ(snapshot(), Expected);
      EXPECT_EQ(backing(), RAM);
    }
}

TEST_P(X64FloatInteger, UnsupportedFormsRejectBeforeObservations) {
  auto Reject = [&](llvm::ArrayRef<uint8_t> Bytes) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(Conversions[0], inputValue(Conversions[0], OneHalf),
               InitialMXCSR);
    ASSERT_FALSE(HasFatalFailure());
    const auto Before = snapshot();
    const auto RAM = backing();
    unsigned Accesses = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
    const auto Exit = run(Bytes, std::move(Hooks));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Accesses, 0u);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(backing(), RAM);
  };
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (bool Memory : {false, true}) {
        auto Bytes = instruction(C, Wide, 0, 0, Memory);
        Bytes.insert(Bytes.begin(), Lock);
        Reject(Bytes);
        ASSERT_FALSE(HasFatalFailure());
      }
#define NEVERD_FLOAT_INT_INVALID(Name, ...)                                    \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Reject(Name);                                                              \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "X64FloatIntegerCases.def"
#undef NEVERD_FLOAT_INT_INVALID
}

TEST_P(X64FloatInteger, DeviceOperandsRejectBeforeCallbacks) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true}) {
      resetMemory();
      ASSERT_FALSE(HasFatalFailure());
      initialize(C, inputValue(C, OneHalf), InitialMXCSR, 0, 0, true, Stack);
      ASSERT_FALSE(HasFatalFailure());
      const auto Before = snapshot();
      const auto RAM = backing();
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
      auto Mapped = CPU->mapMMIO(Stack, PageSize, std::move(Device));
      if (GetParam().User)
        EXPECT_EQ(llvm::toString(std::move(Mapped)), diagnostic::DeviceMapping);
      else {
        ASSERT_EQ(llvm::toString(std::move(Mapped)), "");
        const auto Exit = run(instruction(C, Wide, 0, 0, true));
        EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
            << Exit.Diagnostic;
      }
      EXPECT_EQ(Calls, 0u);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(backing(), RAM);
    }
}

using X64FloatIntegerDAZ = daz_test::Fixture<X64FloatInteger>;
#define NEVERD_FLOAT_INT_OPERATION(Name, Prefix, Opcode, Bits, Truncate)       \
  TEST_P(X64FloatIntegerDAZ, Name##PreservesStateAcrossRoundingModes) {        \
    rounding({#Name, Prefix, Opcode, Bits, Truncate},                          \
             daz_test::DenormalsAreZero);                                      \
  }
#include "X64FloatIntegerCases.def"
#undef NEVERD_FLOAT_INT_OPERATION
INSTANTIATE_TEST_SUITE_P(DAZBackends, X64FloatIntegerDAZ,
                         testing::ValuesIn(daz_test::Parameters),
                         [](const auto &Info) { return Info.param.Name; });

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64FloatInteger,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
