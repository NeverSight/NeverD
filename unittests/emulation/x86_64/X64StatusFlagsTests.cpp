//===- X64StatusFlagsTests.cpp - Carry and implicit AH status transfers
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <cpuid.h>
#endif

namespace neverd::emulation {
namespace {
#define NEVERD_STATUS_FLAGS_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_STATUS_FLAGS_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_STATUS_FLAGS_BYTES(Name, ...)                                   \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64StatusFlagsCases.def"
#undef NEVERD_STATUS_FLAGS_BYTES
#undef NEVERD_STATUS_FLAGS_TEXT
#undef NEVERD_STATUS_FLAGS_VALUE
constexpr uint64_t FlagBits[] = {
#define NEVERD_STATUS_FLAGS_BIT(Value) Value,
#include "X64StatusFlagsCases.def"
#undef NEVERD_STATUS_FLAGS_BIT
};
enum class Operation {
#define NEVERD_STATUS_FLAGS_INSTRUCTION(Name, Opcode) Name,
#include "X64StatusFlagsCases.def"
#undef NEVERD_STATUS_FLAGS_INSTRUCTION
};
struct Instruction {
  const char *Name;
  Operation Op;
  uint8_t Opcode;
};
constexpr Instruction Instructions[] = {
#define NEVERD_STATUS_FLAGS_INSTRUCTION(Name, Opcode)                          \
  {#Name, Operation::Name, Opcode},
#include "X64StatusFlagsCases.def"
#undef NEVERD_STATUS_FLAGS_INSTRUCTION
};
struct Prefix {
  const char *Name;
  std::vector<uint8_t> Bytes;
};
const Prefix Prefixes[] = {
#define NEVERD_STATUS_FLAGS_PREFIX(Name, ...) {#Name, {__VA_ARGS__}},
#include "X64StatusFlagsCases.def"
#undef NEVERD_STATUS_FLAGS_PREFIX
};
uint64_t flags(unsigned Mask) {
  uint64_t Result = Reserved;
  for (unsigned N = 0; N < std::size(FlagBits); ++N)
    if (Mask & (1u << N))
      Result |= FlagBits[N];
  return Result;
}
struct Result {
  uint64_t AX, Flags;
};
Result expected(Operation Op, uint64_t AX, uint64_t Before) {
  switch (Op) {
  case Operation::CLC:
    return {AX, Before & ~Carry};
  case Operation::STC:
    return {AX, Before | Carry};
  case Operation::CMC:
    return {AX, Before ^ Carry};
  case Operation::LAHF:
    return {(AX & ~AHMask) | (((Before & LowStatus) | Reserved) << AHShift),
            Before};
  case Operation::SAHF:
    return {AX, (Before & ~LowStatus) | ((AX >> AHShift) & LowStatus)};
  }
  std::abort();
}

TEST(X64StatusFlagsOracle, OriginalInstructionsPreserveEveryUnselectedBit) {
#if defined(__x86_64__) || defined(_M_X64)
  const auto CPUID = [](unsigned Leaf) {
#ifdef _MSC_VER
    int Values[4];
    __cpuidex(Values, Leaf, 0);
    return std::array<unsigned, 4>{unsigned(Values[0]), unsigned(Values[1]),
                                   unsigned(Values[2]), unsigned(Values[3])};
#else
    std::array<unsigned, 4> Values{};
    __cpuid_count(Leaf, 0, Values[0], Values[1], Values[2], Values[3]);
    return Values;
#endif
  };
  if (CPUID(ExtendedCPUIDBase)[0] < ExtendedCPUIDFeatures ||
      !(CPUID(ExtendedCPUIDFeatures)[2] & LAHFSAHFFeature))
    GTEST_SKIP() << OracleFeatureUnavailable;
  for (const auto &I : Instructions)
    for (const auto &P : Prefixes) {
      SCOPED_TRACE(I.Name);
      SCOPED_TRACE(P.Name);
#ifdef _WIN32
      std::vector<uint8_t> Bytes(std::begin(Win64Argument),
                                 std::end(Win64Argument));
#else
      std::vector<uint8_t> Bytes(std::begin(SysVArgument),
                                 std::end(SysVArgument));
#endif
      Bytes.insert(Bytes.end(), std::begin(OracleBefore),
                   std::end(OracleBefore));
      Bytes.insert(Bytes.end(), P.Bytes.begin(), P.Bytes.end());
      Bytes.push_back(I.Opcode);
      Bytes.insert(Bytes.end(), std::begin(OracleAfter), std::end(OracleAfter));
      std::error_code EC;
      auto Block = llvm::sys::Memory::allocateMappedMemory(
          Page, nullptr,
          llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
      ASSERT_FALSE(bool(EC)) << EC.message();
      auto Release = llvm::scope_exit(
          [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
      std::memcpy(Block.base(), Bytes.data(), Bytes.size());
      EC = llvm::sys::Memory::protectMappedMemory(
          Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
      ASSERT_FALSE(bool(EC)) << EC.message();
      llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Bytes.size());
      auto Execute = reinterpret_cast<void (*)(uint64_t *)>(Block.base());
      for (unsigned Mask = 0; Mask <= UINT8_MAX; ++Mask)
        for (uint64_t SeedAX : {Seed, OtherSeed}) {
          // Cover all AH bytes independently of the requested status flags.
          const uint64_t AX = (SeedAX & ~AHMask) | (uint64_t(Mask) << AHShift);
          std::array<uint64_t, 5> Packet{AX, flags(Mask)};
          Execute(Packet.data());
          const auto Expected = expected(I.Op, AX, Packet[4]);
          EXPECT_EQ(Packet[2], Expected.AX);
          EXPECT_EQ(Packet[3], Expected.Flags);
        }
    }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
constexpr Parameter Parameters[] = {
#define NEVERD_STATUS_FLAGS_BACKEND(Name, Backend, Contract)                   \
  {#Name, ExecutionBackendKind::Backend, ExecutionContract::Contract},
#include "X64StatusFlagsCases.def"
#undef NEVERD_STATUS_FLAGS_BACKEND
};
class X64StatusFlags : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint8_t> RAM = std::vector<uint8_t>(Page, Fill);
  unsigned Length = 0, Reads = 0, Writes = 0;
  void SetUp() override { initialize(); }
  void initialize() {
    auto Created =
        createExecutionBackend(GetParam().Backend, GetParam().Contract, Limit);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(Created->CPU);
    llvm::cantFail(
        CPU->map(Code, Page, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, Page, Read | Write | UserAccessible));
    llvm::cantFail(CPU->write(Data, RAM));
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), Seed + R));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, MXCSR));
    llvm::cantFail(CPU->setReg(X64Register::FPCW, FPCW));
    llvm::cantFail(CPU->setReg(X64Register::FPSW, FPSW));
    for (unsigned N = 0; N < VectorCount; ++N)
      llvm::cantFail(CPU->setXmm(N, {Seed + N, OtherSeed - N}));
    for (unsigned N = 0; N < FPCount; ++N)
      llvm::cantFail(CPU->writeRegister(
          CPURegister(unsigned(CPURegister::X64FP0) + N), {Seed + N, FPHigh}));
  }
  void prepare(const Instruction &I, uint64_t AX = Seed,
               uint64_t Before = Flags, const Prefix &P = Prefixes[0]) {
    std::vector<uint8_t> Bytes(P.Bytes);
    Bytes.push_back(I.Opcode);
    Length = Bytes.size();
    Bytes.push_back(Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->setReg(X64Register::AX, AX));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Before));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    Reads = Writes = 0;
  }
  std::map<CPURegister, RegisterValue> snapshot() {
    std::map<CPURegister, RegisterValue> Result;
    for (unsigned R = unsigned(CPURegister::X64AX);
         R <= unsigned(CPURegister::X64FP7); ++R)
      if (registerMatches(CPURegister(R), GuestArchitecture::X64))
        Result[CPURegister(R)] =
            llvm::cantFail(CPU->readRegister(CPURegister(R)));
    return Result;
  }
  BackendHooks observers() {
    BackendHooks H;
    H.Read = [&](uint64_t, uint32_t) { ++Reads; };
    H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
    return H;
  }
  ExecutionExit run() {
    auto H = observers();
    H.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC == Code + Length)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(H));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void check(const Instruction &I, uint64_t AX, uint64_t Before,
             const Prefix &P = Prefixes[0]) {
    prepare(I, AX, Before, P);
    auto State = snapshot();
    const auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    const auto Result = expected(I.Op, AX, Before);
    State[CPURegister::X64AX][0] = Result.AX;
    State[CPURegister::X64FLAGS][0] = Result.Flags;
    State[CPURegister::X64PC][0] += Length;
    EXPECT_EQ(snapshot(), State);
    EXPECT_EQ(Reads, 0u);
    EXPECT_EQ(Writes, 0u);
  }
  void expectMemoryPreserved() {
    std::vector<uint8_t> After(Page);
    llvm::cantFail(CPU->snapshotBacking(Data, After));
    EXPECT_EQ(After, RAM);
  }
};

TEST_P(X64StatusFlags, CarryInstructionsPreserveAllOtherFlagsAndRegisters) {
  for (const auto &I : Instructions) {
    if (I.Op == Operation::LAHF || I.Op == Operation::SAHF)
      continue;
    SCOPED_TRACE(I.Name);
    for (unsigned Mask = 0; Mask <= UINT8_MAX; ++Mask)
      for (uint64_t AX : {Seed, OtherSeed})
        check(I, AX, flags(Mask));
  }
  expectMemoryPreserved();
}

TEST_P(X64StatusFlags, LAHFReadsOnlyStatusBitsIntoAH) {
  for (const auto &I : Instructions)
    if (I.Op == Operation::LAHF)
      for (unsigned Mask = 0; Mask <= UINT8_MAX; ++Mask)
        for (uint64_t AX : {Seed, OtherSeed})
          check(I, AX, flags(Mask));
  expectMemoryPreserved();
}

TEST_P(X64StatusFlags, SAHFRestoresOnlyDefinedStatusBits) {
  for (const auto &I : Instructions)
    if (I.Op == Operation::SAHF)
      for (unsigned AH = 0; AH <= UINT8_MAX; ++AH)
        for (unsigned Controls = 0; Controls < ControlCombinations; ++Controls)
          for (uint64_t Low : {Reserved, Reserved | LowStatus}) {
            const uint64_t Before = (uint64_t(Controls) << ControlShift) | Low;
            check(I, (Seed & ~AHMask) | (uint64_t(AH) << AHShift), Before);
          }
  expectMemoryPreserved();
}

TEST_P(X64StatusFlags, IgnoredPrefixesKeepImplicitAHAndCarry) {
  for (const auto &I : Instructions)
    for (const auto &P : Prefixes) {
      SCOPED_TRACE(I.Name);
      SCOPED_TRACE(P.Name);
      for (uint64_t AX : {Seed, OtherSeed})
        for (uint64_t Before : {Reserved, Flags})
          check(I, AX, Before, P);
    }
  expectMemoryPreserved();
}

TEST_P(X64StatusFlags, InstructionStopRestoresContextAndNativeContinuation) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    prepare(I);
    llvm::cantFail(CPU->write(Code + Length, Continuation));
    llvm::cantFail(CPU->setReg(X64Register::CX, Seed));
    llvm::cantFail(CPU->setReg(X64Register::DX, Data));
    // These implicit flag instructions must never access the guest stack.
    llvm::cantFail(CPU->setReg(X64Register::SP, Noncanonical));
    const auto Before = snapshot();
    auto Saved = llvm::cantFail(CPU->saveContext());
    auto H = observers();
    H.Instruction = [&](uint64_t PC, uint32_t Size) {
      EXPECT_EQ(PC, Code);
      EXPECT_EQ(Size, Length);
      CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(H));
    EXPECT_EQ(llvm::cantFail(CPU->runUntilExit(Code, Timeout)).Kind,
              ExecutionExitKind::Stopped);
    EXPECT_EQ(snapshot(), Before);
    for (bool Restore : {false, true}) {
      if (Restore)
        llvm::cantFail(CPU->restoreContext(*Saved));
      std::vector<uint64_t> PCs;
      H.Instruction = [&](uint64_t PC, uint32_t) {
        PCs.push_back(PC);
        if (PC == Code + Length + ADCBytes + StoreBytes)
          CPU->stop();
      };
      llvm::cantFail(CPU->installHooks(H));
      const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(PCs, (std::vector<uint64_t>{
                         Code, Code + Length, Code + Length + ADCBytes,
                         Code + Length + ADCBytes + StoreBytes}));
      const auto Result = expected(I.Op, Seed, Flags);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Result.AX);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, sizeof(uint64_t))),
                Seed + (Result.Flags & Carry));
      EXPECT_EQ(Reads, 0u);
    }
    EXPECT_EQ(Writes, 2u);
    llvm::cantFail(CPU->write(Data, RAM));
  }
}

TEST_P(X64StatusFlags, InstructionObserverFailurePreservesCompleteState) {
  for (const auto &I : Instructions) {
    // A terminal backend failure cannot be reused as a fresh execution.
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    prepare(I);
    const auto Before = snapshot();
    auto H = observers();
    H.Instruction = [](uint64_t, uint32_t) {
      throw std::runtime_error(ObserverFailure);
    };
    llvm::cantFail(CPU->installHooks(H));
    const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure) << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(Reads, 0u);
    EXPECT_EQ(Writes, 0u);
    expectMemoryPreserved();
  }
}

TEST_P(X64StatusFlags, LockedFormsRejectWithoutEffects) {
  const Prefix Locked{nullptr, {uint8_t(Lock)}};
  for (const auto &I : Instructions) {
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    prepare(I, Seed, Flags, Locked);
    const auto Before = snapshot();
    const auto Exit = run();
    const bool Portable = GetParam().Backend == ExecutionBackendKind::Unicorn &&
                          GetParam().Contract == ExecutionContract::Legacy;
    EXPECT_EQ(Exit.Kind, Portable ? ExecutionExitKind::GuestFault
                                  : ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->PC, Code);
    if (Portable)
      EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::InvalidInstruction);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(Reads, 0u);
    EXPECT_EQ(Writes, 0u);
    expectMemoryPreserved();
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64StatusFlags,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
