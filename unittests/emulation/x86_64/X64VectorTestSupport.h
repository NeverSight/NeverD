//===- X64VectorTestSupport.h - Shared checked vector test state ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_X86_64_X64VECTORTESTSUPPORT_H
#define NEVERD_UNITTESTS_EMULATION_X86_64_X64VECTORTESTSUPPORT_H
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <map>
#include <vector>

namespace neverd::emulation::vector_test {
#define NEVERD_USER_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "../UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_VECTOR_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "X64VectorTestCases.def"
#undef NEVERD_VECTOR_VALUE

struct Input {
  RegisterValue Left, Right;
};
struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  bool User;
};
inline void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
inline constexpr Parameter Parameters[] = {
#define NEVERD_INTEGER_BACKEND(Name, Backend, User)                            \
  {#Name, ExecutionBackendKind::Backend, User},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_BACKEND
};

class X64VectorTest : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override { reset(); }
  void reset() {
    CPU.reset();
    auto B = createExecutionBackend(GetParam().Backend,
                                    GetParam().User
                                        ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedX64,
                                    Limit, GuestArchitecture::X64);
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
  }
  void seed(const Input &Input, uint64_t Address = Data) {
    llvm::cantFail(CPU->setXmm(0, Input.Left));
    llvm::cantFail(CPU->setXmm(1, Input.Right));
    for (unsigned Index = 2; Index < XmmCount; ++Index)
      llvm::cantFail(
          CPU->setXmm(Index, {SentinelLow + Index, SentinelHigh - Index}));
    llvm::cantFail(CPU->setReg(X64Register::AX, SentinelLow));
    llvm::cantFail(CPU->setReg(X64Register::DX, SentinelHigh));
    llvm::cantFail(CPU->setReg(X64Register::CX, Address));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, MXCSR));
    llvm::cantFail(CPU->writeInteger(Data, Input.Right[0], WordBytes));
    llvm::cantFail(
        CPU->writeInteger(Data + WordBytes, Input.Right[1], WordBytes));
  }
  ExecutionExit run(llvm::ArrayRef<uint8_t> Instruction,
                    BackendHooks Hooks = {}) {
    std::vector<uint8_t> Bytes(Instruction.begin(), Instruction.end());
    Bytes.push_back(Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  auto snapshot() {
    std::map<CPURegister, RegisterValue> State;
#define NEVERD_SCALAR_REGISTER(ISA, Name, Bits, Backend)                       \
  if (GuestArchitecture::ISA == GuestArchitecture::X64)                        \
    State[CPURegister::ISA##Name] =                                            \
        llvm::cantFail(CPU->readRegister(CPURegister::ISA##Name));
#define NEVERD_EXTENDED_REGISTER(ISA, Name, Bits, Backend)                     \
  NEVERD_SCALAR_REGISTER(ISA, Name, Bits, Backend)
#define NEVERD_VECTOR_REGISTER(ISA, Index, Backend)                            \
  if (GuestArchitecture::ISA == GuestArchitecture::X64) {                      \
    const auto R = vectorRegister(GuestArchitecture::ISA, Index);              \
    State[R] = llvm::cantFail(CPU->readRegister(R));                           \
  }
#include "neverd/emulation/Registers.def"
#undef NEVERD_VECTOR_REGISTER
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_SCALAR_REGISTER
    return State;
  }
  void expectState(const Input &Input, const RegisterValue &Expected,
                   uint64_t PC, uint64_t Address = Data) {
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), Expected);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(1)), Input.Right);
    for (unsigned Index = 2; Index < XmmCount; ++Index)
      EXPECT_EQ(llvm::cantFail(CPU->xmm(Index)),
                (RegisterValue{SentinelLow + Index, SentinelHigh - Index}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), SentinelLow);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), SentinelHigh);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), Address);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), Flags);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)), MXCSR);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), PC);
    // A terminal CPU fault rejects ordinary API reads. Inspect diagnostic
    // backing directly without consuming the fault or changing guest state.
    std::array<uint8_t, VectorBytes> Bytes{};
    ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Alias, Bytes)), "");
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), Input.Right[0]);
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + WordBytes),
              Input.Right[1]);
  }
};

} // namespace neverd::emulation::vector_test
#endif
