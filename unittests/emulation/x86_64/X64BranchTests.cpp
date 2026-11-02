//===- X64BranchTests.cpp - Relative branches across CPU transports -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/CheckedX64Backend.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <array>
#include <cstring>
#include <iostream>
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
#define NEVERD_BRANCH_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_BRANCH_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_BRANCH_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_BYTES
#undef NEVERD_BRANCH_TEXT
#undef NEVERD_BRANCH_VALUE
enum class Operation {
#define NEVERD_BRANCH_OPERATION(Name, Short, Near, Expression) Name,
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_OPERATION
};
struct Branch {
  const char *Name;
  Operation Kind;
  uint8_t Short, Near;
};
constexpr Branch Branches[] = {
#define NEVERD_BRANCH_OPERATION(Name, Short, Near, Expression)                 \
  {#Name, Operation::Name, Short, Near},
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_OPERATION
};
enum class PrefixIndex {
#define NEVERD_BRANCH_PREFIX(Name, ...) Name,
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_PREFIX
};
struct Prefix {
  const char *Name;
  bool Operand16;
  std::vector<uint8_t> Bytes;
};
const Prefix Prefixes[] = {
#define NEVERD_BRANCH_PREFIX(Name, Operand16, ...)                             \
  {#Name, Operand16, {__VA_ARGS__}},
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_PREFIX
};
constexpr int Displacements[] = {
#define NEVERD_BRANCH_DISPLACEMENT(Value) Value,
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_DISPLACEMENT
};
bool taken(Operation Kind, uint64_t Flags) {
  const bool C = Flags & CarryFlag, P = Flags & ParityFlag,
             Z = Flags & ZeroFlag, S = Flags & SignFlag,
             O = Flags & OverflowFlag;
  switch (Kind) {
#define NEVERD_BRANCH_OPERATION(Name, Short, Near, Expression)                 \
  case Operation::Name:                                                        \
    return Expression;
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_OPERATION
  }
  std::abort();
}
bool boundaryBranch(Operation Kind) {
  switch (Kind) {
#define NEVERD_BRANCH_BOUNDARY(Name)                                           \
  case Operation::Name:                                                        \
    return true;
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_BOUNDARY
  default:
    return false;
  }
}
uint64_t flags(unsigned Combination) {
  constexpr uint64_t Bits[]{CarryFlag, ParityFlag, ZeroFlag, SignFlag,
                            OverflowFlag};
  uint64_t Value = ClearFlags;
  for (unsigned I = 0; I < std::size(Bits); ++I)
    if (Combination & (1u << I))
      Value |= Bits[I];
  return Value;
}
std::vector<uint8_t> encoding(const Prefix &P, const Branch &B, bool Near,
                              bool AMD, int Offset) {
  auto Bytes = P.Bytes;
  if (Near && B.Kind != Operation::JMP)
    Bytes.push_back(OpcodeEscape);
  Bytes.push_back(Near ? B.Near : B.Short);
  const unsigned Width =
      Near ? (AMD && P.Operand16 ? HalfBytes : WordBytes) : ShortBytes;
  for (unsigned I = 0; I < Width; ++I)
    Bytes.push_back(uint32_t(Offset) >> (I * ByteBits));
  return Bytes;
}
bool nativeAMD() {
#if defined(_MSC_VER) && defined(_M_X64)
  int R[4];
  __cpuid(R, 0);
  return uint32_t(R[1]) == AMDVendorEBX && uint32_t(R[3]) == AMDVendorEDX &&
         uint32_t(R[2]) == AMDVendorECX;
#elif defined(__x86_64__)
  unsigned AX, BX, CX, DX;
  __cpuid(0, AX, BX, CX, DX);
  return BX == AMDVendorEBX && DX == AMDVendorEDX && CX == AMDVendorECX;
#else
  return false;
#endif
}
TEST(X64BranchOracle, ConditionsFlagsAndWidthsMatchOriginalHostInstructions) {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned Executed = 0, GuestOnly = 0;
  for (const auto &P : Prefixes)
    for (const auto &B : Branches)
      for (bool Near : {false, true}) {
        std::vector<uint8_t> Bytes;
        auto Append = [&](llvm::ArrayRef<uint8_t> Part) {
          Bytes.insert(Bytes.end(), Part.begin(), Part.end());
        };
        Append(OraclePrefix);
#ifdef _WIN32
        Append(Win64Argument);
#else
        Append(SysVArgument);
#endif
        Append(OracleLoad);
        Append(encoding(P, B, Near, nativeAMD(),
                        sizeof(OracleFalse) + sizeof(OracleSkipTrue)));
        Append(OracleFalse);
        Append(OracleSkipTrue);
        Append(OracleTrue);
        Append(OracleSave);
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
        llvm::sys::Memory::InvalidateInstructionCache(Block.base(),
                                                      Bytes.size());
        auto Execute = reinterpret_cast<void (*)(uint64_t *)>(Block.base());
        for (unsigned F = 0; F < FlagCombinations; ++F) {
          const uint64_t Before = flags(F);
          SCOPED_TRACE(P.Name);
          SCOPED_TRACE(B.Name);
          SCOPED_TRACE(Near);
          SCOPED_TRACE(F);
          if (nativeAMD() && P.Operand16 && taken(B.Kind, Before)) {
            ++GuestOnly;
            continue;
          }
          std::array<uint64_t, 5> Packet{Source, Before};
          Execute(Packet.data());
          ++Executed;
          ASSERT_EQ(Packet[2], Packet[0]);
          ASSERT_EQ(Packet[3], Packet[1]);
          ASSERT_EQ(Packet[4], taken(B.Kind, Packet[1]));
        }
      }
  std::cout << OracleExecuted << Executed << OracleGuestOnly << GuestOnly
            << '\n';
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}
class DecodeMachine final : public X64Machine {
public:
  explicit DecodeMachine(X64BranchModel Model) : Model(Model) {}
  X64BranchModel branchModel() const override { return Model; }
  llvm::Error step(X64MachineState &, uint64_t, MachineRunControl) override {
    ADD_FAILURE();
    return llvm::Error::success();
  }

private:
  const X64BranchModel Model;
};
TEST(X64BranchDecoding, BothModelsRequireCompleteBytesBeforeObservers) {
  for (auto Model : {X64BranchModel::Intel, X64BranchModel::AMD})
    for (const auto &P : Prefixes)
      for (const auto &B : Branches)
        for (bool Near : {false, true})
          for (bool Complete : {false, true}) {
            SCOPED_TRACE(P.Name);
            SCOPED_TRACE(B.Name);
            SCOPED_TRACE(Near);
            SCOPED_TRACE(Complete);
            auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
            auto CPU = llvm::cantFail(CheckedX64Backend::create(
                std::move(Memory), std::make_unique<DecodeMachine>(Model)));
            llvm::cantFail(CPU->map(Code, Page, Read | Write | Execute));
            const auto Bytes = encoding(
                P, B, Near, Model == X64BranchModel::AMD, Displacement);
            const unsigned Available = Bytes.size() - unsigned(!Complete);
            const uint64_t Entry = Code + Page - Available;
            llvm::cantFail(
                CPU->write(Entry, llvm::ArrayRef(Bytes).take_front(Available)));
            unsigned Visits = 0;
            BackendHooks H;
            H.Instruction = [&](uint64_t PC, uint32_t Length) {
              EXPECT_EQ(PC, Entry);
              EXPECT_EQ(Length, Bytes.size());
              ++Visits;
              CPU->stop();
            };
            llvm::cantFail(CPU->installHooks(std::move(H)));
            const auto Exit = llvm::cantFail(CPU->runUntilExit(Entry, Timeout));
            ASSERT_EQ(Exit.Kind, Complete
                                     ? ExecutionExitKind::Stopped
                                     : ExecutionExitKind::UnsupportedOperation)
                << Exit.Diagnostic;
            EXPECT_EQ(Visits, unsigned(Complete));
            EXPECT_EQ(CPU->x64BranchModel(), Model);
            EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Entry);
          }
}
struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
  ExecutionContract Contract;
};
constexpr Backend Backends[] = {
#define NEVERD_BRANCH_BACKEND(Name, Kind, Contract)                            \
  {#Name, ExecutionBackendKind::Kind, ExecutionContract::Contract},
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_BACKEND
};
struct Parameter {
  Backend B;
  Prefix P;
  bool Near;
  const char *FormName;
  std::string name() const { return std::string(B.Name) + '_' + FormName; }
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.name(); }
std::vector<Parameter> parameters() {
  std::vector<Parameter> Result;
  for (const auto &B : Backends) {
#define NEVERD_BRANCH_FORM(Name, Prefix, Near)                                 \
  Result.push_back({B, Prefixes[unsigned(PrefixIndex::Prefix)], Near, #Name});
#include "X64BranchCases.def"
#undef NEVERD_BRANCH_FORM
  }
  return Result;
}
class X64Branch : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  uint64_t Entry = Code + EntryOffset;
  unsigned Length = 0;
  bool amd() const {
    return GetParam().B.Kind != ExecutionBackendKind::Unicorn && nativeAMD();
  }
  uint64_t target(const Branch &B, uint64_t Before, int Offset) const {
    const uint64_t Next = Entry + Length;
    if (!taken(B.Kind, Before))
      return Next;
    return amd() && GetParam().P.Operand16 ? uint16_t(Next + Offset)
                                           : Next + Offset;
  }
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
    ASSERT_EQ(CPU->x64BranchModel(),
              amd() ? X64BranchModel::AMD : X64BranchModel::Intel);
    for (uint64_t A : {LowCode, LowCode + Page, Code, HighCode}) {
      llvm::cantFail(
          CPU->map(A, Page, Read | Write | Execute | UserAccessible));
      llvm::cantFail(CPU->write(A, std::vector<uint8_t>(Page, Nop)));
    }
    llvm::cantFail(CPU->map(Data, Page, Read | Write | UserAccessible));
    llvm::cantFail(CPU->write(Data, std::vector<uint8_t>(Page, Fill)));
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
  std::vector<uint8_t> prepare(const Branch &B, uint64_t Before, int Offset) {
    auto Bytes = encoding(GetParam().P, B, GetParam().Near, amd(), Offset);
    Length = Bytes.size();
    llvm::cantFail(CPU->write(Entry, Bytes));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Before));
    llvm::cantFail(CPU->setReg(X64Register::PC, Entry));
    return Bytes;
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
    std::vector<uint8_t> Bytes(Page);
    llvm::cantFail(CPU->addressSpace()->read(Data, Bytes));
    return Bytes;
  }
  ExecutionExit run(BackendHooks H = {}) {
    unsigned Visits = 0;
    if (!H.Instruction)
      H.Instruction = [&](uint64_t PC, uint32_t Size) {
        if (!Visits) {
          EXPECT_EQ(PC, Entry);
          EXPECT_EQ(Size, Length);
        }
        if (++Visits == 2)
          CPU->stop();
      };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Entry, Timeout));
  }
};
TEST_P(X64Branch, ConditionsTargetsLengthsAndStateMatch) {
  const auto RAM = memory();
  for (const auto &B : Branches)
    for (uint64_t Base : {Code, HighCode})
      for (unsigned F = 0; F < FlagCombinations; ++F) {
        const int Offset = Displacements[F % std::size(Displacements)];
        SCOPED_TRACE(B.Name);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(F);
        SCOPED_TRACE(Offset);
        Entry = Base + EntryOffset;
        prepare(B, flags(F), Offset);
        auto State = snapshot();
        unsigned Reads = 0, Writes = 0;
        BackendHooks H;
        H.Read = [&](uint64_t, uint32_t) { ++Reads; };
        H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
        const auto Exit = run(H);
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        State[CPURegister::X64PC][0] = target(B, flags(F), Offset);
        ASSERT_EQ(snapshot(), State);
        ASSERT_EQ(Reads, 0u);
        ASSERT_EQ(Writes, 0u);
      }
  EXPECT_EQ(memory(), RAM);
}
TEST_P(X64Branch, ObserverStopsAndFailuresPrecedeEffects) {
  for (const auto &B : Branches)
    for (bool Throw : {false, true}) {
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      prepare(B, SetFlags, Displacement);
      const auto State = snapshot();
      const auto RAM = memory();
      unsigned Visits = 0;
      BackendHooks H;
      H.Instruction = [&](uint64_t PC, uint32_t Size) {
        EXPECT_EQ(PC, Entry);
        EXPECT_EQ(Size, Length);
        ++Visits;
        if (Throw)
          throw std::runtime_error(ObserverFailure);
        CPU->stop();
      };
      const auto Exit = run(H);
      EXPECT_EQ(Exit.Kind, Throw ? ExecutionExitKind::BackendFailure
                                 : ExecutionExitKind::Stopped)
          << Exit.Diagnostic;
      EXPECT_EQ(Visits, 1u);
      EXPECT_EQ(snapshot(), State);
      EXPECT_EQ(memory(), RAM);
    }
}
TEST_P(X64Branch, CrossPageDecodeRequiresTheActualInstructionExtent) {
  enum class Suffix { Executable, Absent, NoExecute };
  for (const auto &B : Branches) {
    if (!boundaryBranch(B.Kind))
      continue;
    const auto Bytes =
        encoding(GetParam().P, B, GetParam().Near, amd(), Displacement);
    for (unsigned Split = 1; Split < Bytes.size(); ++Split)
      for (auto S : {Suffix::Executable, Suffix::Absent, Suffix::NoExecute}) {
        initialize();
        if (HasFatalFailure() || IsSkipped())
          return;
        llvm::cantFail(CPU->map(Code + Page, Page,
                                Read | Write | Execute | UserAccessible));
        llvm::cantFail(
            CPU->write(Code + Page, std::vector<uint8_t>(Page, Nop)));
        Entry = Code + Page - Split;
        prepare(B, SetFlags, Displacement);
        if (S == Suffix::Absent)
          llvm::cantFail(CPU->addressSpace()->unmap(Code + Page, Page));
        else if (S == Suffix::NoExecute)
          llvm::cantFail(
              CPU->protect(Code + Page, Page, Read | Write | UserAccessible));
        auto State = snapshot();
        const auto RAM = memory();
        const auto Exit = run();
        if (S == Suffix::Executable) {
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
          State[CPURegister::X64PC][0] = target(B, SetFlags, Displacement);
        } else {
          EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
              << Exit.Diagnostic;
        }
        EXPECT_EQ(snapshot(), State);
        EXPECT_EQ(memory(), RAM);
      }
  }
}
TEST_P(X64Branch, TargetFaultRetainsTheRetiredBranch) {
  for (const auto &B : Branches)
    for (bool NoExecute : {false, true}) {
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      unsigned F = 0;
      while (!taken(B.Kind, flags(F)))
        ++F;
      auto Bytes =
          encoding(GetParam().P, B, GetParam().Near, amd(), Displacement);
      Entry = Code + Page - Bytes.size();
      prepare(B, flags(F), Displacement);
      const uint64_t Target = target(B, flags(F), Displacement);
      const uint64_t TargetPage = Target & ~(Page - 1);
      if (amd() && GetParam().P.Operand16)
        llvm::cantFail(CPU->addressSpace()->unmap(TargetPage, Page));
      if (NoExecute)
        llvm::cantFail(
            CPU->map(TargetPage, Page, Read | Write | UserAccessible));
      auto State = snapshot();
      const auto RAM = memory();
      const auto Exit = run();
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
      State[CPURegister::X64PC][0] = Target;
      EXPECT_EQ(snapshot(), State);
      EXPECT_EQ(memory(), RAM);
    }
}
TEST_P(X64Branch, ContextRestoreRetainsModelAndConditionalState) {
  for (const auto &B : Branches) {
    prepare(B, SetFlags, Displacement);
    const auto Model = CPU->x64BranchModel();
    auto State = snapshot();
    const auto RAM = memory();
    auto Saved = llvm::cantFail(CPU->saveContext());
    for (bool Restore : {false, true}) {
      if (Restore)
        llvm::cantFail(CPU->restoreContext(*Saved));
      ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
      State[CPURegister::X64PC][0] = target(B, SetFlags, Displacement);
      EXPECT_EQ(snapshot(), State);
      EXPECT_EQ(memory(), RAM);
      EXPECT_EQ(CPU->x64BranchModel(), Model);
    }
  }
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64Branch,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.name(); });
} // namespace
} // namespace neverd::emulation
