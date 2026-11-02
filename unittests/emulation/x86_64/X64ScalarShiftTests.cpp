//===- X64ScalarShiftTests.cpp - Transactional shifts and rotates --------===//
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
#define NEVERD_SCALAR_SHIFT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SCALAR_SHIFT_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_SCALAR_SHIFT_BYTES(Name, ...)                                   \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64ScalarShiftCases.def"
#undef NEVERD_SCALAR_SHIFT_BYTES
#undef NEVERD_SCALAR_SHIFT_TEXT
#undef NEVERD_SCALAR_SHIFT_VALUE
constexpr unsigned Counts[] = {
#define NEVERD_SCALAR_SHIFT_COUNT(Value) Value,
#include "X64ScalarShiftCases.def"
#undef NEVERD_SCALAR_SHIFT_COUNT
};
constexpr uint64_t Inputs[] = {
#define NEVERD_SCALAR_SHIFT_INPUT(Value) Value,
#include "X64ScalarShiftCases.def"
#undef NEVERD_SCALAR_SHIFT_INPUT
};
enum class Operation { SHL, SHR, SAR, ROL, ROR, RCL, RCR };
enum class Form { Immediate, CL, One };
struct Shift {
  const char *Name;
  unsigned Size;
  Operation Op;
  uint8_t Extension;
};
constexpr Shift Shifts[] = {
#define NEVERD_SCALAR_SHIFT_CASE(Name, Size, Op, Extension)                    \
  {#Name, Size, Operation::Op, Extension},
#include "X64ScalarShiftCases.def"
#undef NEVERD_SCALAR_SHIFT_CASE
};
uint64_t widthMask(const Shift &S) {
  return UINT64_MAX >> ((WordBytes - S.Size) * CHAR_BIT);
}
bool rotates(const Shift &S) {
  return S.Op == Operation::ROL || S.Op == Operation::ROR ||
         S.Op == Operation::RCL || S.Op == Operation::RCR;
}
bool throughCarry(const Shift &S) {
  return S.Op == Operation::RCL || S.Op == Operation::RCR;
}
unsigned count(const Shift &S, unsigned Raw) {
  return Raw % ((S.Size == WordBytes ? WordBytes : DWordBytes) * CHAR_BIT);
}
unsigned effectiveCount(const Shift &S, unsigned Raw) {
  const unsigned N = count(S, Raw), Bits = S.Size * CHAR_BIT;
  if (throughCarry(S) && S.Size < DWordBytes)
    return N % (Bits + 1);
  return rotates(S) && !throughCarry(S) ? N % Bits : N;
}
uint64_t definedFlags(const Shift &S, unsigned Raw) {
  const unsigned N = count(S, Raw);
  // A whole carry-ring rotation can preserve the value and CF while leaving
  // OF undefined. Only a zero masked count guarantees every flag is unchanged.
  if (!N)
    return UINT64_MAX;
  uint64_t Undefined = N == 1 ? 0 : Overflow;
  if (!rotates(S)) {
    Undefined |= Auxiliary;
    if (S.Op != Operation::SAR && N >= S.Size * CHAR_BIT)
      Undefined |= Carry;
  }
  return ~Undefined;
}
struct Result {
  uint64_t Value, Flags;
};
// A one-bit recurrence is independent of the host's variable-count opcode.
// It avoids C++ shifts by the word size and models the extra carry bit
// directly.
Result expected(const Shift &S, uint64_t Input, unsigned Raw, uint64_t Flags) {
  const unsigned Bits = S.Size * CHAR_BIT, N = count(S, Raw);
  const uint64_t Mask = widthMask(S), SignBit = uint64_t(1) << (Bits - 1);
  uint64_t Value = Input & Mask;
  bool CF = Flags & Carry;
  for (unsigned I = 0; I < effectiveCount(S, Raw); ++I) {
    const bool High = Value & SignBit, Low = Value & 1, BeforeCarry = CF;
    switch (S.Op) {
    case Operation::SHL:
      CF = High;
      Value = (Value << 1) & Mask;
      break;
    case Operation::SHR:
      CF = Low;
      Value >>= 1;
      break;
    case Operation::SAR:
      CF = Low;
      Value = (Value >> 1) | (High ? SignBit : 0);
      break;
    case Operation::ROL:
      CF = High;
      Value = ((Value << 1) | High) & Mask;
      break;
    case Operation::ROR:
      CF = Low;
      Value = (Value >> 1) | (Low ? SignBit : 0);
      break;
    case Operation::RCL:
      CF = High;
      Value = ((Value << 1) | BeforeCarry) & Mask;
      break;
    case Operation::RCR:
      CF = Low;
      Value = (Value >> 1) | (BeforeCarry ? SignBit : 0);
      break;
    }
  }
  if (N && !(throughCarry(S) && !effectiveCount(S, Raw))) {
    if (S.Op == Operation::ROL)
      CF = Value & 1;
    if (S.Op == Operation::ROR)
      CF = Value & SignBit;
    bool OF = false;
    if (N == 1) {
      if (S.Op == Operation::SHR)
        OF = Input & SignBit;
      else if (S.Op == Operation::ROR || S.Op == Operation::RCR)
        OF = bool(Value & SignBit) != bool(Value & (SignBit >> 1));
      else if (S.Op != Operation::SAR)
        OF = bool(Value & SignBit) != CF;
    }
    Flags =
        (Flags & ~(Carry | Overflow)) | (CF ? Carry : 0) | (OF ? Overflow : 0);
    if (!rotates(S)) {
      Flags &= ~(Parity | Auxiliary | Zero | Sign);
      Flags |= (!Value ? Zero : 0) | (Value & SignBit ? Sign : 0) |
               (std::popcount(uint8_t(Value)) % 2 == 0 ? Parity : 0);
    }
  }
  if (S.Size < DWordBytes)
    Value |= Input & ~Mask;
  return {Value, Flags};
}
std::vector<uint8_t> encoding(const Shift &S, unsigned Raw, Form F,
                              bool Memory = false, uint8_t RM = 0,
                              uint8_t REX = 0) {
  std::vector<uint8_t> Bytes;
  if (S.Size == HalfWordBytes)
    Bytes.push_back(Operand16);
  if (S.Size == WordBytes)
    REX |= Operand64;
  if (REX)
    Bytes.push_back(REX);
  const auto Opcode = F == Form::Immediate ? ImmediateByte
                      : F == Form::CL      ? CLByte
                                           : OneByte;
  Bytes.push_back(Opcode + (S.Size == ByteBytes ? 0 : WideOpcodeOffset));
  Bytes.push_back(S.Extension |
                  (Memory ? MemoryDestination : RegisterDestination | RM));
  if (F == Form::Immediate)
    Bytes.push_back(Raw);
  return Bytes;
}

TEST(X64ScalarShiftOracle, DefinedResultsMatchOriginalHostInstructions) {
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
    const auto Instruction = encoding(S, 0, Form::CL);
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
      for (uint64_t Input : Inputs)
        for (uint64_t Before : {Reserved, Flags}) {
          SCOPED_TRACE(S.Name);
          SCOPED_TRACE(Raw);
          SCOPED_TRACE(Input);
          SCOPED_TRACE(Before);
          std::array<uint64_t, 5> Packet{Input, Raw, Before};
          Execute(Packet.data());
          const auto E = expected(S, Input, Raw, Packet[2]);
          ASSERT_EQ(Packet[3], E.Value);
          ASSERT_EQ(Packet[4] & definedFlags(S, Raw),
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
#define NEVERD_SCALAR_SHIFT_BACKEND(Name, Kind, Contract)                      \
  {#Name, ExecutionBackendKind::Kind, ExecutionContract::Contract},
#include "X64ScalarShiftCases.def"
#undef NEVERD_SCALAR_SHIFT_BACKEND
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
class X64ScalarShift : public testing::TestWithParam<Parameter> {
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
  void prepare(unsigned Raw, Form F, bool Memory = false, uint64_t AX = Seed,
               uint64_t Before = Flags) {
    auto Bytes = encoding(shift(), Raw, F, Memory);
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
  void registerCounts(Form F) {
    const auto RAM = memory();
    for (unsigned Raw = 0; Raw <= UINT8_MAX; ++Raw)
      for (uint64_t AX : Inputs)
        for (uint64_t BeforeFlags : {Reserved, Flags}) {
          SCOPED_TRACE(Raw);
          SCOPED_TRACE(AX);
          SCOPED_TRACE(BeforeFlags);
          prepare(Raw, F, false, AX, BeforeFlags);
          auto State = snapshot();
          unsigned Reads = 0, Writes = 0;
          BackendHooks H;
          H.Read = [&](uint64_t, uint32_t) { ++Reads; };
          H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
          const auto Exit = run(H);
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
          const auto E = expected(shift(), AX, Raw, BeforeFlags);
          auto Actual = snapshot();
          State[CPURegister::X64AX][0] = E.Value;
          State[CPURegister::X64PC][0] += Length;
          State[CPURegister::X64FLAGS][0] =
              E.Flags & definedFlags(shift(), Raw);
          Actual[CPURegister::X64FLAGS][0] &= definedFlags(shift(), Raw);
          ASSERT_EQ(Actual, State);
          ASSERT_EQ(Reads, 0u);
          ASSERT_EQ(Writes, 0u);
        }
    EXPECT_EQ(memory(), RAM);
  }
};
TEST_P(X64ScalarShift, RegisterImmediateCountsMatchDefinedSemantics) {
  registerCounts(Form::Immediate);
}
TEST_P(X64ScalarShift, RegisterCLCountsMatchDefinedSemantics) {
  registerCounts(Form::CL);
}
TEST_P(X64ScalarShift, ImplicitOneAndAliasedRegistersUseOriginalInputs) {
  const struct {
    const char *Name;
    X64Register Destination;
    uint8_t RM, REX;
    unsigned Offset;
    bool ByteOnly;
  } Cases[] = {
#define NEVERD_SCALAR_SHIFT_ALIAS(Name, Destination, RM, REX, Offset,          \
                                  ByteOnly)                                    \
  {#Name, X64Register::Destination, RM, REX, Offset, ByteOnly},
#include "X64ScalarShiftCases.def"
#undef NEVERD_SCALAR_SHIFT_ALIAS
  };
  const auto RAM = memory();
  for (Form F : {Form::Immediate, Form::CL, Form::One})
    for (const auto &C : Cases) {
      if (C.ByteOnly && shift().Size != ByteBytes)
        continue;
      SCOPED_TRACE(C.Name);
      const unsigned Raw = F == Form::One ? 1 : ProbeCount;
      prepare(Raw, F);
      auto Bytes = encoding(shift(), Raw, F, false, C.RM, C.REX);
      Length = Bytes.size();
      Bytes.push_back(Nop);
      llvm::cantFail(CPU->write(Code, Bytes));
      const auto Original = llvm::cantFail(CPU->reg(C.Destination));
      auto State = snapshot();
      const auto Exit = run();
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      auto Actual = snapshot();
      const auto E = expected(shift(), Original >> C.Offset, Raw, Flags);
      State[cpuRegister(C.Destination)][0] =
          C.Offset ? (Original & ~(widthMask(shift()) << C.Offset)) |
                         ((E.Value & widthMask(shift())) << C.Offset)
                   : E.Value;
      State[CPURegister::X64PC][0] += Length;
      State[CPURegister::X64FLAGS][0] = E.Flags & definedFlags(shift(), Raw);
      Actual[CPURegister::X64FLAGS][0] &= definedFlags(shift(), Raw);
      EXPECT_EQ(Actual, State);
    }
  EXPECT_EQ(memory(), RAM);
}
TEST_P(X64ScalarShift, MemoryResultsAndObserversUseExactOperandSpans) {
  for (Form F : {Form::Immediate, Form::CL, Form::One})
    for (unsigned Raw : Counts)
      for (uint64_t Input : Inputs)
        for (uint64_t BeforeFlags : {Reserved, Flags})
          if (F != Form::One || Raw == 1) {
            SCOPED_TRACE(Raw);
            SCOPED_TRACE(Input);
            SCOPED_TRACE(BeforeFlags);
            prepare(Raw, F, true, Seed, BeforeFlags);
            const auto Address = Data + Page - shift().Size / 2;
            llvm::cantFail(CPU->setReg(X64Register::SI, Address));
            llvm::cantFail(CPU->writeInteger(Address, Input, WordBytes));
            auto State = snapshot();
            auto RAM = memory();
            const auto E = expected(shift(), Input, Raw, BeforeFlags);
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
            State[CPURegister::X64FLAGS][0] =
                E.Flags & definedFlags(shift(), Raw);
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

TEST_P(X64ScalarShift, ReadStopsAndWriteCancellationPreserveState) {
  for (bool ReadStop : {true, false})
    for (bool Throw : {false, true}) {
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      prepare(ProbeCount, Form::CL, true);
      llvm::cantFail(CPU->writeInteger(Data, Seed, WordBytes));
      const auto State = snapshot();
      const auto RAM = memory();
      unsigned Reads = 0, Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t, uint32_t) {
        ++Reads;
        if (ReadStop) {
          if (Throw)
            throw std::runtime_error(ObserverFailure);
          CPU->stop();
        }
      };
      H.Write = [&](uint64_t, uint32_t, uint64_t Value) {
        ++Writes;
        EXPECT_EQ(Value, expected(shift(), Seed, ProbeCount, Flags).Value &
                             widthMask(shift()));
        EXPECT_EQ(memory(), RAM);
        if (Throw)
          throw std::runtime_error(ObserverFailure);
        CPU->stop();
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

TEST_P(X64ScalarShift, MemoryFaultsAndDeviceOperandsPreserveState) {
  enum class Mapping { ReadOnly, Absent, Device };
  for (auto M : {Mapping::ReadOnly, Mapping::Absent, Mapping::Device})
    for (unsigned Raw : {0u, unsigned(ProbeCount)}) {
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      prepare(Raw, Form::CL, true);
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

TEST_P(X64ScalarShift, LockedFormsRejectBeforeEffects) {
  for (bool Memory : {false, true})
    for (Form F : {Form::Immediate, Form::CL, Form::One}) {
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      prepare(ProbeCount, F, Memory);
      auto Bytes = encoding(shift(), ProbeCount, F, Memory);
      Bytes.insert(Bytes.begin(), Lock);
      Length = Bytes.size();
      llvm::cantFail(CPU->write(Code, Bytes));
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
    }
}
TEST_P(X64ScalarShift, ContextRestoreAndNativeContinuationPreserveCarry) {
  prepare(ProbeCount, Form::CL);
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
    const auto E = expected(shift(), Seed, ProbeCount, Flags);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)),
              E.Value + (E.Flags & Carry));
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)),
              E.Value + (E.Flags & Carry));
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64ScalarShift,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.name(); });
} // namespace
} // namespace neverd::emulation
