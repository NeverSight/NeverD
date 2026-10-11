//===- X64DataTests.cpp - Checked native scalar and SSE2 state -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "X64DAZTestSupport.h"
#include "arch/x86_64/X64Exception.h"
#include "arch/x86_64/X64Machine.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_DATA_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_DATA_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_DATA_TEXT(Name, Text) constexpr char Name[] = Text;
#include "X64DataCases.def"
#undef NEVERD_DATA_BYTES
#undef NEVERD_DATA_TEXT
#undef NEVERD_DATA_VALUE

#define NEVERD_MXCSR_TEXT(Name, Text) constexpr char MXCSR##Name[] = Text;
#include "X64MXCSRCases.def"
#undef NEVERD_MXCSR_TEXT
using Parameter = std::tuple<ExecutionBackendKind, ExecutionContract>;
class X64DataBase : public testing::Test {
protected:
  virtual Parameter parameter() const = 0;
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto B = createExecutionBackend(std::get<0>(parameter()),
                                    std::get<1>(parameter()), Limit,
                                    GuestArchitecture::X64);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(std::get<0>(parameter()), GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->setReg(X64Register::CX, Data));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
  }
  void arithmetic(uint64_t DAZ, llvm::ArrayRef<uint8_t> Selected = {});
  ExecutionExit run(llvm::ArrayRef<uint8_t> Bytes, BackendHooks Hooks = {}) {
    std::vector<uint8_t> CodeBytes(Bytes.begin(), Bytes.end());
    CodeBytes.push_back(NopOpcode);
    llvm::cantFail(CPU->write(Code, CodeBytes));
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void expectStopped(const ExecutionExit &Exit) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  }
  void seedVector() {
    llvm::cantFail(CPU->setXmm(0, {Low, High}));
    llvm::cantFail(CPU->writeInteger(Data, Low, WordBytes));
    llvm::cantFail(CPU->writeInteger(Data + WordBytes, High, WordBytes));
  }
};

class X64Data : public X64DataBase,
                public testing::WithParamInterface<Parameter> {
  Parameter parameter() const override { return GetParam(); }
};

class X64Address : public X64DataBase,
                   public testing::WithParamInterface<vector_test::Parameter> {
  Parameter parameter() const override {
    return {GetParam().Backend, GetParam().User
                                    ? ExecutionContract::CheckedUserX64
                                    : ExecutionContract::CheckedX64};
  }
};

TEST_P(X64Address, SibIndicesPreserveAddressesAndMemoryTransactions) {
  for (bool Narrow : {false, true}) {
    for (bool ExtendedIndex : {false, true}) {
      for (unsigned Scale = 0; Scale != 4; ++Scale) {
        for (unsigned Size : {4u, 8u}) {
          for (bool Store : {false, true}) {
            for (bool Stop : {false, true}) {
              SCOPED_TRACE(testing::Message()
                           << Narrow << '/' << ExtendedIndex << '/' << Scale
                           << '/' << Size << '/' << Store << '/' << Stop);
              // MOV EAX/RAX, [EBX/RBX + absent-index/R12 * scale - 25],
              // or the corresponding store. REX.X distinguishes real R12
              // from the otherwise absent SIB index; scale is ignored only
              // for the absent index. High halves test address-size wrapping.
              std::vector<uint8_t> Bytes;
              if (Narrow)
                Bytes.push_back(0x67);
              if (Size == 8 || ExtendedIndex)
                Bytes.push_back(0x40 | (Size == 8 ? 8 : 0) |
                                (ExtendedIndex ? 2 : 0));
              Bytes.insert(Bytes.end(), {uint8_t(Store ? 0x89 : 0x8b), 0x44,
                                         uint8_t((Scale << 6) | 0x23), 0xe7});
              const uint64_t HighAddress =
                  Narrow ? UINT64_C(0x123400000000) : 0;
              const uint64_t Base = HighAddress + Data + 0x99;
              const uint64_t Index = HighAddress + 7;
              const uint64_t Address =
                  Data + 0x80 + (ExtendedIndex ? (7u << Scale) : 0);
              const uint64_t Mask = Size == 8 ? UINT64_MAX : UINT32_MAX;
              llvm::cantFail(CPU->setReg(X64Register::BX, Base));
              llvm::cantFail(CPU->setReg(X64Register::R12, Index));
              llvm::cantFail(CPU->setReg(X64Register::SP, Stack));
              llvm::cantFail(CPU->setReg(X64Register::AX, Low));
              llvm::cantFail(CPU->writeInteger(Address, High, WordBytes));
              unsigned Reads = 0, Writes = 0;
              BackendHooks Hooks;
              Hooks.Read = [&](uint64_t A, uint32_t N) {
                EXPECT_EQ(A, Address);
                EXPECT_EQ(N, Size);
                ++Reads;
                if (Stop)
                  CPU->stop();
              };
              Hooks.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
                EXPECT_EQ(A, Address);
                EXPECT_EQ(N, Size);
                EXPECT_EQ(V, Low & Mask);
                EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)),
                          High);
                ++Writes;
                if (Stop)
                  CPU->stop();
              };
              expectStopped(run(Bytes, std::move(Hooks)));
              ASSERT_FALSE(HasFatalFailure());
              EXPECT_EQ(Reads, Store ? 0u : 1u);
              EXPECT_EQ(Writes, Store ? 1u : 0u);
              EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)),
                        Store && !Stop ? (High & ~Mask) | (Low & Mask) : High);
              EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)),
                        Store || Stop ? Low : High & Mask);
              EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::BX)), Base);
              EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::R12)), Index);
              EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SP)), Stack);
              EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)),
                        InitialFlags);
              EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)),
                        Stop ? Code : Code + Bytes.size());
            }
          }
        }
      }
    }
  }
}

TEST_P(X64Address, AbsentSibIndicesRetainMemoryFaultsAndEntryState) {
  for (bool Narrow : {false, true}) {
    for (bool Store : {false, true}) {
      SetUp();
      ASSERT_TRUE(CPU);
      std::vector<uint8_t> Bytes;
      if (Narrow)
        Bytes.push_back(0x67);
      // An absent SIB index with redundant scale 8 and an unmapped base.
      Bytes.insert(Bytes.end(),
                   {0x48, uint8_t(Store ? 0x89 : 0x8b), 0x04, 0xe3});
      llvm::cantFail(CPU->setReg(X64Register::BX, Data + PageSize));
      llvm::cantFail(CPU->setReg(X64Register::AX, Low));
      const auto Exit = run(Bytes);
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::UnmappedMemory);
      EXPECT_EQ(Exit.Fault->Access,
                Store ? BackendAccessKind::Write : BackendAccessKind::Read);
      EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
      EXPECT_EQ(Exit.Fault->Size, WordBytes);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Low);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64Address,
                         testing::ValuesIn(vector_test::Parameters),
                         [](const auto &Info) { return Info.param.Name; });

TEST_P(X64Data, MemoryArithmeticPreviewMatchesNativeBytesAndDefinedFlags) {
  auto Check = [&](const char *Name, unsigned Size, uint64_t Before,
                   uint64_t Operand, uint64_t After, uint64_t Flags,
                   llvm::ArrayRef<uint8_t> Bytes) {
    SCOPED_TRACE(Name);
    for (bool Stop : {true, false}) {
      llvm::cantFail(CPU->writeInteger(Data, Before, Size));
      llvm::cantFail(CPU->writeInteger(Data + Size, High, WordBytes));
      llvm::cantFail(CPU->setReg(X64Register::DX, Operand));
      llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
      unsigned Reads = 0, Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t A, uint32_t N) {
        EXPECT_EQ(A, Data);
        EXPECT_EQ(N, Size);
        ++Reads;
      };
      H.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(A, Data);
        EXPECT_EQ(N, Size);
        EXPECT_EQ(V, After);
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Size)), Before);
        ++Writes;
        if (Stop)
          CPU->stop();
      };
      expectStopped(run(Bytes, std::move(H)));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(Writes, 1u);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Size)),
                Stop ? Before : After);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + Size, WordBytes)), High);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)) & ArithmeticFlags,
                Stop ? InitialFlags & ArithmeticFlags : Flags);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)),
                Stop ? Code : Code + Bytes.size());
    }
  };
#define NEVERD_DATA_UPDATE(Name, Size, Before, Operand, After, Flags, ...)     \
  Check(#Name, Size, Before, Operand, After, Flags, {__VA_ARGS__});
#include "X64DataCases.def"
#undef NEVERD_DATA_UPDATE
}

TEST_P(X64Data, MemorySetConditionCoversEveryFlagCombinationWithoutReading) {
  auto Check = [&](uint32_t Truth, llvm::ArrayRef<uint8_t> Bytes) {
    for (unsigned N = 0; N < FlagCombinations; ++N) {
      const uint64_t FlagBits[] = {Carry, Parity, Zero, Sign, Overflow};
      uint64_t Flags = InitialFlags & ~ArithmeticFlags;
      for (unsigned I = 0; I < std::size(FlagBits); ++I)
        if (N & (1u << I))
          Flags |= FlagBits[I];
      llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
      llvm::cantFail(CPU->writeInteger(Data, High, WordBytes));
      unsigned Reads = 0, Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t, uint32_t) { ++Reads; };
      H.Write = [&](uint64_t A, uint32_t Size, uint64_t V) {
        EXPECT_EQ(A, Data);
        EXPECT_EQ(Size, 1u);
        EXPECT_EQ(V, (Truth >> N) & 1);
        ++Writes;
      };
      expectStopped(run(Bytes, std::move(H)));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(Reads, 0u);
      EXPECT_EQ(Writes, 1u);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 1)), (Truth >> N) & 1);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), Flags);
    }
  };
#define NEVERD_DATA_CONDITION(Name, Truth, ...)                                \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Truth, {__VA_ARGS__});                                               \
  }
#include "X64DataCases.def"
#undef NEVERD_DATA_CONDITION
}

TEST_P(X64Data, RegisterBitTestMasksItsIndexAndDoesNotChangeOperands) {
  for (auto Bytes : {llvm::ArrayRef(Bit32), llvm::ArrayRef(Bit64)}) {
    const unsigned Bits = Bytes.size() == sizeof(Bit32)
                              ? sizeof(uint32_t) * CHAR_BIT
                              : sizeof(uint64_t) * CHAR_BIT;
    for (unsigned Index = 0; Index < Bits * 2; ++Index) {
      llvm::cantFail(CPU->setReg(X64Register::DX, High));
      llvm::cantFail(CPU->setReg(X64Register::CX, Index));
      expectStopped(run(Bytes));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)) & Carry,
                (High >> (Index % Bits)) & Carry);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), High);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), Index);
    }
  }
}

TEST_P(X64Data, EveryXmmRegisterSurvivesNativeExecutionAndContextRestore) {
  for (unsigned N = 0; N < XmmCount; ++N)
    llvm::cantFail(CPU->setXmm(N, {Low + N, High + N}));
  auto Saved = llvm::cantFail(CPU->saveContext());
  expectStopped(run(XorSelf));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{0, 0}));
  for (unsigned N = 0; N < XmmCount; ++N)
    llvm::cantFail(CPU->setXmm(N, {0, 0}));
  llvm::cantFail(CPU->restoreContext(*Saved));
  for (unsigned N = 0; N < XmmCount; ++N) {
    std::vector<uint8_t> Bytes(std::begin(StoreXmm), std::end(StoreXmm));
    Bytes[0] = RexBase | (N > ModRMRegisterMask ? RexHighRegister : 0);
    Bytes[ModRMOffset] =
        ModRMCX | ((N & ModRMRegisterMask) << ModRMRegisterShift);
    expectStopped(run(Bytes));
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)), Low + N);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + WordBytes, WordBytes)),
              High + N);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(N)), (RegisterValue{Low + N, High + N}));
  }
}

TEST_P(X64Data, VectorStoresObserveEveryByteBeforeAnyEffect) {
  auto Check = [&](unsigned Size, unsigned Alignment,
                   llvm::ArrayRef<uint8_t> Bytes) {
    const uint64_t Address = Data + Alignment;
    llvm::cantFail(CPU->setReg(X64Register::CX, Address));
    for (unsigned StopAt = 0; StopAt <= (Size + WordBytes - 1) / WordBytes;
         ++StopAt) {
      seedVector();
      llvm::cantFail(CPU->writeInteger(Address, 0, WordBytes));
      llvm::cantFail(CPU->writeInteger(Address + WordBytes, 0, WordBytes));
      unsigned Writes = 0;
      BackendHooks H;
      H.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
        EXPECT_EQ(A, Address + Writes * WordBytes);
        EXPECT_EQ(N, std::min<uint64_t>(Size, WordBytes));
        EXPECT_EQ(V, Writes
                         ? High
                         : Low & (UINT64_MAX >> ((WordBytes - N) * CHAR_BIT)));
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)), 0u);
        EXPECT_EQ(
            llvm::cantFail(CPU->readInteger(Address + WordBytes, WordBytes)),
            0u);
        if (++Writes == StopAt)
          CPU->stop();
      };
      expectStopped(run(Bytes, std::move(H)));
      ASSERT_FALSE(HasFatalFailure());
      if (StopAt) {
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)), 0u);
        EXPECT_EQ(
            llvm::cantFail(CPU->readInteger(Address + WordBytes, WordBytes)),
            0u);
        EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
      } else {
        EXPECT_EQ(Writes, (Size + WordBytes - 1) / WordBytes);
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(
                      Address, std::min<uint64_t>(Size, WordBytes))),
                  Low & (UINT64_MAX >>
                         ((WordBytes - std::min<uint64_t>(Size, WordBytes)) *
                          CHAR_BIT)));
        EXPECT_EQ(
            llvm::cantFail(CPU->readInteger(Address + WordBytes, WordBytes)),
            Size > WordBytes ? High : 0);
      }
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
    }
  };
#define NEVERD_DATA_STORE(Name, Size, Alignment, ...)                          \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Size, Alignment, {__VA_ARGS__});                                     \
  }
#include "X64DataCases.def"
#undef NEVERD_DATA_STORE
}

TEST_P(X64Data, VectorLoadsAndRegisterMovesPreserveTheArchitecturalUpperBits) {
  auto Check = [&](unsigned Size, RegisterValue Expected,
                   llvm::ArrayRef<uint8_t> Bytes) {
    seedVector();
    llvm::cantFail(CPU->setXmm(1, {High, Low}));
    llvm::cantFail(CPU->setReg(X64Register::DX, High));
    unsigned Reads = 0;
    BackendHooks H;
    H.Read = [&](uint64_t A, uint32_t N) {
      EXPECT_EQ(A, Data);
      EXPECT_EQ(N, Size);
      ++Reads;
    };
    expectStopped(run(Bytes, std::move(H)));
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(Reads, Size ? 1u : 0u);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), Expected);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(1)), (RegisterValue{High, Low}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
  };
#define NEVERD_DATA_LOAD(Name, Size, L, H, ...)                                \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Size, {L, H}, {__VA_ARGS__});                                        \
  }
#define NEVERD_DATA_REGISTER(Name, L, H, ...)                                  \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(0, {L, H}, {__VA_ARGS__});                                           \
  }
#include "X64DataCases.def"
#undef NEVERD_DATA_REGISTER
#undef NEVERD_DATA_LOAD
}

TEST_P(X64Data, VectorStorePermissionFaultPreservesBothWords) {
  seedVector();
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  auto Exit = run(StoreXmm);
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Write);
  std::array<uint8_t, VectorBytes> Bytes{};
  llvm::cantFail(CPU->snapshotBacking(Data, Bytes));
  for (unsigned N = 0; N < WordBytes; ++N) {
    EXPECT_EQ(Bytes[N], uint8_t(Low >> (N * CHAR_BIT)));
    EXPECT_EQ(Bytes[N + WordBytes], uint8_t(High >> (N * CHAR_BIT)));
  }
  EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{Low, High}));
}

void X64DataBase::arithmetic(uint64_t DAZ, llvm::ArrayRef<uint8_t> Selected) {
#if defined(__x86_64__) || defined(_M_X64)
  if (DAZ && !(daz_test::hostMXCSRMask() & DAZ))
    GTEST_SKIP() << daz_test::HostUnavailable;
#define NEVERD_FP_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FPCases.def"
#undef NEVERD_FP_BYTES
  struct Input {
    uint64_t DoubleA, DoubleB;
    uint32_t FloatA, FloatB;
  };
  const Input Inputs[] = {
#define NEVERD_SSE_INPUT(Name, DA, DB, FA, FB) {DA, DB, FA, FB},
#include "X64DataCases.def"
#undef NEVERD_SSE_INPUT
  };
  auto Check = [&](unsigned Width, bool Double,
                   llvm::ArrayRef<uint8_t> Instruction) {
    if (!Selected.empty() && Selected != Instruction)
      return;
#ifdef _WIN32
    std::vector<uint8_t> Oracle(std::begin(Win64Before), std::end(Win64Before));
    const auto After = llvm::ArrayRef<uint8_t>(Win64After);
#else
    std::vector<uint8_t> Oracle(std::begin(SysVBefore), std::end(SysVBefore));
    const auto After = llvm::ArrayRef<uint8_t>(SysVAfter);
#endif
    Oracle.insert(Oracle.end(), Instruction.begin(), Instruction.end());
    Oracle.insert(Oracle.end(), After.begin(), After.end());
    std::error_code EC;
    auto Block = llvm::sys::Memory::allocateMappedMemory(
        PageSize, nullptr,
        llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    auto Release = llvm::scope_exit(
        [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
    std::memcpy(Block.base(), Oracle.data(), Oracle.size());
    EC = llvm::sys::Memory::protectMappedMemory(
        Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Oracle.size());
    auto Execute = reinterpret_cast<void (*)(void *, void *, void *, void *)>(
        Block.base());
    for (const auto &I : Inputs) {
      RegisterValue A, B;
      if (Double) {
        A = {I.DoubleA, I.DoubleA};
        B = {I.DoubleB, I.DoubleB};
      } else {
        const uint64_t FA = I.FloatA | (uint64_t(I.FloatA) << FloatLaneShift);
        const uint64_t FB = I.FloatB | (uint64_t(I.FloatB) << FloatLaneShift);
        A = {FA, FA};
        B = {FB, FB};
      }
      for (uint64_t Rounding :
           {uint64_t(0), RoundingDown, RoundingUp, RoundingTruncate}) {
        for (uint64_t Flush : {uint64_t(0), FlushToZero}) {
          X64MachineState Seed;
          Seed.Xmm[0] = A;
          Seed.Xmm[1] = B;
          Seed.MXCSR = InitialMXCSR | Rounding | Flush | DAZ;
          alignas(x64::fp::RegisterSlotBytes)
              std::array<uint8_t, x64::fp::LegacyBytes>
                  Input{}, Output{}, Host{};
          llvm::cantFail(
              encodeX64FXState(Seed, Input, x64::fp::ArchitecturalMXCSRMask));
          Execute(Input.data(), Output.data(), Host.data(), nullptr);
          X64MachineState Expected;
          llvm::cantFail(decodeX64FXState(Expected, Output));
          for (bool Memory : {false, true}) {
            std::vector<uint8_t> Bytes(Instruction.begin(), Instruction.end());
            if (Memory)
              Bytes.back() = MemoryModRM;
            llvm::cantFail(CPU->setXmm(0, A));
            llvm::cantFail(CPU->setXmm(1, B));
            llvm::cantFail(CPU->setReg(X64Register::MXCSR, Seed.MXCSR));
            llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
            llvm::cantFail(CPU->writeInteger(Data, B[0], WordBytes));
            llvm::cantFail(
                CPU->writeInteger(Data + WordBytes, B[1], WordBytes));
            unsigned Reads = 0;
            BackendHooks Hooks;
            Hooks.Read = [&](uint64_t Address, uint32_t Size) {
              EXPECT_EQ(Address, Data);
              EXPECT_EQ(Size, Width);
              ++Reads;
            };
            expectStopped(run(Bytes, std::move(Hooks)));
            ASSERT_FALSE(HasFatalFailure());
            EXPECT_EQ(Reads, unsigned(Memory));
            EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), Expected.Xmm[0]);
            EXPECT_EQ(llvm::cantFail(CPU->xmm(1)), B);
            EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)),
                      Expected.MXCSR);
            EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)),
                      InitialFlags);
          }
        }
      }
    }
  };
#define NEVERD_X64_SSE_INSTRUCTION(Name, Width, Alignment, Double, ...)        \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    constexpr uint8_t Bytes[] = {__VA_ARGS__};                                 \
    Check(Width, Double, Bytes);                                               \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "arch/x86_64/X64SSEInstructions.def"
#undef NEVERD_X64_SSE_INSTRUCTION
#else
  GTEST_SKIP();
#endif
}

TEST_P(X64Data, MaskedSSEArithmeticMatchesIndependentHostExecution) {
  arithmetic(0);
}
struct DAZParameter {
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
  std::string name() const {
    return std::string(executionBackendName(Backend)) +
           (Contract == ExecutionContract::CheckedUserX64
                ? MXCSRUserSuffix
                : MXCSRSupervisorSuffix);
  }
};
void PrintTo(const DAZParameter &P, std::ostream *OS) { *OS << P.name(); }
class X64DAZData : public X64DataBase,
                   public testing::WithParamInterface<DAZParameter> {
  Parameter parameter() const override {
    return {GetParam().Backend, GetParam().Contract};
  }
};
#define NEVERD_X64_SSE_INSTRUCTION(Name, Width, Alignment, Double, ...)        \
  TEST_P(X64DAZData, DAZ##Name##MatchesIndependentHostExecution) {             \
    const auto Mask =                                                          \
        llvm::cantFail(CPU->supportedControlBits(CPURegister::X64MXCSR));      \
    if (!(Mask[0] & x64::fp::DenormalsAreZero))                                \
      GTEST_SKIP();                                                            \
    constexpr uint8_t Bytes[] = {__VA_ARGS__};                                 \
    arithmetic(x64::fp::DenormalsAreZero, Bytes);                              \
  }
#include "arch/x86_64/X64SSEInstructions.def"
#undef NEVERD_X64_SSE_INSTRUCTION

TEST_P(X64Data, SSEMemoryObserverStopsBeforeResultAndStatusChanges) {
  auto Check = [&](unsigned Width, llvm::ArrayRef<uint8_t> Instruction) {
    std::vector<uint8_t> Bytes(Instruction.begin(), Instruction.end());
    Bytes.back() = MemoryModRM;
    seedVector();
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, InitialMXCSR));
    unsigned Reads = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t Address, uint32_t Size) {
      EXPECT_EQ(Address, Data);
      EXPECT_EQ(Size, Width);
      ++Reads;
      CPU->stop();
    };
    expectStopped(run(Bytes, std::move(Hooks)));
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{Low, High}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)), InitialMXCSR);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  };
#define NEVERD_X64_SSE_INSTRUCTION(Name, Width, Alignment, Double, ...)        \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    constexpr uint8_t Bytes[] = {__VA_ARGS__};                                 \
    Check(Width, Bytes);                                                       \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "arch/x86_64/X64SSEInstructions.def"
#undef NEVERD_X64_SSE_INSTRUCTION
}

TEST_P(X64Data, ScalarConversionPreservesStickyStatusAndIgnoresRoundingMode) {
  auto Check = [&](uint64_t Bits, uint64_t Integer, uint64_t Status) {
    for (uint64_t Rounding :
         {uint64_t(0), RoundingDown, RoundingUp, RoundingTruncate}) {
      llvm::cantFail(CPU->setXmm(0, {Bits, High}));
      llvm::cantFail(CPU->setReg(X64Register::MXCSR, InitialMXCSR | Rounding));
      expectStopped(run(ConvertSD));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Integer);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)),
                InitialMXCSR | Rounding | Status);
      EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{Bits, High}));
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
    }
  };
#define NEVERD_DATA_CONVERSION(Name, Bits, Integer, Status)                    \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Bits, Integer, Status);                                              \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "X64DataCases.def"
#undef NEVERD_DATA_CONVERSION
}

TEST_P(X64Data,
       ScalarSubtractionUsesRestoredMXCSRRoundingAndPreservesUpperLane) {
  for (uint64_t Rounding :
       {uint64_t(0), RoundingDown, RoundingUp, RoundingTruncate}) {
    llvm::cantFail(CPU->setXmm(0, {DoubleOne, High}));
    llvm::cantFail(CPU->setXmm(1, {NegativeHalfULP, Low}));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR,
                               InitialMXCSR | Rounding | InvalidStatus));
    auto Saved = llvm::cantFail(CPU->saveContext());
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, InitialMXCSR));
    llvm::cantFail(CPU->restoreContext(*Saved));
    expectStopped(run(SubSD));
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)),
              (RegisterValue{DoubleOne + (Rounding == RoundingUp), High}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)),
              InitialMXCSR | Rounding | InvalidStatus | PrecisionStatus);
  }
  const auto Kind = std::get<0>(GetParam());
  const bool Precise =
      Kind == ExecutionBackendKind::KVM || Kind == ExecutionBackendKind::WHP;
  const uint64_t Before = llvm::cantFail(CPU->reg(X64Register::MXCSR));
  auto Diagnostic = llvm::toString(CPU->setReg(X64Register::MXCSR, 0));
  EXPECT_EQ(Diagnostic.empty(), Precise) << Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)), Precise ? 0 : Before);
}

TEST_P(X64Data, VectorLoadReadObserverStopsBeforeRegisterChanges) {
  seedVector();
  llvm::cantFail(CPU->setXmm(0, {High, Low}));
  BackendHooks H;
  H.Read = [&](uint64_t, uint32_t) { CPU->stop(); };
  expectStopped(run(LoadXmm, std::move(H)));
  EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{High, Low}));
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
}

TEST_P(X64Data, CrossPageVectorStoreReportsTheMissingSecondPage) {
  llvm::cantFail(CPU->setReg(X64Register::CX, Data + PageSize - WordBytes));
  unsigned Writes = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
  auto Exit = run(StoreXmm, std::move(H));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::UnmappedMemory);
  EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
  EXPECT_EQ(Exit.Fault->Size, WordBytes);
  EXPECT_EQ(Writes, 2u);
  std::array<uint8_t, WordBytes> Prefix{};
  llvm::cantFail(CPU->snapshotBacking(Data + PageSize - WordBytes, Prefix));
  EXPECT_EQ(Prefix, (std::array<uint8_t, WordBytes>{}));
}

TEST_P(X64Data, MisalignedAlignedVectorStoreRejectsBeforeObservations) {
  llvm::cantFail(CPU->setReg(X64Register::CX, Data + 1));
  unsigned Writes = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
  EXPECT_EQ(run(AlignedVectorStore, std::move(H)).Kind,
            ExecutionExitKind::GuestTrap);
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Interrupt,
            unsigned(x64::ExceptionVector::GeneralProtection));
  EXPECT_EQ(CPU->fault()->ErrorCode, 0);
  EXPECT_EQ(Writes, 0u);
}

TEST_P(X64Data, MisalignedLockedUpdateRejectsBeforeObservations) {
  llvm::cantFail(CPU->setReg(X64Register::CX, Data + 1));
  unsigned Writes = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
  EXPECT_EQ(run(LockedUpdate, std::move(H)).Kind,
            ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Writes, 0u);
}

TEST_P(X64Data, RepeatCopyStopsAtRestartBoundaryAndHandlesZeroCount) {
  for (bool Backward : {false, true}) {
    for (auto Bytes :
         {llvm::ArrayRef(RepeatMove), llvm::ArrayRef(RepeatMove32)}) {
      llvm::cantFail(CPU->writeInteger(Data, Low, WordBytes));
      llvm::cantFail(CPU->writeInteger(Data + VectorBytes, 0, WordBytes));
      const uint64_t Offset = Backward ? DeviceWidth : 0;
      llvm::cantFail(CPU->setReg(X64Register::SI, Data + Offset));
      llvm::cantFail(CPU->setReg(X64Register::DI, Data + VectorBytes + Offset));
      llvm::cantFail(CPU->setReg(X64Register::CX, CopyCount));
      llvm::cantFail(CPU->setReg(
          X64Register::FLAGS, InitialFlags | (Backward ? DirectionFlag : 0)));
      unsigned Writes = 0;
      BackendHooks H;
      H.Write = [&](uint64_t, uint32_t, uint64_t) {
        if (++Writes == CopyCount)
          CPU->stop();
      };
      expectStopped(run(Bytes, std::move(H)));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), CopyCount - 1);
      expectStopped(run(Bytes));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + VectorBytes, WordBytes)),
                Low);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), 0u);
      // No memory access occurs for a zero count, even with invalid pointers.
      llvm::cantFail(CPU->setReg(X64Register::SI, 0));
      llvm::cantFail(CPU->setReg(X64Register::DI, 0));
      expectStopped(run(Bytes));
      ASSERT_FALSE(HasFatalFailure());
    }
  }
}

class X64Device : public X64Data {
protected:
  unsigned Reads = 0, Writes = 0, Prepares = 0, Validations = 0;
  uint64_t DeviceValue = Value;
  bool FailCommit = false, ThrowRead = false;
  bool StopValidation = false, StopPreparation = false;
  GuestMMIOCallbacks callbacks() {
    GuestMMIOCallbacks IO;
    IO.Validate = [&](uint64_t, uint64_t, bool) {
      ++Validations;
      if (StopValidation)
        CPU->stop();
      return llvm::Error::success();
    };
    IO.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      if (ThrowRead)
        throw std::runtime_error(DeviceFailure);
      ++Reads;
      return DeviceValue++;
    };
    IO.Write = [&](uint64_t, unsigned, uint64_t V) {
      ++Writes;
      DeviceValue = V;
      return llvm::Error::success();
    };
    IO.PrepareRead = [&](uint64_t,
                         unsigned) -> llvm::Expected<GuestMMIOPreparedRead> {
      ++Prepares;
      if (StopPreparation)
        CPU->stop();
      return GuestMMIOPreparedRead{DeviceValue, [&]() -> llvm::Error {
                                     if (FailCommit)
                                       return llvm::createStringError(
                                           llvm::inconvertibleErrorCode(),
                                           DeviceFailure);
                                     ++Reads;
                                     ++DeviceValue;
                                     return llvm::Error::success();
                                   }};
    };
    return IO;
  }
  void mapDevice() {
    llvm::cantFail(CPU->mapMMIO(Alias, PageSize, callbacks()));
    llvm::cantFail(CPU->setReg(X64Register::CX, Alias));
  }
  void prepareCopy() {
    mapDevice();
    llvm::cantFail(CPU->setReg(X64Register::SI, Alias));
    llvm::cantFail(CPU->setReg(X64Register::DI, Data));
    llvm::cantFail(CPU->setReg(X64Register::CX, CopyCount));
  }
};

TEST_P(X64Device, ScalarDeviceAccessIsOneTransactionAndNeverRAM) {
  mapDevice();
  EXPECT_NE(llvm::toString(CPU->validateBacking(Alias, DeviceWidth)), "");
  llvm::cantFail(CPU->setReg(X64Register::AX, UINT64_MAX));
  expectStopped(run(LoadDevice));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Value);
  EXPECT_EQ(Reads, 1u);
  EXPECT_EQ(Prepares, 0u);
  expectStopped(run(StoreDevice));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(DeviceValue, Value);
  llvm::cantFail(CPU->unmapMMIO(Alias, PageSize));
  llvm::cantFail(CPU->map(Alias, PageSize, Read | Write));
  llvm::cantFail(CPU->writeInteger(Alias, Updated, DeviceWidth));
  expectStopped(run(LoadDevice));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Updated);
  EXPECT_EQ(Reads, 1u);
}

TEST_P(X64Device, ReadObserverStopDoesNotConsumeDeviceRead) {
  mapDevice();
  BackendHooks H;
  H.Read = [&](uint64_t, uint32_t) { CPU->stop(); };
  expectStopped(run(LoadDevice, std::move(H)));
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(DeviceValue, Value);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
}

TEST_P(X64Device, PreparedStringReadCommitsOnlyAfterWriteObserverAdmitsIt) {
  prepareCopy();
  unsigned Observations = 0;
  BackendHooks H;
  H.Write = [&](uint64_t A, uint32_t Size, uint64_t V) {
    EXPECT_EQ(A, Data + Observations * DeviceWidth);
    EXPECT_EQ(Size, DeviceWidth);
    EXPECT_EQ(V, Value + Observations);
    if (++Observations == CopyCount)
      CPU->stop();
  };
  expectStopped(run(RepeatMove, std::move(H)));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(Prepares, CopyCount);
  EXPECT_EQ(Reads, CopyCount - 1);
  EXPECT_EQ(DeviceValue, Value + 1);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, DeviceWidth)), Value);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + DeviceWidth, DeviceWidth)),
            0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), 1u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SI)), Alias + DeviceWidth);
  expectStopped(run(RepeatMove));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(Reads, CopyCount);
  EXPECT_EQ(Prepares, CopyCount + 1);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + DeviceWidth, DeviceWidth)),
            Value + 1);
}

TEST_P(X64Device, InvalidDestinationPreventsDevicePreparationAndEffects) {
  prepareCopy();
  llvm::cantFail(CPU->protect(Data, PageSize, Read));
  auto Exit = run(RepeatMove);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault);
  EXPECT_EQ(Prepares, 0u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), CopyCount);
}

TEST_P(X64Device, ValidationStopPreventsPreparationAndWriteObservation) {
  prepareCopy();
  StopValidation = true;
  unsigned Observations = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Observations; };
  expectStopped(run(RepeatMove, std::move(H)));
  EXPECT_EQ(Prepares, 0u);
  EXPECT_EQ(Observations, 0u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, DeviceWidth)), 0u);
}

TEST_P(X64Device, PreparationStopPreventsWriteObservationAndReadCommit) {
  prepareCopy();
  StopPreparation = true;
  unsigned Observations = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Observations; };
  expectStopped(run(RepeatMove, std::move(H)));
  EXPECT_EQ(Prepares, 1u);
  EXPECT_EQ(Observations, 0u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, DeviceWidth)), 0u);
}

TEST_P(X64Device, DeviceCommitFailureIsTerminalWithoutAdvancingOrWritingRAM) {
  prepareCopy();
  FailCommit = true;
  auto Saved = llvm::cantFail(CPU->saveContext());
  auto Exit = run(RepeatMove);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::DeviceFailure) << Exit.Diagnostic;
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), CopyCount);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SI)), Alias);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  EXPECT_NE(llvm::toString(CPU->restoreContext(*Saved)), "");
  std::array<uint8_t, DeviceWidth> Bytes{};
  llvm::cantFail(CPU->snapshotBacking(Data, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, DeviceWidth>{}));
}

TEST_P(X64Device, CallbackExceptionIsADeviceFailure) {
  mapDevice();
  ThrowRead = true;
  auto Exit = run(LoadDevice);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::DeviceFailure);
  EXPECT_EQ(Reads, 0u);
}

TEST_P(X64Device, ReadModifyWriteDeviceAccessFailsBeforeEveryDeviceCallback) {
  mapDevice();
  auto Exit = run(DeviceUpdate);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Validations, 0u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(Writes, 0u);
}

TEST_P(X64Device, LegacyReadCallbackCannotBeSpeculatedForStringWritePreview) {
  auto IO = callbacks();
  IO.PrepareRead = {};
  llvm::cantFail(CPU->mapMMIO(Alias, PageSize, std::move(IO)));
  llvm::cantFail(CPU->setReg(X64Register::SI, Alias));
  llvm::cantFail(CPU->setReg(X64Register::DI, Data));
  llvm::cantFail(CPU->setReg(X64Register::CX, CopyCount));
  EXPECT_EQ(run(RepeatMove).Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Validations, 0u);
  EXPECT_EQ(Reads, 0u);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, X64Device,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::CheckedX64)));

TEST_P(X64Data, UnmodeledFormsRemainExplicitlyUnsupported) {
  // An unsupported exit is terminal, so each independent form uses a fresh CPU.
  for (auto Bytes :
       {llvm::ArrayRef(X87LoadZero), llvm::ArrayRef(LockedStringMove),
        llvm::ArrayRef(AVXMove), llvm::ArrayRef(MMXMove),
        llvm::ArrayRef(LockedBitTestMemory)}) {
    SetUp();
    ASSERT_TRUE(CPU);
    auto Exit = run(Bytes);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
  }
}

TEST_P(X64Address, UnalignedExchangesPreserveAtomicMemoryAndAddressRegisters) {
  llvm::cantFail(
      CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
  const auto Encoding = [](unsigned Size, bool Locked) {
    std::vector<uint8_t> Bytes;
    if (Locked)
      Bytes.push_back(0xf0);
    if (Size == 2)
      Bytes.push_back(0x66);
    if (Size == 8)
      Bytes.push_back(0x48);
    // XCHG [RDX], DL/DX/EDX/RDX: the destination register also owns the
    // original address, so a premature register update corrupts the store.
    Bytes.insert(Bytes.end(), {uint8_t(Size == 1 ? 0x86 : 0x87), 0x12});
    return Bytes;
  };
  for (unsigned Size : {1u, 2u, 4u, 8u}) {
    const uint64_t Mask = UINT64_MAX >> ((WordBytes - Size) * CHAR_BIT);
    for (uint64_t Offset :
         {uint64_t(1), uint64_t(3), uint64_t(7), uint64_t(61), PageSize - 3}) {
      const uint64_t Address = Data + Offset;
      for (bool Locked : {false, true}) {
        for (unsigned Stop = 0; Stop != 3; ++Stop) {
          SCOPED_TRACE(testing::Message() << Size << '/' << Offset << '/'
                                          << Locked << '/' << Stop);
          llvm::cantFail(CPU->writeInteger(Address, High, WordBytes));
          llvm::cantFail(CPU->setReg(X64Register::DX, Address));
          unsigned Reads = 0, Writes = 0;
          BackendHooks Hooks;
          Hooks.Read = [&](uint64_t A, uint32_t N) {
            EXPECT_EQ(A, Address);
            EXPECT_EQ(N, Size);
            ++Reads;
            if (Stop == 1)
              CPU->stop();
          };
          Hooks.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
            EXPECT_EQ(Reads, 1u);
            EXPECT_EQ(A, Address);
            EXPECT_EQ(N, Size);
            EXPECT_EQ(V, Address & Mask);
            EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), Address);
            EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)),
                      High);
            ++Writes;
            if (Stop == 2)
              CPU->stop();
          };
          const auto Bytes = Encoding(Size, Locked);
          expectStopped(run(Bytes, std::move(Hooks)));
          ASSERT_FALSE(HasFatalFailure());
          EXPECT_EQ(Reads, 1u);
          EXPECT_EQ(Writes, Stop == 1 ? 0u : 1u);
          EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)),
                    Stop ? High : (High & ~Mask) | (Address & Mask));
          const uint64_t Preserved = Size == 4 ? 0 : Address & ~Mask;
          EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)),
                    Stop ? Address : Preserved | (High & Mask));
          EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
          EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)),
                    Stop ? Code : Code + Bytes.size());
        }
      }
    }
  }
  for (unsigned Size : {2u, 4u, 8u}) {
    for (bool Locked : {false, true}) {
      SetUp();
      ASSERT_TRUE(CPU);
      const uint64_t Address = Data + PageSize - 1;
      llvm::cantFail(CPU->writeInteger(Address, 0x5a, 1));
      llvm::cantFail(CPU->setReg(X64Register::DX, Address));
      const auto Exit = run(Encoding(Size, Locked));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::UnmappedMemory);
      EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), Address);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
      uint8_t Prefix = 0;
      llvm::cantFail(CPU->snapshotBacking(Address, {&Prefix, 1}));
      EXPECT_EQ(Prefix, 0x5a);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    DAZBackends, X64DAZData,
    testing::Values(
        DAZParameter{ExecutionBackendKind::Unicorn,
                     ExecutionContract::CheckedX64},
        DAZParameter{ExecutionBackendKind::Unicorn,
                     ExecutionContract::CheckedUserX64},
        DAZParameter{ExecutionBackendKind::KVM, ExecutionContract::CheckedX64},
        DAZParameter{ExecutionBackendKind::KVM,
                     ExecutionContract::CheckedUserX64},
        DAZParameter{ExecutionBackendKind::WHP, ExecutionContract::CheckedX64},
        DAZParameter{ExecutionBackendKind::WHP,
                     ExecutionContract::CheckedUserX64}),
    [](const auto &Info) { return Info.param.name(); });

INSTANTIATE_TEST_SUITE_P(
    Backends, X64Data,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::CheckedX64,
                                     ExecutionContract::CheckedUserX64)));
} // namespace
} // namespace neverd::emulation
