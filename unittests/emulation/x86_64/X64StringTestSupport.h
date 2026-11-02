//===- X64StringTestSupport.h - Shared string execution test harness -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_X86_64_X64STRINGTESTSUPPORT_H
#define NEVERD_UNITTESTS_EMULATION_X86_64_X64STRINGTESTSUPPORT_H
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace neverd::emulation::string_test {
#define NEVERD_USER_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "../UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_STRING_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#define NEVERD_STRING_TEXT(Name, Value) inline constexpr char Name[] = Value;
#define NEVERD_STRING_BYTES(Name, ...)                                         \
  inline constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64StringTransferCases.def"
#undef NEVERD_STRING_BYTES
#undef NEVERD_STRING_TEXT
#undef NEVERD_STRING_VALUE

struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  bool User;
};
inline void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
inline constexpr Parameter Parameters[] = {
#define NEVERD_STRING_BACKEND(Name, Backend, User)                             \
  {#Name, ExecutionBackendKind::Backend, User},
#include "X64StringTransferCases.def"
#undef NEVERD_STRING_BACKEND
};
struct StringState {
  uint64_t AX, CX, SI, DI, Flags;
};
static_assert(offsetof(StringState, CX) == WordBytes);
static_assert(offsetof(StringState, SI) == 2 * WordBytes);
static_assert(offsetof(StringState, DI) == 3 * WordBytes);
static_assert(offsetof(StringState, Flags) == 4 * WordBytes);

inline void runHost(llvm::ArrayRef<uint8_t> Instruction, StringState &State) {
#if defined(__x86_64__) || defined(_M_X64)
  std::vector<uint8_t> Bytes(std::begin(OraclePrefix), std::end(OraclePrefix));
#ifdef _WIN32
  Bytes.insert(Bytes.end(), std::begin(Win64Argument), std::end(Win64Argument));
#else
  Bytes.insert(Bytes.end(), std::begin(SysVArgument), std::end(SysVArgument));
#endif
  Bytes.insert(Bytes.end(), std::begin(OracleLoad), std::end(OracleLoad));
  Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
  Bytes.insert(Bytes.end(), std::begin(OracleSave), std::end(OracleSave));
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
  reinterpret_cast<void (*)(StringState *)>(Block.base())(&State);
#else
  ADD_FAILURE() << OracleUnavailable;
#endif
}

class X64StringTest : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::array<uint8_t, BufferBytes> Before{};
  void SetUp() override { resetCPU(); }
  void resetCPU() {
    CPU.reset();
    auto B = createExecutionBackend(GetParam().Backend,
                                    GetParam().User
                                        ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedX64,
                                    Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(GetParam().Backend, GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->mapAlias(Alias, Data, PageSize, Read | Write | UserAccessible));
    for (size_t I = 0; I < Before.size(); ++I)
      Before[I] = uint8_t(Seed + I * PatternStep);
  }
  uint64_t reg(X64Register R) { return llvm::cantFail(CPU->reg(R)); }
  void seed(const StringState &S) {
    llvm::cantFail(CPU->write(Data, Before));
    llvm::cantFail(CPU->setReg(X64Register::AX, S.AX));
    llvm::cantFail(CPU->setReg(X64Register::CX, S.CX));
    llvm::cantFail(CPU->setReg(X64Register::SI, S.SI));
    llvm::cantFail(CPU->setReg(X64Register::DI, S.DI));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, S.Flags));
  }
  void expectState(const StringState &S) {
    EXPECT_EQ(reg(X64Register::AX), S.AX);
    EXPECT_EQ(reg(X64Register::CX), S.CX);
    EXPECT_EQ(reg(X64Register::SI), S.SI);
    EXPECT_EQ(reg(X64Register::DI), S.DI);
    EXPECT_EQ(reg(X64Register::FLAGS), S.Flags);
  }
  ExecutionExit run(llvm::ArrayRef<uint8_t> Instruction,
                    BackendHooks Hooks = {}) {
    std::vector<uint8_t> Bytes(Instruction.begin(), Instruction.end());
    Bytes.push_back(Nop);
    auto Failed = [](llvm::Error E) {
      auto Reason = llvm::toString(std::move(E));
      ADD_FAILURE() << Reason;
      return ExecutionExit{ExecutionExitKind::BackendFailure, std::nullopt,
                           Reason};
    };
    if (auto E = CPU->write(Code, Bytes))
      return Failed(std::move(E));
    auto Observer = std::move(Hooks.Instruction);
    Hooks.Instruction = [&, Observer](uint64_t PC, uint32_t Size) {
      if (PC != Code)
        CPU->stop();
      else if (Observer)
        Observer(PC, Size);
    };
    if (auto E = CPU->installHooks(std::move(Hooks)))
      return Failed(std::move(E));
    auto Exit = CPU->runUntilExit(Code, Timeout);
    if (!Exit)
      return Failed(Exit.takeError());
    return std::move(*Exit);
  }
  std::array<uint8_t, BufferBytes> memory(uint64_t Address = Data) {
    std::array<uint8_t, BufferBytes> Bytes{};
    llvm::cantFail(CPU->addressSpace()->read(Address, Bytes));
    return Bytes;
  }
  std::array<uint8_t, WordBytes> word(uint64_t Address) {
    std::array<uint8_t, WordBytes> Bytes{};
    llvm::cantFail(CPU->addressSpace()->read(Address, Bytes));
    return Bytes;
  }
  void mapCrossingPages() {
    llvm::cantFail(CPU->map(Stack, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->mapAlias(Alias + PageSize, Data + PageSize, PageSize,
                                 Read | Write | UserAccessible));
  }
  template <typename Case>
  static std::vector<uint8_t> encoding(const Case &C, bool Repeat,
                                       bool Narrow = false,
                                       uint8_t Segment = 0) {
    std::vector<uint8_t> Bytes;
    if (Narrow)
      Bytes.push_back(AddressPrefix);
    if (Segment)
      Bytes.push_back(Segment);
    if (Repeat)
      Bytes.push_back(Rep);
    Bytes.insert(Bytes.end(), C.Bytes.begin(), C.Bytes.end());
    return Bytes;
  }
};

} // namespace neverd::emulation::string_test
#endif
