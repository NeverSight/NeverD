//===- X64PackedFloatTests.cpp - Packed signed integer to SSE float ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64DAZTestSupport.h"
#include "arch/x86_64/X64Exception.h"
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
#define NEVERD_PACKED_FLOAT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PACKED_FLOAT_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_PACKED_FLOAT_INVALID(Name, ...)                                 \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_INVALID
#undef NEVERD_PACKED_FLOAT_TEXT
#undef NEVERD_PACKED_FLOAT_VALUE

enum class OperationKind {
#define NEVERD_PACKED_FLOAT_OPERATION(Name, ...) Name,
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_OPERATION
};
struct Conversion {
  OperationKind Kind;
  const char *Name;
  uint8_t Prefix, Opcode;
  unsigned Lanes, ResultBits, Alignment;
};
constexpr Conversion Conversions[] = {
#define NEVERD_PACKED_FLOAT_OPERATION(Name, Prefix, Opcode, Lanes, Bits,       \
                                      Alignment)                               \
  {OperationKind::Name, #Name, Prefix, Opcode, Lanes, Bits, Alignment},
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_OPERATION
};
struct Rounding {
  uint64_t Control;
  llvm::APFloat::roundingMode Mode;
};
constexpr Rounding Roundings[] = {
#define NEVERD_INT_FLOAT_ROUND(Name, Bits, Mode) {Bits, llvm::APFloat::Mode},
#include "X64IntegerFloatCases.def"
#undef NEVERD_INT_FLOAT_ROUND
};
struct NumericInput {
  const char *Name;
  uint32_t Bits;
};
#define NEVERD_PACKED_FLOAT_INPUT(Name, Bits)                                  \
  constexpr NumericInput Name = {#Name, Bits};
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_INPUT
constexpr NumericInput Inputs[] = {
#define NEVERD_PACKED_FLOAT_INPUT(Name, ...) Name,
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_INPUT
};
unsigned sourceBytes(const Conversion &C) {
  return SingleBits / CHAR_BIT * C.Lanes;
}
Input input(const Conversion &C, const NumericInput &Left,
            const NumericInput &Right, bool Negate = false) {
  // The low two lanes supply both widths; single precision also converts
  // distinct high lanes. Unsigned subtraction preserves every signed32 bit.
  const uint32_t L = Negate ? uint32_t(0) - Left.Bits : Left.Bits;
  const uint32_t R = Negate ? uint32_t(0) - Right.Bits : Right.Bits;
  const uint32_t HighL = uint32_t(0) - L;
  const uint32_t HighR = uint32_t(0) - R;
  return {{SentinelLow, SentinelHigh},
          {uint64_t(R) | (uint64_t(L) << SingleBits),
           uint64_t(HighL) | (uint64_t(HighR) << SingleBits)}};
}
struct Result {
  RegisterValue Vector;
  uint64_t MXCSR;
};
Result reference(const Conversion &C, const Input &I, uint64_t Control) {
  const auto &Format = C.ResultBits == SingleBits ? llvm::APFloat::IEEEsingle()
                                                  : llvm::APFloat::IEEEdouble();
  const auto &R = Roundings[(Control & RoundingMask) >> RoundingShift];
  RegisterValue Vector{};
  uint64_t Status = 0;
  for (unsigned Lane = 0; Lane < C.Lanes; ++Lane) {
    const auto Raw = I.Right[Lane / 2] >> ((Lane % 2) * SingleBits);
    llvm::APFloat Value(Format);
    const auto Converted =
        Value.convertFromAPInt(llvm::APInt(SingleBits, Raw), true, R.Mode);
    EXPECT_EQ(unsigned(Converted) & ~unsigned(llvm::APFloat::opInexact), 0u);
    if (Converted & llvm::APFloat::opInexact)
      Status |= PrecisionStatus;
    const auto Bits = Value.bitcastToAPInt().getZExtValue();
    if (C.ResultBits == DoubleBits)
      Vector[Lane] = Bits;
    else
      Vector[Lane / 2] |= Bits << ((Lane % 2) * SingleBits);
  }
  return {Vector, Control | Status};
}
std::vector<uint8_t> instruction(const Conversion &C, unsigned Dest = 0,
                                 unsigned Source = 1, bool Memory = false) {
  std::vector<uint8_t> Bytes;
  if (C.Prefix)
    Bytes.push_back(C.Prefix);
  const uint8_t Extension = (Dest > RegisterMask ? RexDestination : 0) |
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

void nativeOracle(OperationKind Kind, uint64_t DAZ = 0) {
#if defined(__x86_64__) || defined(_M_X64)
  const auto Mask = daz_test::hostMXCSRMask();
  if (DAZ && !(Mask & DAZ))
    GTEST_SKIP() << daz_test::HostUnavailable;
#define NEVERD_FP_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FPCases.def"
#undef NEVERD_FP_BYTES
#define NEVERD_PACKED_FLOAT_BYTES(Name, ...)                                   \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_BYTES
  for (const auto &C : Conversions) {
    if (C.Kind != Kind)
      continue;
    for (bool Memory : {false, true}) {
#ifdef _WIN32
      std::vector<uint8_t> Bytes(std::begin(Win64Before),
                                 std::end(Win64Before));
      const auto After = llvm::ArrayRef<uint8_t>(Win64After);
#else
      std::vector<uint8_t> Bytes(std::begin(SysVBefore), std::end(SysVBefore));
      const auto After = llvm::ArrayRef<uint8_t>(SysVAfter);
#endif
      Bytes.insert(Bytes.end(), std::begin(BeforeConversion),
                   std::end(BeforeConversion));
      const auto Convert = instruction(C, 0, 1, Memory);
      Bytes.insert(Bytes.end(), Convert.begin(), Convert.end());
      Bytes.insert(Bytes.end(), std::begin(AfterConversion),
                   std::end(AfterConversion));
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
          reinterpret_cast<uint64_t (*)(void *, void *, void *, const void *)>(
              Block.base());
      for (unsigned L = 0; L < std::size(Inputs); ++L)
        for (const auto &Right : Inputs)
          for (bool Negate : {false, true})
            for (const auto &R : Roundings)
              for (auto InitialFlags :
                   {ClearFlags, Flags, Flags | DirectionFlag})
                for (auto Sticky :
                     {uint64_t(0), PrecisionStatus, ExistingStatus})
                  for (auto Flush : {uint64_t(0), FlushToZero}) {
                    SCOPED_TRACE(C.Name);
                    SCOPED_TRACE(Inputs[L].Name);
                    SCOPED_TRACE(Right.Name);
                    SCOPED_TRACE(Memory);
                    SCOPED_TRACE(Negate);
                    SCOPED_TRACE(R.Control);
                    SCOPED_TRACE(Flush);
                    const auto I = input(C, Inputs[L], Right, Negate);
                    X64MachineState Seed;
                    for (unsigned N = 0; N < XmmCount; ++N)
                      Seed.Xmm[N] = {SentinelLow + N, SentinelHigh - N};
                    Seed.Xmm[0] = I.Left;
                    Seed.Xmm[1] = I.Right;
                    Seed.MXCSR =
                        InitialMXCSR | DAZ | R.Control | Sticky | Flush;
                    alignas(x64::fp::RegisterSlotBytes)
                        std::array<uint8_t, x64::fp::LegacyBytes>
                            Before{}, After{}, Host{};
                    ASSERT_EQ(
                        llvm::toString(encodeX64FXState(Seed, Before, Mask)),
                        "");
                    alignas(x64::fp::RegisterSlotBytes)
                        const std::array<uint64_t, 4>
                            Arguments{I.Right[0], I.Right[1], InitialFlags, 0};
                    const auto ActualFlags =
                        Execute(Before.data(), After.data(), Host.data(),
                                Arguments.data());
                    X64MachineState Actual;
                    ASSERT_EQ(llvm::toString(decodeX64FXState(Actual, After)),
                              "");
                    const auto Expected = reference(C, I, Seed.MXCSR);
                    ASSERT_EQ(ActualFlags, InitialFlags);
                    Seed.Xmm[0] = Expected.Vector;
                    ASSERT_EQ(Actual.Xmm, Seed.Xmm);
                    ASSERT_EQ(Actual.MXCSR, Expected.MXCSR);
                  }
    }
  }
#else
  GTEST_SKIP();
#endif
}
#define NEVERD_PACKED_FLOAT_OPERATION(Name, ...)                               \
  TEST(X64PackedFloatOracle, Name##MatchesOriginalInstructions) {              \
    nativeOracle(OperationKind::Name);                                         \
  }
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_OPERATION
#define NEVERD_PACKED_FLOAT_OPERATION(Name, ...)                               \
  TEST(X64PackedFloatDAZOracle, Name##MatchesOriginalInstructions) {           \
    nativeOracle(OperationKind::Name, daz_test::DenormalsAreZero);             \
  }
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_OPERATION

class X64PackedFloat : public X64VectorTest {
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
  void initialize(const Conversion &C, const Input &I, uint64_t Control,
                  unsigned Dest = 0, unsigned Source = 1, bool Memory = false,
                  uint64_t Address = Data, uint64_t InitialFlags = Flags) {
    seed(I, Address);
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::PC, Code)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::MXCSR, Control)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::FLAGS, InitialFlags)),
              "");
    ASSERT_EQ(llvm::toString(CPU->setXmm(Dest, I.Left)), "");
    if (!Memory)
      ASSERT_EQ(llvm::toString(CPU->setXmm(Source, I.Right)), "");
    ASSERT_EQ(llvm::toString(CPU->writeRegister(
                  CPURegister::X64FP0, {SentinelLow, PhysicalExponent})),
              "");
    ASSERT_EQ(llvm::toString(CPU->writeRegister(CPURegister::X64FPTag, {1, 0})),
              "");
    std::array<uint8_t, 2 * PageSize> RAM{};
    for (size_t N = 0; N < RAM.size(); ++N)
      RAM[N] = N * PatternStride + N / PageSize;
    ASSERT_EQ(llvm::toString(CPU->write(Alias, RAM)), "");
    if (Memory && Address != Stack) {
      std::array<uint8_t, VectorBytes> Bytes{};
      llvm::support::endian::write64le(Bytes.data(), I.Right[0]);
      llvm::support::endian::write64le(Bytes.data() + WordBytes, I.Right[1]);
      ASSERT_EQ(llvm::toString(CPU->write(
                    Address, llvm::ArrayRef(Bytes).take_front(sourceBytes(C)))),
                "");
    }
  }
  auto backing(bool Observing = false) {
    std::array<uint8_t, 2 * PageSize> RAM{};
    EXPECT_EQ(llvm::toString(Observing ? CPU->read(Alias, RAM)
                                       : CPU->snapshotBacking(Alias, RAM)),
              "");
    return RAM;
  }
  void expectSnapshot(const std::map<CPURegister, RegisterValue> &Expected) {
    const auto Actual = snapshot();
    ASSERT_EQ(Actual.size(), Expected.size());
    for (const auto &[R, Value] : Expected) {
      SCOPED_TRACE(static_cast<unsigned>(R));
      ASSERT_NE(Actual.find(R), Actual.end());
      ASSERT_EQ(Actual.at(R), Value);
    }
  }
  void check(const Conversion &C, Input I, uint64_t Control, unsigned Dest = 0,
             unsigned Source = 1, bool Memory = false, uint64_t Address = Data,
             uint64_t InitialFlags = Flags) {
    SCOPED_TRACE(C.Name);
    SCOPED_TRACE(Dest);
    SCOPED_TRACE(Source);
    SCOPED_TRACE(Memory);
    SCOPED_TRACE(Control);
    SCOPED_TRACE(InitialFlags);
    if (!Memory && Dest == Source)
      I.Left = I.Right;
    initialize(C, I, Control, Dest, Source, Memory, Address, InitialFlags);
    ASSERT_FALSE(HasFatalFailure());
    auto Expected = snapshot();
    const auto RAM = backing();
    const auto Value = reference(C, I, Control);
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, sourceBytes(C));
      expectSnapshot(Expected);
      EXPECT_EQ(backing(true), RAM);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
    const auto Bytes = instruction(C, Dest, Source, Memory);
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    Expected[vectorRegister(GuestArchitecture::X64, Dest)] = Value.Vector;
    Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
    Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    expectSnapshot(Expected);
    EXPECT_EQ(backing(), RAM);
    EXPECT_EQ(Reads, unsigned(Memory));
    EXPECT_EQ(Writes, 0u);
  }
  void convert(OperationKind Kind, uint64_t DAZ = 0) {
    for (const auto &C : Conversions) {
      if (C.Kind != Kind)
        continue;
      for (unsigned L = 0; L < std::size(Inputs); ++L)
        for (const auto &Right : Inputs)
          for (auto Sticky : {uint64_t(0), ExistingStatus})
            for (bool Memory : {false, true}) {
              SCOPED_TRACE(Inputs[L].Name);
              SCOPED_TRACE(Right.Name);
              check(C, input(C, Inputs[L], Right), InitialMXCSR | DAZ | Sticky,
                    0, 1, Memory, Alias + (C.Alignment == 1));
              ASSERT_FALSE(HasFatalFailure());
            }
    }
  }
  void rounding(OperationKind Kind, uint64_t DAZ = 0) {
    for (const auto &C : Conversions) {
      if (C.Kind != Kind)
        continue;
      for (unsigned N = 0; N < std::size(Inputs); ++N)
        for (bool Negate : {false, true})
          for (const auto &R : Roundings)
            for (auto Sticky : {uint64_t(0), PrecisionStatus, ExistingStatus})
              for (auto Flush : {uint64_t(0), FlushToZero})
                for (bool Memory : {false, true}) {
                  SCOPED_TRACE(Inputs[N].Name);
                  SCOPED_TRACE(Negate);
                  const auto I = input(C, Inputs[(N + 1) % std::size(Inputs)],
                                       Inputs[N], Negate);
                  check(C, I, InitialMXCSR | DAZ | R.Control | Sticky | Flush,
                        0, 1, Memory,
                        Alias + PageSize -
                            (C.Alignment == 1 ? sourceBytes(C) / 2
                                              : sourceBytes(C)),
                        N % 2 ? ClearFlags : Flags | DirectionFlag);
                  ASSERT_FALSE(HasFatalFailure());
                }
    }
  }
};
#define NEVERD_PACKED_FLOAT_OPERATION(Name, ...)                               \
  TEST_P(X64PackedFloat, Name##ValuesAndStatusPreserveOtherState) {            \
    convert(OperationKind::Name);                                              \
  }                                                                            \
  TEST_P(X64PackedFloat, Name##RoundingFlushAndStickyStatus) {                 \
    rounding(OperationKind::Name);                                             \
  }
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_OPERATION

TEST_P(X64PackedFloat, PackedAlignmentFaultPrecedesMemoryAndStatus) {
  enum class Mapping { Readable, Denied, Absent };
  for (const auto &C : Conversions) {
    if (C.Alignment == 1)
      continue;
    for (auto M : {Mapping::Readable, Mapping::Denied, Mapping::Absent}) {
      resetMemory();
      ASSERT_FALSE(HasFatalFailure());
      const auto I = input(C, One, Odd24);
      initialize(C, I, InitialMXCSR, 0, 1, true, Data + 1);
      ASSERT_FALSE(HasFatalFailure());
      // Keep an aligned copy behind the other virtual alias for retry.
      llvm::cantFail(
          CPU->writeInteger(Alias + VectorBytes, I.Right[0], WordBytes));
      llvm::cantFail(CPU->writeInteger(Alias + VectorBytes + WordBytes,
                                       I.Right[1], WordBytes));
      auto Before = snapshot();
      const auto RAM = backing();
      if (M == Mapping::Denied)
        llvm::cantFail(CPU->protect(Data, PageSize, UserAccessible));
      if (M == Mapping::Absent)
        llvm::cantFail(CPU->addressSpace()->unmap(Data, PageSize));
      unsigned Observations = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Observations; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
      Hooks.RecoverableFault = [](const BackendFault &) { return true; };
      const auto Bytes = instruction(C, 0, 1, true);
      const auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
          << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Interrupt);
      EXPECT_EQ(Exit.Fault->Interrupt,
                unsigned(x64::ExceptionVector::GeneralProtection));
      EXPECT_EQ(Exit.Fault->ErrorCode, 0u);
      EXPECT_EQ(Exit.Fault->Cause, BackendFaultCause::OperandAlignment);
      EXPECT_EQ(Observations, 0u);
      expectSnapshot(Before);
      EXPECT_EQ(backing(), RAM);
      ASSERT_TRUE(CPU->takeRecoverableFault());
      llvm::cantFail(CPU->setReg(X64Register::CX, Alias + VectorBytes));
      const auto Retry = run(Bytes);
      ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
      Before[CPURegister::X64CX] = {Alias + VectorBytes, 0};
      const auto Value = reference(C, I, InitialMXCSR);
      Before[CPURegister::X64V0] = Value.Vector;
      Before[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
      Before[CPURegister::X64PC] = {Code + Bytes.size(), 0};
      expectSnapshot(Before);
      EXPECT_EQ(backing(), RAM);
    }
  }
}

TEST_P(X64PackedFloat, EveryVectorPairAndAliasedSource) {
  for (const auto &C : Conversions)
    for (unsigned Dest = 0; Dest < XmmCount; ++Dest) {
      for (unsigned Source = 0; Source < XmmCount; ++Source) {
        check(C,
              input(C, Inputs[Dest % std::size(Inputs)],
                    Inputs[(Dest + Source) % std::size(Inputs)]),
              InitialMXCSR | ExistingStatus, Dest, Source);
        ASSERT_FALSE(HasFatalFailure());
      }
      // Aliasing still converts every active original source lane; it cannot
      // bypass rounding or sticky precision status.
      for (const auto &Value : {Maximum, Odd24, Halfway24}) {
        check(C, input(C, One, Value), InitialMXCSR, Dest, Dest);
        ASSERT_FALSE(HasFatalFailure());
      }
      check(C, input(C, NegativeOne, Halfway24), InitialMXCSR, Dest, 1, true);
      ASSERT_FALSE(HasFatalFailure());
    }
}

TEST_P(X64PackedFloat, MemoryFaultsPreserveValuesAndStatusAndRetry) {
  enum class Mapping { Denied, Absent, SupervisorOnly };
  for (const auto &C : Conversions)
    for (const auto &Input : {Halfway24, Maximum})
      for (unsigned FirstBytes = C.Alignment == 1 ? 1 : sourceBytes(C);
           FirstBytes <= sourceBytes(C); ++FirstBytes)
        for (unsigned Page : {0u, 1u})
          for (auto Kind :
               {Mapping::Denied, Mapping::Absent, Mapping::SupervisorOnly}) {
            if ((Kind == Mapping::SupervisorOnly && !GetParam().User) ||
                (FirstBytes == sourceBytes(C) && Page))
              continue;
            resetMemory();
            ASSERT_FALSE(HasFatalFailure());
            const auto Width = sourceBytes(C);
            const uint64_t Address = Data + PageSize - FirstBytes;
            const uint64_t Control = InitialMXCSR | ExistingStatus;
            initialize(C, input(C, One, Input), Control, XmmCount - 1, 0, true,
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
            const auto Bytes = instruction(C, XmmCount - 1, 0, true);
            const auto Exit = run(Bytes, std::move(Hooks));
            ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
                << Exit.Diagnostic;
            ASSERT_TRUE(Exit.Fault);
            EXPECT_EQ(Exit.Fault->Address, Page ? PageAddress : Address);
            EXPECT_EQ(Exit.Fault->Size, Page ? Width - FirstBytes : FirstBytes);
            EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
            EXPECT_EQ(Exit.Fault->Kind, Kind == Mapping::Absent
                                            ? BackendFaultKind::UnmappedMemory
                                            : BackendFaultKind::Protection);
            EXPECT_EQ(Reads, 1u);
            EXPECT_EQ(Writes, 0u);
            expectSnapshot(Expected);
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
            const auto Value = reference(C, input(C, One, Input), Control);
            Expected[vectorRegister(GuestArchitecture::X64, XmmCount - 1)] =
                Value.Vector;
            Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
            Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
            expectSnapshot(Expected);
            EXPECT_EQ(backing(), RAM);
          }
}

TEST_P(X64PackedFloat, ReadObserversStopBeforeConversionAndStatus) {
  for (const auto &C : Conversions)
    for (const auto &Input : {Halfway24, Maximum})
      for (bool Fail : {false, true}) {
        resetMemory();
        ASSERT_FALSE(HasFatalFailure());
        initialize(C, input(C, One, Input), InitialMXCSR, 0, 0, true,
                   Data + (C.Alignment == 1));
        ASSERT_FALSE(HasFatalFailure());
        auto Expected = snapshot();
        const auto RAM = backing();
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t A, unsigned Size) {
          EXPECT_EQ(A, Data + (C.Alignment == 1));
          EXPECT_EQ(Size, sourceBytes(C));
          expectSnapshot(Expected);
          EXPECT_EQ(backing(true), RAM);
          ++Reads;
          if (Fail)
            throw std::runtime_error(ObserverError);
          CPU->stop();
        };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
        const auto Bytes = instruction(C, 0, 0, true);
        const auto Exit = run(Bytes, std::move(Hooks));
        EXPECT_EQ(Exit.Kind, Fail ? ExecutionExitKind::BackendFailure
                                  : ExecutionExitKind::Stopped)
            << Exit.Diagnostic;
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(Writes, 0u);
        expectSnapshot(Expected);
        EXPECT_EQ(backing(), RAM);
        if (!Fail) {
          const auto Retry = run(Bytes);
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          const auto Value = reference(C, input(C, One, Input), InitialMXCSR);
          Expected[vectorRegister(GuestArchitecture::X64, 0)] = Value.Vector;
          Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          expectSnapshot(Expected);
          EXPECT_EQ(backing(), RAM);
        }
      }
}

TEST_P(X64PackedFloat, InstructionObserversStopBeforeRegisterEffects) {
  for (const auto &C : Conversions)
    for (const auto &Input : {Halfway24, Maximum})
      for (bool Fail : {false, true}) {
        resetMemory();
        ASSERT_FALSE(HasFatalFailure());
        initialize(C, input(C, One, Input), InitialMXCSR);
        ASSERT_FALSE(HasFatalFailure());
        const auto Before = snapshot();
        const auto RAM = backing();
        const auto Bytes = instruction(C);
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
        expectSnapshot(Before);
        EXPECT_EQ(backing(), RAM);
        if (!Fail) {
          const auto Retry = run(Bytes);
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          const auto Value = reference(C, input(C, One, Input), InitialMXCSR);
          auto Expected = Before;
          Expected[vectorRegister(GuestArchitecture::X64, 0)] = Value.Vector;
          Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          expectSnapshot(Expected);
          EXPECT_EQ(backing(), RAM);
        }
      }
}

TEST_P(X64PackedFloat, PageEndInputsUseOnlyTheIntegerSourceWidth) {
  for (const auto &C : Conversions) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    const auto Width = sourceBytes(C);
    initialize(C, input(C, One, Halfway24), InitialMXCSR, 0, 0, true,
               Data + PageSize - Width);
    ASSERT_FALSE(HasFatalFailure());
    auto Expected = snapshot();
    const auto RAM = backing();
    ASSERT_EQ(
        llvm::toString(CPU->addressSpace()->unmap(Data + PageSize, PageSize)),
        "");
    const auto Bytes = instruction(C, 0, 0, true);
    const auto Exit = run(Bytes);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    const auto Value = reference(C, input(C, One, Halfway24), InitialMXCSR);
    Expected[vectorRegister(GuestArchitecture::X64, 0)] = Value.Vector;
    Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
    Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    expectSnapshot(Expected);
    EXPECT_EQ(backing(), RAM);
  }
}

TEST_P(X64PackedFloat, UnsupportedFormsRejectBeforeObservations) {
  auto Reject = [&](llvm::ArrayRef<uint8_t> Bytes) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(Conversions[0], input(Conversions[0], One, Halfway24),
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
    expectSnapshot(Before);
    EXPECT_EQ(backing(), RAM);
  };
  for (const auto &C : Conversions)
    for (bool Memory : {false, true}) {
      auto Bytes = instruction(C, 0, 0, Memory);
      Bytes.insert(Bytes.begin(), Lock);
      Reject(Bytes);
      ASSERT_FALSE(HasFatalFailure());
    }
#define NEVERD_PACKED_FLOAT_INVALID(Name, ...)                                 \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Reject(Name);                                                              \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_INVALID
}

TEST_P(X64PackedFloat, DeviceOperandsRejectBeforeCallbacks) {
  for (const auto &C : Conversions) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(C, input(C, One, Halfway24), InitialMXCSR, 0, 0, true, Stack);
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
      const auto Exit = run(instruction(C, 0, 0, true));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
          << Exit.Diagnostic;
    }
    EXPECT_EQ(Calls, 0u);
    expectSnapshot(Before);
    EXPECT_EQ(backing(), RAM);
  }
}

using X64PackedFloatDAZ = daz_test::Fixture<X64PackedFloat>;
#define NEVERD_PACKED_FLOAT_OPERATION(Name, ...)                               \
  TEST_P(X64PackedFloatDAZ, Name##ValuesAndStatusPreserveOtherState) {         \
    convert(OperationKind::Name, daz_test::DenormalsAreZero);                  \
  }                                                                            \
  TEST_P(X64PackedFloatDAZ, Name##RoundingFlushAndStickyStatus) {              \
    rounding(OperationKind::Name, daz_test::DenormalsAreZero);                 \
  }
#include "X64PackedFloatCases.def"
#undef NEVERD_PACKED_FLOAT_OPERATION
INSTANTIATE_TEST_SUITE_P(DAZBackends, X64PackedFloatDAZ,
                         testing::ValuesIn(daz_test::Parameters),
                         [](const auto &Info) { return Info.param.Name; });

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64PackedFloat,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
