//===- X64IntegerFloatTests.cpp - Signed integer to scalar SSE ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64DAZTestSupport.h"
#include "arch/x86_64/X64Machine.h"
#include "core/ExecutionDiagnostics.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <climits>
#include <cstring>
#include <stdexcept>

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_INT_FLOAT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_INT_FLOAT_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_INT_FLOAT_INVALID(Name, ...)                                    \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64IntegerFloatCases.def"
#undef NEVERD_INT_FLOAT_INVALID
#undef NEVERD_INT_FLOAT_TEXT
#undef NEVERD_INT_FLOAT_VALUE
struct Conversion {
  const char *Name;
  uint8_t Prefix;
  unsigned ResultBits;
};
constexpr Conversion Conversions[] = {
#define NEVERD_INT_FLOAT_OPERATION(Name, Prefix, Bits) {#Name, Prefix, Bits},
#include "X64IntegerFloatCases.def"
#undef NEVERD_INT_FLOAT_OPERATION
};
struct Rounding {
  const char *Name;
  uint64_t Control;
  llvm::APFloat::roundingMode Mode;
};
constexpr Rounding Roundings[] = {
#define NEVERD_INT_FLOAT_ROUND(Name, Bits, Mode)                               \
  {#Name, Bits, llvm::APFloat::Mode},
#include "X64IntegerFloatCases.def"
#undef NEVERD_INT_FLOAT_ROUND
};
constexpr uint64_t Inputs[] = {
#define NEVERD_INT_FLOAT_INPUT(Value) Value,
#include "X64IntegerFloatCases.def"
#undef NEVERD_INT_FLOAT_INPUT
};
// The independent mask fixture lists the GPRs in machine encoding order.
constexpr CPURegister GeneralRegisters[] = {
#define NEVERD_MASK_REGISTER(Name) CPURegister::X64##Name,
#include "X64VectorMaskCases.def"
#undef NEVERD_MASK_REGISTER
};
constexpr RegisterValue Destination = {SentinelLow, SentinelHigh};
unsigned sourceBytes(bool Wide) {
  return Wide ? sizeof(uint64_t) : sizeof(uint32_t);
}

struct Result {
  RegisterValue Vector;
  uint64_t MXCSR;
};
Result reference(const Conversion &C, bool Wide, uint64_t Input,
                 const Rounding &R, uint64_t Control) {
  llvm::APFloat Value(C.ResultBits == SingleBits ? llvm::APFloat::IEEEsingle()
                                                 : llvm::APFloat::IEEEdouble());
  const auto Status = Value.convertFromAPInt(
      llvm::APInt(sourceBytes(Wide) * CHAR_BIT, Input), true, R.Mode);
  EXPECT_EQ(unsigned(Status) & ~unsigned(llvm::APFloat::opInexact), 0u);
  const auto Bits = Value.bitcastToAPInt().getZExtValue();
  auto Vector = Destination;
  Vector[0] = C.ResultBits == SingleBits
                  ? (Vector[0] & ~uint64_t(UINT32_MAX)) | Bits
                  : Bits;
  return {Vector,
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
  Bytes.push_back(Opcode);
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
  for (const auto &C : Conversions)
    for (bool Wide : {false, true}) {
#ifdef _WIN32
      std::vector<uint8_t> Bytes(std::begin(Win64Before),
                                 std::end(Win64Before));
      const auto After = llvm::ArrayRef<uint8_t>(Win64After);
#else
      std::vector<uint8_t> Bytes(std::begin(SysVBefore), std::end(SysVBefore));
      const auto After = llvm::ArrayRef<uint8_t>(SysVAfter);
#endif
      const auto Convert = instruction(C, Wide, 0, 0, true);
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
      llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Bytes.size());
      auto Execute =
          reinterpret_cast<void (*)(void *, void *, void *, const void *)>(
              Block.base());
      for (auto Input : Inputs)
        for (const auto &R : Roundings)
          for (auto Sticky :
               {uint64_t(0), ExistingStatus, ExistingStatus | PrecisionStatus})
            for (auto Flush : {uint64_t(0), FlushToZero}) {
              SCOPED_TRACE(C.Name);
              SCOPED_TRACE(Wide);
              SCOPED_TRACE(Input);
              SCOPED_TRACE(R.Name);
              X64MachineState Seed;
              Seed.Xmm[0] = Destination;
              Seed.MXCSR = InitialMXCSR | DAZ | R.Control | Sticky | Flush;
              alignas(x64::fp::RegisterSlotBytes)
                  std::array<uint8_t, x64::fp::LegacyBytes>
                      Before{}, After{}, Host{};
              ASSERT_EQ(llvm::toString(encodeX64FXState(Seed, Before, Mask)),
                        "");
              Execute(Before.data(), After.data(), Host.data(), &Input);
              X64MachineState Actual;
              ASSERT_EQ(llvm::toString(decodeX64FXState(Actual, After)), "");
              const auto Expected = reference(C, Wide, Input, R, Seed.MXCSR);
              Seed.Xmm[0] = Expected.Vector;
              EXPECT_EQ(Actual.Xmm, Seed.Xmm);
              EXPECT_EQ(Actual.MXCSR, Expected.MXCSR);
            }
    }
#else
  GTEST_SKIP();
#endif
}

TEST(X64IntegerFloatOracle, AllRoundingModesMatchOriginalNativeInstructions) {
  nativeOracle(0);
}
TEST(X64IntegerFloatDAZOracle,
     AllRoundingModesMatchOriginalNativeInstructions) {
  nativeOracle(daz_test::DenormalsAreZero);
}

class X64IntegerFloat : public X64VectorTest {
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
  void initialize(uint64_t Input, uint64_t Control, unsigned Dest = 0,
                  unsigned Source = 0, bool Memory = false,
                  uint64_t Address = Data, bool Wide = true) {
    seed({Destination, {Input, SentinelHigh}}, Address);
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::PC, Code)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::MXCSR, Control)), "");
    ASSERT_EQ(llvm::toString(CPU->setXmm(Dest, Destination)), "");
    if (!Memory)
      ASSERT_EQ(llvm::toString(
                    CPU->writeRegister(GeneralRegisters[Source], {Input, 0})),
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
          llvm::toString(CPU->writeInteger(Address, Input, sourceBytes(Wide))),
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
    initialize(Input, Control, Dest, Source, Memory, Address, Wide);
    ASSERT_FALSE(HasFatalFailure());
    auto Expected = snapshot();
    const auto RAM = backing();
    const auto Value = reference(C, Wide, Input, R, Control);
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, sourceBytes(Wide));
      EXPECT_EQ(snapshot(), Expected);
      EXPECT_EQ(backing(true), RAM);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
    const auto Bytes = instruction(C, Wide, Dest, Source, Memory);
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    Expected[vectorRegister(GuestArchitecture::X64, Dest)] = Value.Vector;
    Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
    Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(backing(), RAM);
    EXPECT_EQ(Reads, unsigned(Memory));
    EXPECT_EQ(Writes, 0u);
  }
  void rounding(const Conversion &C, uint64_t DAZ = 0) {
    for (bool Wide : {false, true})
      for (auto Input : Inputs)
        for (const auto &R : Roundings)
          for (auto Sticky :
               {uint64_t(0), ExistingStatus, ExistingStatus | PrecisionStatus})
            for (auto Flush : {uint64_t(0), FlushToZero}) {
              check(C, Wide, Input, R, DAZ | Sticky | Flush);
              ASSERT_FALSE(HasFatalFailure());
              check(C, Wide, Input, R, DAZ | Sticky | Flush, 0, 0, true,
                    Alias + 1);
              ASSERT_FALSE(HasFatalFailure());
              check(C, Wide, Input, R, DAZ | Sticky | Flush, 0, 0, true,
                    Alias + PageSize - sourceBytes(Wide) / 2);
              ASSERT_FALSE(HasFatalFailure());
            }
  }
};

#define NEVERD_INT_FLOAT_OPERATION(Name, Prefix, Bits)                         \
  TEST_P(X64IntegerFloat, Name##PreservesStateAcrossRoundingModes) {           \
    rounding({#Name, Prefix, Bits});                                           \
  }
#include "X64IntegerFloatCases.def"
#undef NEVERD_INT_FLOAT_OPERATION

TEST_P(X64IntegerFloat, EveryIntegerSourceAndVectorDestination) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (unsigned Dest = 0; Dest < XmmCount; ++Dest)
        for (unsigned Source = 0; Source < std::size(GeneralRegisters);
             ++Source) {
          check(C, Wide, Inputs[(Dest + Source) % std::size(Inputs)],
                Roundings[(Dest + Source) % std::size(Roundings)],
                ExistingStatus, Dest, Source);
          ASSERT_FALSE(HasFatalFailure());
        }
}

TEST_P(X64IntegerFloat, CrossPageFaultsPreserveStatusAndRetry) {
  enum class Mapping { Denied, Absent, SupervisorOnly };
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (unsigned Page : {0u, 1u})
        for (auto Kind :
             {Mapping::Denied, Mapping::Absent, Mapping::SupervisorOnly}) {
          if (Kind == Mapping::SupervisorOnly && !GetParam().User)
            continue;
          resetMemory();
          ASSERT_FALSE(HasFatalFailure());
          const auto Width = sourceBytes(Wide);
          const uint64_t Address = Data + PageSize - Width / 2;
          const uint64_t Control = InitialMXCSR | ExistingStatus;
          initialize(InexactInput, Control, XmmCount - 1, 0, true, Address,
                     Wide);
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
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          const auto Value =
              reference(C, Wide, InexactInput, Roundings[0], Control);
          Expected[vectorRegister(GuestArchitecture::X64, XmmCount - 1)] =
              Value.Vector;
          Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          EXPECT_EQ(snapshot(), Expected);
          EXPECT_EQ(backing(), RAM);
        }
}

TEST_P(X64IntegerFloat, ReadObserversStopBeforeConversionAndPrecisionStatus) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (bool Fail : {false, true}) {
        resetMemory();
        ASSERT_FALSE(HasFatalFailure());
        initialize(InexactInput, InitialMXCSR, 0, 0, true, Data + 1, Wide);
        ASSERT_FALSE(HasFatalFailure());
        auto Expected = snapshot();
        const auto RAM = backing();
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t A, unsigned Size) {
          EXPECT_EQ(A, Data + 1);
          EXPECT_EQ(Size, sourceBytes(Wide));
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
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          const auto Value =
              reference(C, Wide, InexactInput, Roundings[0], InitialMXCSR);
          Expected[CPURegister::X64V0] = Value.Vector;
          Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          EXPECT_EQ(snapshot(), Expected);
          EXPECT_EQ(backing(), RAM);
        }
      }
}

TEST_P(X64IntegerFloat, InstructionObserversStopBeforeRegisterEffects) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true})
      for (bool Fail : {false, true}) {
        resetMemory();
        ASSERT_FALSE(HasFatalFailure());
        initialize(InexactInput, InitialMXCSR);
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
      }
}

TEST_P(X64IntegerFloat, PageEndInputsUseOnlyTheIntegerSourceWidth) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true}) {
      resetMemory();
      ASSERT_FALSE(HasFatalFailure());
      const auto Width = sourceBytes(Wide);
      initialize(InexactInput, InitialMXCSR, 0, 0, true,
                 Data + PageSize - Width, Wide);
      ASSERT_FALSE(HasFatalFailure());
      auto Expected = snapshot();
      const auto RAM = backing();
      ASSERT_EQ(
          llvm::toString(CPU->addressSpace()->unmap(Data + PageSize, PageSize)),
          "");
      const auto Bytes = instruction(C, Wide, 0, 0, true);
      const auto Exit = run(Bytes);
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      const auto Value =
          reference(C, Wide, InexactInput, Roundings[0], InitialMXCSR);
      Expected[CPURegister::X64V0] = Value.Vector;
      Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
      Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
      EXPECT_EQ(snapshot(), Expected);
      EXPECT_EQ(backing(), RAM);
    }
}

TEST_P(X64IntegerFloat, UnsupportedFormsRejectBeforeObservations) {
  auto Reject = [&](llvm::ArrayRef<uint8_t> Bytes) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(InexactInput, InitialMXCSR);
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
#define NEVERD_INT_FLOAT_INVALID(Name, ...)                                    \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Reject(Name);                                                              \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "X64IntegerFloatCases.def"
#undef NEVERD_INT_FLOAT_INVALID
}

TEST_P(X64IntegerFloat, DeviceOperandsRejectBeforeCallbacks) {
  for (const auto &C : Conversions)
    for (bool Wide : {false, true}) {
      resetMemory();
      ASSERT_FALSE(HasFatalFailure());
      initialize(InexactInput, InitialMXCSR, 0, 0, true, Stack, Wide);
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

using X64IntegerFloatDAZ = daz_test::Fixture<X64IntegerFloat>;
#define NEVERD_INT_FLOAT_OPERATION(Name, Prefix, Bits)                         \
  TEST_P(X64IntegerFloatDAZ, Name##PreservesStateAcrossRoundingModes) {        \
    rounding({#Name, Prefix, Bits}, daz_test::DenormalsAreZero);               \
  }
#include "X64IntegerFloatCases.def"
#undef NEVERD_INT_FLOAT_OPERATION
INSTANTIATE_TEST_SUITE_P(DAZBackends, X64IntegerFloatDAZ,
                         testing::ValuesIn(daz_test::Parameters),
                         [](const auto &Info) { return Info.param.Name; });

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64IntegerFloat,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
