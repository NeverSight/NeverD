//===- X64SSEPredicateTests.cpp - Legacy SSE predicate masks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64DAZTestSupport.h"
#include "arch/x86_64/X64Exception.h"
#include "arch/x86_64/X64Machine.h"
#include "core/ExecutionDiagnostics.h"

#include "capstone/capstone.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <climits>
#include <cstring>
#include <stdexcept>

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_SSE_COMPARE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SSE_COMPARE_TEXT(Name, Value) constexpr char Name[] = Value;
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_TEXT
#undef NEVERD_SSE_COMPARE_VALUE
#define NEVERD_SSE_PREDICATE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SSE_PREDICATE_INVALID(Name, ...)                                \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_INVALID
#undef NEVERD_SSE_PREDICATE_VALUE
enum class OperationKind {
#define NEVERD_SSE_PREDICATE_OPERATION(Name, ...) Name,
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_OPERATION
};
struct Operation {
  const char *Name;
  OperationKind Kind;
  uint8_t Prefix;
  unsigned Bits, Lanes, Alignment;
  unsigned DecoderID;
};
constexpr Operation Operations[] = {
#define NEVERD_SSE_PREDICATE_OPERATION(Name, Prefix, Bits, Lanes, Align)       \
  {#Name, OperationKind::Name, Prefix, Bits, Lanes, Align, X86_INS_##Name},
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_OPERATION
};
struct Predicate {
  const char *Name;
  uint8_t Immediate;
  bool Equal, Less, Greater, Unordered, SignalsQuietNaN;
};
constexpr Predicate Predicates[] = {
#define NEVERD_SSE_PREDICATE_CONDITION(Name, Imm, Eq, Lt, Gt, Unord, Signal)   \
  {#Name, Imm, Eq, Lt, Gt, Unord, Signal},
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_CONDITION
};
struct Comparison {
  Operation Op;
  Predicate P;
};
constexpr auto Comparisons = [] {
  std::array<Comparison, std::size(Operations) * std::size(Predicates)>
      Result{};
  unsigned N = 0;
  for (auto Op : Operations)
    for (auto P : Predicates)
      Result[N++] = {Op, P};
  return Result;
}();
// Splitting a matrix must neither omit nor repeat an opcode/predicate pair.
constexpr std::pair<OperationKind, unsigned> ValueCases[] = {
#define NEVERD_SSE_PREDICATE_TEST(Operation, Condition, Immediate)             \
  {OperationKind::Operation, Immediate},
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_TEST
};
static_assert(std::size(ValueCases) == std::size(Comparisons));
static_assert([] {
  for (const auto &C : Comparisons) {
    unsigned Matches = 0;
    for (const auto &[Kind, Immediate] : ValueCases)
      Matches += Kind == C.Op.Kind && Immediate == C.P.Immediate;
    if (Matches != 1)
      return false;
  }
  return true;
}());
constexpr uint64_t Roundings[] = {
#define NEVERD_SSE_COMPARE_ROUND(Name, Bits) Bits,
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_ROUND
};
struct NumericInput {
  const char *Name;
  uint64_t Single, Double;
};
#define NEVERD_SSE_COMPARE_INPUT(Name, Single, Double)                         \
  constexpr NumericInput Name = {#Name, Single, Double};
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_INPUT
constexpr NumericInput Inputs[] = {
#define NEVERD_SSE_COMPARE_INPUT(Name, Single, Double) Name,
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_INPUT
};
unsigned sourceBytes(const Comparison &C) {
  return C.Op.Bits * C.Op.Lanes / CHAR_BIT;
}
RegisterValue
vector(const Comparison &C,
       const std::array<NumericInput, VectorBytes / sizeof(uint32_t)> &Lanes) {
  llvm::APInt Result(VectorBytes * CHAR_BIT, 0);
  for (unsigned N = 0; N < VectorBytes * CHAR_BIT / C.Op.Bits; ++N)
    Result.insertBits(llvm::APInt(C.Op.Bits, C.Op.Bits == SingleBits
                                                 ? Lanes[N].Single
                                                 : Lanes[N].Double),
                      N * C.Op.Bits);
  return {Result.getRawData()[0], Result.getRawData()[1]};
}
Input input(const Comparison &C, const NumericInput &Left,
            const NumericInput &Right) {
  return {vector(C, {Left, Left, Left, Left}),
          vector(C, {Right, Right, Right, Right})};
}
std::vector<Input> mixedInputs(const Comparison &C) {
  std::vector<Input> Result;
  for (auto NaN : {QuietNaN, SignalingNaN}) {
    std::array Left{One, NaN, MinSubnormal, NegativeZero};
    std::array Right{AboveOne, MinSubnormal, One, Zero};
    for (unsigned N = 0; N < Left.size(); ++N) {
      Result.push_back({vector(C, Left), vector(C, Right)});
      std::rotate(Left.begin(), Left.begin() + 1, Left.end());
      std::rotate(Right.begin(), Right.begin() + 1, Right.end());
    }
  }
  return Result;
}
std::vector<Input> allInputs(const Comparison &C) {
  auto Result = mixedInputs(C);
  for (const auto &Left : Inputs)
    for (const auto &Right : Inputs)
      Result.push_back(input(C, Left, Right));
  return Result;
}
struct Result {
  RegisterValue Vector;
  uint64_t MXCSR;
};
Result reference(const Comparison &C, const Input &I, uint64_t Control) {
  const auto &Format = C.Op.Bits == SingleBits ? llvm::APFloat::IEEEsingle()
                                               : llvm::APFloat::IEEEdouble();
  const llvm::APInt A(VectorBytes * CHAR_BIT, llvm::ArrayRef(I.Left));
  const llvm::APInt B(VectorBytes * CHAR_BIT, llvm::ArrayRef(I.Right));
  auto Value = A;
  uint64_t Status = 0;
  for (unsigned Lane = 0; Lane < C.Op.Lanes; ++Lane) {
    const auto Left = daz_test::operand(
        Format, A.extractBits(C.Op.Bits, Lane * C.Op.Bits), Control);
    const auto Right = daz_test::operand(
        Format, B.extractBits(C.Op.Bits, Lane * C.Op.Bits), Control);
    bool Match = false;
    switch (Left.compare(Right)) {
    case llvm::APFloat::cmpEqual:
      Match = C.P.Equal;
      break;
    case llvm::APFloat::cmpLessThan:
      Match = C.P.Less;
      break;
    case llvm::APFloat::cmpGreaterThan:
      Match = C.P.Greater;
      break;
    case llvm::APFloat::cmpUnordered:
      Match = C.P.Unordered;
      break;
    }
    Value.insertBits(Match ? llvm::APInt::getAllOnes(C.Op.Bits)
                           : llvm::APInt(C.Op.Bits, 0),
                     Lane * C.Op.Bits);
    // NaN priority applies within each lane; independent lanes accumulate
    // their masked invalid and denormal-operand status together.
    if (Left.isNaN() || Right.isNaN()) {
      if (C.P.SignalsQuietNaN || Left.isSignaling() || Right.isSignaling())
        Status |= InvalidStatus;
    } else if (Left.isDenormal() || Right.isDenormal())
      Status |= DenormalStatus;
  }
  return {{Value.getRawData()[0], Value.getRawData()[1]}, Control | Status};
}
std::vector<uint8_t> instruction(const Comparison &C, unsigned Dest = 0,
                                 unsigned Source = 1, bool Memory = false) {
  std::vector<uint8_t> Bytes;
  if (C.Op.Prefix)
    Bytes.push_back(C.Op.Prefix);
  const uint8_t Extension = (Dest > RegisterMask ? RexDestination : 0) |
                            (!Memory && Source > RegisterMask ? RexSource : 0);
  if (Extension)
    Bytes.push_back(Rex | Extension);
  Bytes.push_back(OpcodeMap);
  Bytes.push_back(CompareOpcode);
  Bytes.push_back(
      ((Dest & RegisterMask) << RegisterShift) |
      (Memory ? MemoryModRM : RegisterModRM | (Source & RegisterMask)));
  Bytes.push_back(C.P.Immediate);
  return Bytes;
}

TEST(X64SSEPredicateDecoder, FamilyAndPredicateAreIndependentOfSyntax) {
  for (auto Mode : {CS_MODE_32, CS_MODE_64})
    for (auto Syntax : {CS_OPT_SYNTAX_INTEL, CS_OPT_SYNTAX_ATT}) {
      csh Decoder;
      ASSERT_EQ(cs_open(CS_ARCH_X86, Mode, &Decoder), CS_ERR_OK);
      auto Close = llvm::scope_exit([&] { cs_close(&Decoder); });
      ASSERT_EQ(cs_option(Decoder, CS_OPT_DETAIL, CS_OPT_ON), CS_ERR_OK);
      ASSERT_EQ(cs_option(Decoder, CS_OPT_SYNTAX, Syntax), CS_ERR_OK);
      for (const auto &Op : Operations)
        for (bool Memory : {false, true})
          for (bool Iterative : {false, true})
            for (unsigned Control = 0; Control <= UINT8_MAX; ++Control) {
              SCOPED_TRACE(Op.Name);
              SCOPED_TRACE(Control);
              SCOPED_TRACE(Memory);
              auto Bytes = instruction(
                  {Op, Predicates[0]},
                  Mode == CS_MODE_64 ? XmmCount - 1 : RegisterMask,
                  Mode == CS_MODE_64 ? XmmCount - 2 : RegisterMask - 1, Memory);
              Bytes.back() = Control;
              cs_insn *Decoded = nullptr;
              if (Iterative) {
                Decoded = cs_malloc(Decoder);
                ASSERT_NE(Decoded, nullptr);
                const uint8_t *Cursor = Bytes.data();
                size_t Remaining = Bytes.size();
                uint64_t PC = Code;
                ASSERT_TRUE(
                    cs_disasm_iter(Decoder, &Cursor, &Remaining, &PC, Decoded));
                EXPECT_EQ(Remaining, 0u);
                EXPECT_EQ(PC, Code + Bytes.size());
              } else {
                ASSERT_EQ(cs_disasm(Decoder, Bytes.data(), Bytes.size(), Code,
                                    1, &Decoded),
                          1u);
              }
              auto Free = llvm::scope_exit([&] { cs_free(Decoded, 1); });
              EXPECT_EQ(Decoded->id, Op.DecoderID);
              const auto &X = Decoded->detail->x86;
              EXPECT_EQ(X.op_count, Control < FirstReservedControl ? 2 : 3);
              EXPECT_EQ(X.sse_cc, Control < FirstReservedControl
                                      ? X86_SSE_CC_EQ + Control
                                      : X86_SSE_CC_INVALID);
              EXPECT_EQ(X.avx_cc, X86_AVX_CC_INVALID);
              const unsigned Source =
                  Syntax == CS_OPT_SYNTAX_ATT
                      ? (Control < FirstReservedControl ? 0 : 1)
                      : 1;
              EXPECT_EQ(X.operands[Source].type,
                        Memory ? X86_OP_MEM : X86_OP_REG);
              EXPECT_EQ(X.operands[Source].size,
                        Memory ? Op.Bits * Op.Lanes / CHAR_BIT : VectorBytes);
            }
    }
}

void nativeOracle(OperationKind Kind, uint64_t DAZ = 0) {
#if defined(__x86_64__) || defined(_M_X64)
  const auto Mask = daz_test::hostMXCSRMask();
  if (DAZ && !(Mask & DAZ))
    GTEST_SKIP() << daz_test::HostUnavailable;
#define NEVERD_FP_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FPCases.def"
#undef NEVERD_FP_BYTES
#define NEVERD_SSE_COMPARE_BYTES(Name, ...)                                    \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_BYTES
#define NEVERD_SSE_PREDICATE_BYTES(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_BYTES
  for (const auto &C : Comparisons) {
    if (C.Op.Kind != Kind)
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
      Bytes.insert(Bytes.end(), std::begin(BeforePredicate),
                   std::end(BeforePredicate));
      const auto Compare = instruction(C, 0, 1, Memory);
      Bytes.insert(Bytes.end(), Compare.begin(), Compare.end());
      Bytes.insert(Bytes.end(), std::begin(AfterComparison),
                   std::end(AfterComparison));
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
      for (const auto &I : allInputs(C))
        for (auto Rounding : Roundings)
          for (auto InitialFlags : {ClearFlags, Flags, Flags | DirectionFlag})
            for (auto Sticky :
                 {uint64_t(0), ExistingStatus,
                  ExistingStatus | InvalidStatus | DenormalStatus})
              for (auto Flush : {uint64_t(0), FlushToZero}) {
                SCOPED_TRACE(C.Op.Name);
                SCOPED_TRACE(C.P.Name);
                SCOPED_TRACE(Memory);
                SCOPED_TRACE(testing::PrintToString(I.Left));
                SCOPED_TRACE(testing::PrintToString(I.Right));
                SCOPED_TRACE(InitialFlags);
                SCOPED_TRACE(Rounding);
                X64MachineState Seed;
                for (unsigned N = 0; N < XmmCount; ++N)
                  Seed.Xmm[N] = {SentinelLow + N, SentinelHigh - N};
                Seed.Xmm[0] = I.Left;
                Seed.Xmm[1] = I.Right;
                Seed.MXCSR = InitialMXCSR | DAZ | Rounding | Sticky | Flush;
                alignas(x64::fp::RegisterSlotBytes)
                    std::array<uint8_t, x64::fp::LegacyBytes>
                        Before{}, After{}, Host{};
                ASSERT_EQ(llvm::toString(encodeX64FXState(Seed, Before, Mask)),
                          "");
                struct alignas(VectorBytes) ArgumentsType {
                  RegisterValue Right;
                  uint64_t Flags;
                };
                const ArgumentsType Arguments{I.Right, InitialFlags};
                const auto ActualFlags = Execute(Before.data(), After.data(),
                                                 Host.data(), &Arguments);
                X64MachineState Actual;
                ASSERT_EQ(llvm::toString(decodeX64FXState(Actual, After)), "");
                const auto Expected = reference(C, I, Seed.MXCSR);
                EXPECT_EQ(ActualFlags, InitialFlags);
                Seed.Xmm[0] = Expected.Vector;
                EXPECT_EQ(Actual.Xmm, Seed.Xmm);
                EXPECT_EQ(Actual.MXCSR, Expected.MXCSR);
              }
    }
  }
#else
  GTEST_SKIP();
#endif
}

#define NEVERD_SSE_PREDICATE_OPERATION(Name, ...)                              \
  TEST(X64SSEPredicateOracle, Name##MasksAndStatusMatchOriginalInstructions) { \
    nativeOracle(OperationKind::Name);                                         \
  }
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_OPERATION
#define NEVERD_SSE_PREDICATE_OPERATION(Name, ...)                              \
  TEST(X64SSEPredicateDAZOracle,                                               \
       Name##MasksAndStatusMatchOriginalInstructions) {                        \
    nativeOracle(OperationKind::Name, daz_test::DenormalsAreZero);             \
  }
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_OPERATION

class X64SSEPredicate : public X64VectorTest {
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
  void initialize(const Comparison &C, const Input &I, uint64_t Control,
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
      EXPECT_EQ(Actual.at(R), Value);
    }
  }
  void check(const Comparison &C, Input I, uint64_t Control, unsigned Dest = 0,
             unsigned Source = 1, bool Memory = false, uint64_t Address = Data,
             uint64_t InitialFlags = Flags) {
    SCOPED_TRACE(C.Op.Name);
    SCOPED_TRACE(C.P.Name);
    SCOPED_TRACE(Dest);
    SCOPED_TRACE(Source);
    SCOPED_TRACE(Memory);
    SCOPED_TRACE(Control);
    SCOPED_TRACE(InitialFlags);
    if (!Memory && Dest == Source)
      I.Right = I.Left;
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
  void compare(OperationKind Kind, unsigned Immediate, uint64_t DAZ = 0) {
    unsigned Matched = 0;
    for (const auto &C : Comparisons) {
      if (C.Op.Kind != Kind || C.P.Immediate != Immediate)
        continue;
      ++Matched;
      for (const auto &Left : Inputs)
        for (const auto &Right : Inputs)
          // Exhaust all numeric pairs here; the separate control matrix
          // covers all rounding modes, sticky masks and FTZ settings.
          for (auto Sticky : {uint64_t(0), ExistingStatus}) {
            SCOPED_TRACE(Left.Name);
            SCOPED_TRACE(Right.Name);
            const auto I = input(C, Left, Right);
            const auto Control = InitialMXCSR | DAZ | Sticky;
            check(C, I, Control);
            ASSERT_FALSE(HasFatalFailure());
            check(C, I, Control, 0, 1, true, Alias + (C.Op.Alignment == 1));
            ASSERT_FALSE(HasFatalFailure());
            check(C, I, Control, 0, 1, true,
                  Alias + PageSize -
                      (C.Op.Alignment == 1 ? sourceBytes(C) / 2
                                           : sourceBytes(C)));
            ASSERT_FALSE(HasFatalFailure());
          }
    }
    EXPECT_EQ(Matched, 1u);
  }
  void rounding(OperationKind Kind, uint64_t DAZ = 0) {
    const std::pair<NumericInput, NumericInput> Pairs[] = {
#define NEVERD_SSE_COMPARE_PAIR(Left, Right) {Left, Right},
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_PAIR
    };
    unsigned Matched = 0;
    for (const auto &C : Comparisons) {
      if (C.Op.Kind != Kind)
        continue;
      ++Matched;
      for (const auto &[Left, Right] : Pairs)
        for (auto Rounding : Roundings)
          for (auto InitialFlags : {ClearFlags, Flags, Flags | DirectionFlag})
            for (auto Sticky :
                 {uint64_t(0), ExistingStatus,
                  ExistingStatus | InvalidStatus | DenormalStatus})
              for (auto Flush : {uint64_t(0), FlushToZero})
                for (bool Memory : {false, true}) {
                  check(C, input(C, Left, Right),
                        InitialMXCSR | DAZ | Rounding | Sticky | Flush, 0, 1,
                        Memory, Data, InitialFlags);
                  ASSERT_FALSE(HasFatalFailure());
                }
    }
    EXPECT_EQ(Matched, std::size(Predicates));
  }
  void mixed(OperationKind Kind, uint64_t DAZ = 0) {
    for (const auto &C : Comparisons) {
      if (C.Op.Kind != Kind)
        continue;
      for (const auto &I : mixedInputs(C))
        for (auto Rounding : Roundings)
          for (auto Sticky : {uint64_t(0), ExistingStatus})
            for (auto Flush : {uint64_t(0), FlushToZero})
              for (bool Memory : {false, true}) {
                check(C, I, InitialMXCSR | DAZ | Rounding | Sticky | Flush, 0,
                      1, Memory);
                ASSERT_FALSE(HasFatalFailure());
              }
    }
  }
};
#define NEVERD_SSE_PREDICATE_TEST(Operation, Condition, Immediate)             \
  TEST_P(X64SSEPredicate,                                                      \
         Operation##Condition##MasksAndStatusPreserveOtherState) {             \
    compare(OperationKind::Operation, Immediate);                              \
  }
#define NEVERD_SSE_PREDICATE_OPERATION(Name, ...)                              \
  TEST_P(X64SSEPredicate,                                                      \
         Name##RoundingModesAndUnrelatedFlagsRemainIndependent) {              \
    rounding(OperationKind::Name);                                             \
  }
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_OPERATION
#undef NEVERD_SSE_PREDICATE_TEST

TEST_P(X64SSEPredicate, MixedLanesAccumulateStatusAndScalarsIgnoreUpperInputs) {
  for (const auto &Op : Operations)
    mixed(Op.Kind);
}

TEST_P(X64SSEPredicate, PackedAlignmentFaultPrecedesMemoryAndStatus) {
  enum class Mapping { Readable, Denied, Absent };
  for (const auto &C : Comparisons) {
    if (C.Op.Alignment == 1)
      continue;
    for (auto M : {Mapping::Readable, Mapping::Denied, Mapping::Absent}) {
      resetMemory();
      ASSERT_FALSE(HasFatalFailure());
      const auto I = input(C, One, SignalingNaN);
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

TEST_P(X64SSEPredicate, EveryVectorPairAndAliasedSource) {
  for (const auto &C : Comparisons)
    for (unsigned Dest = 0; Dest < XmmCount; ++Dest) {
      for (unsigned Source = 0; Source < XmmCount; ++Source) {
        check(C,
              input(C, Inputs[Dest % std::size(Inputs)],
                    Inputs[(Dest + Source) % std::size(Inputs)]),
              InitialMXCSR | ExistingStatus, Dest, Source);
        ASSERT_FALSE(HasFatalFailure());
      }
      // An aliased source is still unordered for NaNs and still reports a
      // denormal operand; identical register numbers do not imply equality.
      for (const auto &Value : {QuietNaN, SignalingNaN, MinSubnormal}) {
        check(C, input(C, Value, One), InitialMXCSR, Dest, Dest);
        ASSERT_FALSE(HasFatalFailure());
      }
      check(C, input(C, NegativeOne, MinSubnormal), InitialMXCSR, Dest, 1,
            true);
      ASSERT_FALSE(HasFatalFailure());
    }
}

TEST_P(X64SSEPredicate, MemoryFaultsPreserveMasksAndStatusAndRetry) {
  enum class Mapping { Denied, Absent, SupervisorOnly };
  for (const auto &C : Comparisons)
    for (const auto &Input : {MinSubnormal, QuietNaN})
      for (unsigned Page : {0u, 1u})
        for (auto Kind :
             {Mapping::Denied, Mapping::Absent, Mapping::SupervisorOnly}) {
          if ((Kind == Mapping::SupervisorOnly && !GetParam().User) ||
              (C.Op.Alignment != 1 && Page))
            continue;
          resetMemory();
          ASSERT_FALSE(HasFatalFailure());
          const auto Width = sourceBytes(C);
          const uint64_t Address =
              Data + PageSize - (C.Op.Alignment == 1 ? Width / 2 : Width);
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
          EXPECT_EQ(Exit.Fault->Size, C.Op.Alignment == 1 ? Width / 2 : Width);
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
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          const auto Value = reference(C, input(C, One, Input), Control);
          Expected[vectorRegister(GuestArchitecture::X64, XmmCount - 1)] =
              Value.Vector;
          Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          expectSnapshot(Expected);
          EXPECT_EQ(backing(), RAM);
        }
}

TEST_P(X64SSEPredicate, ReadObserversStopBeforeComparisonAndStatus) {
  for (const auto &C : Comparisons)
    for (const auto &Input : {MinSubnormal, QuietNaN})
      for (bool Fail : {false, true}) {
        resetMemory();
        ASSERT_FALSE(HasFatalFailure());
        initialize(C, input(C, One, Input), InitialMXCSR, 0, 0, true,
                   Data + (C.Op.Alignment == 1));
        ASSERT_FALSE(HasFatalFailure());
        auto Expected = snapshot();
        const auto RAM = backing();
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t A, unsigned Size) {
          EXPECT_EQ(A, Data + (C.Op.Alignment == 1));
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

TEST_P(X64SSEPredicate, InstructionObserversStopBeforeRegisterEffects) {
  for (const auto &C : Comparisons)
    for (const auto &Input : {MinSubnormal, QuietNaN})
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

TEST_P(X64SSEPredicate, PageEndInputsUseOnlyTheFloatingSourceWidth) {
  for (const auto &C : Comparisons) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    const auto Width = sourceBytes(C);
    initialize(C, input(C, One, MinSubnormal), InitialMXCSR, 0, 0, true,
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
    const auto Value = reference(C, input(C, One, MinSubnormal), InitialMXCSR);
    Expected[vectorRegister(GuestArchitecture::X64, 0)] = Value.Vector;
    Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
    Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    expectSnapshot(Expected);
    EXPECT_EQ(backing(), RAM);
  }
}

TEST_P(X64SSEPredicate, UnsupportedFormsRejectBeforeObservations) {
  auto Reject = [&](llvm::ArrayRef<uint8_t> Bytes) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(Comparisons[0], input(Comparisons[0], One, MinSubnormal),
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
  for (const auto &C : Comparisons)
    for (bool Memory : {false, true}) {
      auto Bytes = instruction(C, 0, 0, Memory);
      Bytes.insert(Bytes.begin(), Lock);
      Reject(Bytes);
      ASSERT_FALSE(HasFatalFailure());
    }
  constexpr uint8_t Reserved[] = {
#define NEVERD_SSE_PREDICATE_RESERVED(Value) Value,
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_RESERVED
  };
  for (const auto &Op : Operations)
    for (bool Memory : {false, true})
      for (auto Control : Reserved) {
        auto Bytes = instruction({Op, Predicates[0]}, 0, 1, Memory);
        Bytes.back() = Control;
        Reject(Bytes);
        ASSERT_FALSE(HasFatalFailure());
      }
#define NEVERD_SSE_PREDICATE_INVALID(Name, ...)                                \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Reject(Name);                                                              \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_INVALID
}

TEST_P(X64SSEPredicate, DeviceOperandsRejectBeforeCallbacks) {
  for (const auto &C : Comparisons) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(C, input(C, One, MinSubnormal), InitialMXCSR, 0, 0, true, Stack);
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

using X64SSEPredicateDAZ = daz_test::Fixture<X64SSEPredicate>;
#define NEVERD_SSE_PREDICATE_TEST(Operation, Condition, Immediate)             \
  TEST_P(X64SSEPredicateDAZ,                                                   \
         Operation##Condition##MasksAndStatusPreserveOtherState) {             \
    compare(OperationKind::Operation, Immediate, daz_test::DenormalsAreZero);  \
  }
#define NEVERD_SSE_PREDICATE_OPERATION(Name, ...)                              \
  TEST_P(X64SSEPredicateDAZ,                                                   \
         Name##RoundingModesAndUnrelatedFlagsRemainIndependent) {              \
    rounding(OperationKind::Name, daz_test::DenormalsAreZero);                 \
  }                                                                            \
  TEST_P(X64SSEPredicateDAZ, Name##MixedLanesAccumulateStatus) {               \
    mixed(OperationKind::Name, daz_test::DenormalsAreZero);                    \
  }
#include "X64SSEPredicateCases.def"
#undef NEVERD_SSE_PREDICATE_OPERATION
#undef NEVERD_SSE_PREDICATE_TEST
INSTANTIATE_TEST_SUITE_P(DAZBackends, X64SSEPredicateDAZ,
                         testing::ValuesIn(daz_test::Parameters),
                         [](const auto &Info) { return Info.param.Name; });

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64SSEPredicate,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
