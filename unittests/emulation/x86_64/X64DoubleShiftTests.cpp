//===- X64DoubleShiftTests.cpp - Original scalar double-shift execution
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <array>
#include <bit>
#include <climits>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_DOUBLE_SHIFT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_DOUBLE_SHIFT_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_DOUBLE_SHIFT_BYTES(Name, ...)                                   \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64DoubleShiftCases.def"
#undef NEVERD_DOUBLE_SHIFT_BYTES
#undef NEVERD_DOUBLE_SHIFT_TEXT
#undef NEVERD_DOUBLE_SHIFT_VALUE
constexpr unsigned Counts[] = {
#define NEVERD_DOUBLE_SHIFT_COUNT(Value) Value,
#include "X64DoubleShiftCases.def"
#undef NEVERD_DOUBLE_SHIFT_COUNT
};
struct Shift {
  const char *Name;
  unsigned Size;
  bool Left;
  uint8_t Opcode;
};
constexpr Shift Shifts[] = {
#define NEVERD_DOUBLE_SHIFT_CASE(Name, Size, Left, Opcode)                     \
  {#Name, Size, Left, Opcode},
#include "X64DoubleShiftCases.def"
#undef NEVERD_DOUBLE_SHIFT_CASE
};
uint64_t widthMask(const Shift &S) {
  return UINT64_MAX >> ((WordBytes - S.Size) * CHAR_BIT);
}
unsigned count(const Shift &S, unsigned Raw) {
  return Raw % ((S.Size == WordBytes ? WordBytes : DWordBytes) * CHAR_BIT);
}
bool defined(const Shift &S, unsigned Raw) {
  return count(S, Raw) <= S.Size * CHAR_BIT;
}
uint64_t definedFlags(const Shift &S, unsigned Raw) {
  const auto N = count(S, Raw);
  return N ? ~(Auxiliary | (N == 1 ? 0 : Overflow)) : UINT64_MAX;
}
struct Result {
  uint64_t Value, Flags;
};
Result expected(const Shift &S, uint64_t Destination, uint64_t Input,
                unsigned Raw, uint64_t Flags) {
  const unsigned N = count(S, Raw), Bits = S.Size * CHAR_BIT;
  const auto Mask = widthMask(S);
  const uint64_t Low = Destination & Mask, Src = Input & Mask;
  uint64_t Value = Low;
  if (N) {
    Value = N == Bits ? Src
            : S.Left  ? ((Low << N) | (Src >> (Bits - N))) & Mask
                      : (Low >> N) | ((Src << (Bits - N)) & Mask);
    const bool CarryOut = (Low >> (S.Left ? Bits - N : N - 1)) & 1;
    const bool SignBit = (Value >> (Bits - 1)) & 1;
    const bool SignChanged = ((Low ^ Value) >> (Bits - 1)) & 1;
    Flags &= ~(Carry | Parity | Auxiliary | Zero | Sign | Overflow);
    Flags |= (CarryOut ? Carry : 0) | (SignBit ? Sign : 0) |
             (!Value ? Zero : 0) |
             (std::popcount(uint8_t(Value)) % 2 == 0 ? Parity : 0) |
             (N == 1 && SignChanged ? Overflow : 0);
  }
  if (S.Size == HalfWordBytes)
    Value |= Destination & ~Mask;
  return {Value, Flags};
}
std::vector<uint8_t> encoding(const Shift &S, unsigned Raw, bool CL,
                              bool Memory = false) {
  std::vector<uint8_t> Bytes;
  if (S.Size == HalfWordBytes)
    Bytes.push_back(Operand16);
  if (S.Size == WordBytes)
    Bytes.push_back(Operand64);
  Bytes.push_back(Escape);
  Bytes.push_back(S.Opcode + (CL ? CLFormOffset : 0));
  Bytes.push_back(Memory ? MemoryDestination : RegisterDestination);
  if (!CL)
    Bytes.push_back(Raw);
  return Bytes;
}

TEST(X64DoubleShiftOracle, DefinedResultsMatchOriginalHostInstructions) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &S : Shifts) {
    std::vector<uint8_t> Bytes(std::begin(OraclePrefix),
                               std::end(OraclePrefix));
#ifdef _WIN32
    Bytes.insert(Bytes.end(), std::begin(Win64Argument),
                 std::end(Win64Argument));
#else
    Bytes.insert(Bytes.end(), std::begin(SysVArgument), std::end(SysVArgument));
#endif
    Bytes.insert(Bytes.end(), std::begin(OracleLoad), std::end(OracleLoad));
    const auto Instruction = encoding(S, 0, true);
    Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
    Bytes.insert(Bytes.end(), std::begin(OracleSave), std::end(OracleSave));
    std::error_code EC;
    auto Block = llvm::sys::Memory::allocateMappedMemory(
        Page, nullptr, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE,
        EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    auto Release = llvm::scope_exit(
        [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
    std::memcpy(Block.base(), Bytes.data(), Bytes.size());
    EC = llvm::sys::Memory::protectMappedMemory(
        Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Bytes.size());
    auto Execute = reinterpret_cast<void (*)(uint64_t *)>(Block.base());
    for (unsigned Raw = 0; Raw <= UINT8_MAX; ++Raw)
      if (defined(S, Raw))
        for (uint64_t Input : {Seed, Source})
          for (uint64_t Before : {Reserved, Flags}) {
            SCOPED_TRACE(S.Name);
            SCOPED_TRACE(Raw);
            std::array<uint64_t, 6> Packet{Input, Source, Raw, Before};
            Execute(Packet.data());
            const auto E = expected(S, Input, Source, Raw, Packet[3]);
            EXPECT_EQ(Packet[4], E.Value);
            EXPECT_EQ(Packet[5] & definedFlags(S, Raw),
                      E.Flags & definedFlags(S, Raw));
          }
  }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
  ExecutionContract Contract;
};
constexpr Backend Backends[] = {
#define NEVERD_DOUBLE_SHIFT_BACKEND(Name, Kind, Contract)                      \
  {#Name, ExecutionBackendKind::Kind, ExecutionContract::Contract},
#include "X64DoubleShiftCases.def"
#undef NEVERD_DOUBLE_SHIFT_BACKEND
};
struct Parameter {
  Backend B;
  Shift S;
  std::string name() const { return std::string(B.Name) + '_' + S.Name; }
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.name(); }
std::vector<Parameter> parameters() {
  std::vector<Parameter> Result;
  for (const auto &B : Backends)
    for (const auto &S : Shifts)
      Result.push_back({B, S});
  return Result;
}
class X64DoubleShift : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  unsigned Length = 0;
  const Shift &shift() const { return GetParam().S; }
  void SetUp() override { initialize(); }
  void initialize() {
    auto B =
        createExecutionBackend(GetParam().B.Kind, GetParam().B.Contract, Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, Page, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, Page * 2, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->mapAlias(Alias, Data, Page * 2, Read | Write | UserAccessible));
    const std::vector<uint8_t> FillBytes(Page * 2, Fill);
    llvm::cantFail(CPU->write(Data, FillBytes));
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), Seed + R));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, MXCSR));
    llvm::cantFail(CPU->setReg(X64Register::FPCW, FPCW));
    llvm::cantFail(CPU->setReg(X64Register::FPSW, FPSW));
    for (unsigned N = 0; N < VectorCount; ++N)
      llvm::cantFail(CPU->setXmm(N, {Seed + N, Source - N}));
    for (unsigned N = 0; N < FPCount; ++N)
      llvm::cantFail(CPU->writeRegister(
          CPURegister(unsigned(CPURegister::X64FP0) + N), {Seed + N, FPHigh}));
  }
  void prepare(unsigned Raw, bool CL, bool Memory = false, uint64_t AX = Seed,
               uint64_t Before = Flags) {
    auto Bytes = encoding(shift(), Raw, CL, Memory);
    Length = Bytes.size();
    Bytes.push_back(Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->setReg(X64Register::AX, AX));
    llvm::cantFail(CPU->setReg(X64Register::DX, Source));
    llvm::cantFail(
        CPU->setReg(X64Register::CX, (Seed & ~uint64_t(UINT8_MAX)) | Raw));
    llvm::cantFail(CPU->setReg(X64Register::SI, Data));
    llvm::cantFail(CPU->setReg(X64Register::DI, Data));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Before));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
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
  std::vector<uint8_t> memory() {
    std::vector<uint8_t> Bytes(Page * 2);
    if (auto E = CPU->addressSpace()->read(Alias, Bytes))
      ADD_FAILURE() << llvm::toString(std::move(E));
    return Bytes;
  }
  ExecutionExit run(BackendHooks H = {}) {
    if (!H.Instruction)
      H.Instruction = [&](uint64_t PC, uint32_t) {
        if (PC == Code + Length)
          CPU->stop();
      };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void registerCounts(bool CL) {
    const auto RAM = memory();
    for (unsigned Raw = 0; Raw <= UINT8_MAX; ++Raw)
      if (defined(shift(), Raw))
        for (uint64_t AX : {Seed, Source}) {
          SCOPED_TRACE(Raw);
          const auto BeforeFlags = Raw & 1 ? Flags : Reserved;
          prepare(Raw, CL, false, AX, BeforeFlags);
          auto State = snapshot();
          unsigned Reads = 0, Writes = 0;
          BackendHooks H;
          H.Read = [&](uint64_t, uint32_t) { ++Reads; };
          H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
          const auto Exit = run(H);
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
          const auto E = expected(shift(), AX, Source, Raw, BeforeFlags);
          auto Actual = snapshot();
          State[CPURegister::X64AX][0] = E.Value;
          State[CPURegister::X64PC][0] += Length;
          State[CPURegister::X64FLAGS][0] =
              E.Flags & definedFlags(shift(), Raw);
          Actual[CPURegister::X64FLAGS][0] &= definedFlags(shift(), Raw);
          EXPECT_EQ(Actual, State);
          EXPECT_EQ(Reads, 0u);
          EXPECT_EQ(Writes, 0u);
        }
    EXPECT_EQ(memory(), RAM);
  }
};
TEST_P(X64DoubleShift, RegisterImmediateCountsMatchDefinedSemantics) {
  registerCounts(false);
}
TEST_P(X64DoubleShift, RegisterCLCountsMatchDefinedSemantics) {
  registerCounts(true);
}

TEST_P(X64DoubleShift, AliasedCountsAndExtendedRegistersReadOriginalInputs) {
  const struct {
    const char *Name;
    X64Register Destination, Source;
    uint8_t ModRM, REX;
  } Cases[] = {
#define NEVERD_DOUBLE_SHIFT_ALIAS(Name, Destination, Source, ModRM, REX)       \
  {#Name, X64Register::Destination, X64Register::Source, ModRM, REX},
#include "X64DoubleShiftCases.def"
#undef NEVERD_DOUBLE_SHIFT_ALIAS
  };
  const auto RAM = memory();
  for (bool CL : {false, true})
    for (const auto &C : Cases) {
      SCOPED_TRACE(C.Name);
      prepare(ProbeCount, CL);
      auto Bytes = encoding(shift(), ProbeCount, CL);
      Bytes[Bytes.size() - (CL ? 1 : 2)] = C.ModRM;
      if (C.REX) {
        if (shift().Size == WordBytes)
          Bytes.front() |= C.REX;
        else
          Bytes.insert(Bytes.begin() + (shift().Size == HalfWordBytes), C.REX);
      }
      Length = Bytes.size();
      Bytes.push_back(Nop);
      llvm::cantFail(CPU->write(Code, Bytes));
      const auto Original = llvm::cantFail(CPU->reg(C.Destination));
      const auto Input = llvm::cantFail(CPU->reg(C.Source));
      auto State = snapshot();
      const auto Exit = run();
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      auto Actual = snapshot();
      const auto E = expected(shift(), Original, Input, ProbeCount, Flags);
      State[cpuRegister(C.Destination)][0] = E.Value;
      State[CPURegister::X64PC][0] += Length;
      State[CPURegister::X64FLAGS][0] =
          E.Flags & definedFlags(shift(), ProbeCount);
      Actual[CPURegister::X64FLAGS][0] &= definedFlags(shift(), ProbeCount);
      EXPECT_EQ(Actual, State);
    }
  EXPECT_EQ(memory(), RAM);
}

TEST_P(X64DoubleShift, MemoryResultsAndObserversUseExactOperandSpans) {
  for (bool CL : {false, true})
    for (unsigned Raw : Counts)
      if (defined(shift(), Raw)) {
        SCOPED_TRACE(Raw);
        prepare(Raw, CL, true);
        const auto Address = Data + Page - shift().Size / 2;
        llvm::cantFail(CPU->setReg(X64Register::SI, Address));
        llvm::cantFail(CPU->writeInteger(Address, Seed, WordBytes));
        auto State = snapshot();
        auto RAM = memory();
        const auto E = expected(shift(), Seed, Source, Raw, Flags);
        unsigned Reads = 0, Writes = 0;
        BackendHooks H;
        H.Read = [&](uint64_t A, uint32_t Size) {
          EXPECT_EQ(A, Address);
          EXPECT_EQ(Size, shift().Size);
          ++Reads;
        };
        H.Write = [&](uint64_t A, uint32_t Size, uint64_t Value) {
          EXPECT_EQ(A, Address);
          EXPECT_EQ(Size, shift().Size);
          EXPECT_EQ(Value, E.Value & widthMask(shift()));
          EXPECT_EQ(memory(), RAM);
          ++Writes;
        };
        const auto Exit = run(H);
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(Writes, 1u);
        auto Actual = snapshot();
        State[CPURegister::X64PC][0] += Length;
        State[CPURegister::X64FLAGS][0] = E.Flags & definedFlags(shift(), Raw);
        Actual[CPURegister::X64FLAGS][0] &= definedFlags(shift(), Raw);
        EXPECT_EQ(Actual, State);
        for (unsigned N = 0; N < shift().Size; ++N)
          RAM[Address - Data + N] = E.Value >> (N * CHAR_BIT);
        EXPECT_EQ(memory(), RAM);
        std::vector<uint8_t> Aliased(RAM.size());
        llvm::cantFail(CPU->snapshotBacking(Alias, Aliased));
        EXPECT_EQ(Aliased, RAM);
      }
}

TEST_P(X64DoubleShift, ReadStopsAndWriteCancellationPreserveState) {
  for (bool ReadStop : {true, false})
    for (bool Throw : {false, true}) {
      if (ReadStop && Throw)
        continue;
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      prepare(ProbeCount, true, true);
      llvm::cantFail(CPU->writeInteger(Data, Seed, WordBytes));
      const auto State = snapshot();
      const auto RAM = memory();
      unsigned Reads = 0, Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t, uint32_t) {
        ++Reads;
        if (ReadStop)
          CPU->stop();
      };
      H.Write = [&](uint64_t, uint32_t, uint64_t Value) {
        ++Writes;
        EXPECT_EQ(Value,
                  expected(shift(), Seed, Source, ProbeCount, Flags).Value &
                      widthMask(shift()));
        EXPECT_EQ(memory(), RAM);
        CPU->stop();
        if (Throw)
          throw std::runtime_error(ObserverFailure);
      };
      const auto Exit = run(H);
      EXPECT_EQ(Exit.Kind, Throw ? ExecutionExitKind::BackendFailure
                                 : ExecutionExitKind::Stopped)
          << Exit.Diagnostic;
      EXPECT_EQ(Reads, 1u);
      EXPECT_EQ(Writes, ReadStop ? 0u : 1u);
      EXPECT_EQ(snapshot(), State);
      EXPECT_EQ(memory(), RAM);
    }
}

TEST_P(X64DoubleShift, MemoryFaultsAndDeviceOperandsPreserveState) {
  enum class Mapping { ReadOnly, Absent, Device };
  for (auto M : {Mapping::ReadOnly, Mapping::Absent, Mapping::Device})
    for (unsigned Raw : {0u, unsigned(ProbeCount)}) {
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      prepare(Raw, true, true);
      unsigned Callbacks = 0;
      const bool DeviceOperand = M == Mapping::Device;
      if (DeviceOperand) {
        GuestMMIOCallbacks IO;
        IO.Validate = [&](uint64_t, uint64_t, bool) {
          ++Callbacks;
          return llvm::Error::success();
        };
        IO.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
          ++Callbacks;
          return Seed;
        };
        IO.Write = [&](uint64_t, unsigned, uint64_t) {
          ++Callbacks;
          return llvm::Error::success();
        };
        llvm::cantFail(CPU->mapMMIO(Device, Page, std::move(IO)));
        llvm::cantFail(CPU->setReg(X64Register::SI, Device));
      } else {
        if (M == Mapping::ReadOnly)
          llvm::cantFail(
              CPU->protect(Data + Page, Page, Read | UserAccessible));
        llvm::cantFail(CPU->setReg(
            X64Register::SI,
            Data + Page * (M == Mapping::ReadOnly ? 1 : 2) - shift().Size / 2));
      }
      const auto State = snapshot();
      const auto RAM = memory();
      const auto Exit = run();
      const bool ProtectedDevice =
          DeviceOperand &&
          GetParam().B.Contract == ExecutionContract::CheckedUserX64;
      EXPECT_EQ(Exit.Kind, DeviceOperand && !ProtectedDevice
                               ? ExecutionExitKind::UnsupportedOperation
                               : ExecutionExitKind::GuestFault)
          << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->PC, Code);
      if (!DeviceOperand) {
        EXPECT_EQ(Exit.Fault->Address,
                  Data + Page * (M == Mapping::ReadOnly ? 1 : 2));
        EXPECT_EQ(Exit.Fault->Access, M == Mapping::ReadOnly
                                          ? BackendAccessKind::Write
                                          : BackendAccessKind::Read);
      }
      EXPECT_EQ(Callbacks, 0u);
      EXPECT_EQ(snapshot(), State);
      EXPECT_EQ(memory(), RAM);
    }
}

TEST_P(X64DoubleShift, LockedAndUndefinedFormsRejectBeforeEffects) {
  for (bool Memory : {false, true})
    for (bool CL : {false, true})
      for (bool Locked : {false, true}) {
        if (!Locked && shift().Size != HalfWordBytes)
          continue;
        initialize();
        if (HasFatalFailure() || IsSkipped())
          return;
        const unsigned Raw = Locked ? ProbeCount : UndefinedCount;
        prepare(Raw, CL, Memory);
        if (Locked) {
          auto Bytes = encoding(shift(), Raw, CL, Memory);
          Bytes.insert(Bytes.begin(), Lock);
          Length = Bytes.size();
          llvm::cantFail(CPU->write(Code, Bytes));
        }
        const auto State = snapshot();
        const auto RAM = memory();
        unsigned Reads = 0, Writes = 0;
        BackendHooks H;
        H.Read = [&](uint64_t, uint32_t) { ++Reads; };
        H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
        const auto Exit = run(H);
        EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
            << Exit.Diagnostic;
        EXPECT_EQ(snapshot(), State);
        EXPECT_EQ(memory(), RAM);
        EXPECT_EQ(Reads, 0u);
        EXPECT_EQ(Writes, 0u);
        if (!Locked) {
          EXPECT_NE(Exit.Diagnostic.find("double-shift count 17"),
                    std::string::npos);
          EXPECT_NE(Exit.Diagnostic.find("16-bit"), std::string::npos);
          EXPECT_NE(Exit.Diagnostic.find("undefined"), std::string::npos);

          // The legacy error interface must retain the typed exit's reason.
          // A fresh CPU avoids attempting to resume the terminal rejection.
          initialize();
          if (HasFatalFailure() || IsSkipped())
            return;
          prepare(Raw, CL, Memory);
          auto Error = CPU->run(Code, Timeout);
          EXPECT_TRUE(Error.isA<UnsupportedExecutionError>());
          EXPECT_EQ(llvm::toString(std::move(Error)), Exit.Diagnostic);
          EXPECT_EQ(snapshot(), State);
          EXPECT_EQ(memory(), RAM);
        }
      }
}

TEST_P(X64DoubleShift, ContextRestoreAndNativeContinuationPreserveCarry) {
  prepare(ProbeCount, true);
  llvm::cantFail(CPU->write(Code + Length, Continuation));
  const auto State = snapshot();
  auto Saved = llvm::cantFail(CPU->saveContext());
  BackendHooks Stop;
  Stop.Instruction = [&](uint64_t PC, uint32_t Size) {
    EXPECT_EQ(PC, Code);
    EXPECT_EQ(Size, Length);
    CPU->stop();
  };
  EXPECT_EQ(run(Stop).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(snapshot(), State);
  for (bool Restore : {false, true}) {
    if (Restore)
      llvm::cantFail(CPU->restoreContext(*Saved));
    std::vector<uint64_t> PCs;
    BackendHooks H;
    H.Instruction = [&](uint64_t PC, uint32_t) {
      PCs.push_back(PC);
      if (PC == Code + Length + ADCBytes + StoreBytes)
        CPU->stop();
    };
    const auto Exit = run(H);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(PCs, (std::vector<uint64_t>{
                       Code, Code + Length, Code + Length + ADCBytes,
                       Code + Length + ADCBytes + StoreBytes}));
    const auto E = expected(shift(), Seed, Source, ProbeCount, Flags);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)),
              E.Value + (E.Flags & Carry));
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)),
              E.Value + (E.Flags & Carry));
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64DoubleShift,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.name(); });
} // namespace
} // namespace neverd::emulation
