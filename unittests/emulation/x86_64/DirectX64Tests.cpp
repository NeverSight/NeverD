//===- DirectX64Tests.cpp - Bounded x64 execution and fetch watches -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "core/RAMInitializationWatch.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/ExecutionSession.h"

#include <array>

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x10000, Data = 0x20000, PageSize = 0x1000;
constexpr uint64_t Value = 0x123456789abcdef0;

class DirectX64 : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::shared_ptr<ExecutionBudget> Budget;
  void SetUp() override {
    auto Backend =
        createExecutionBackend(GetParam(), ExecutionContract::DirectUserX64,
                               0x100000, GuestArchitecture::X64);
    if (!Backend) {
      auto E = Backend.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(Backend->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize * 2, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Data, 0}));
  }
  std::unique_ptr<ExecutionSession> start(uint64_t Timeout = 1000000) {
    Budget = llvm::cantFail(ExecutionBudget::create({1000000, 1000, Timeout}));
    return llvm::cantFail(ExecutionSession::create(std::move(CPU), Budget));
  }
  uint64_t reg(ExecutionSession &S, CPURegister R) {
    return llvm::cantFail(S.cpu().readRegister(R))[0];
  }
};

TEST_P(DirectX64, SplitInstructionRetiresBeforeTheWatchedPageBoundary) {
  // movabs rax, Value straddles the watch; mov [rcx], rax begins inside it.
  constexpr uint8_t Bytes[] = {0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78,
                               0x56, 0x34, 0x12, 0x48, 0x89, 0x01, 0x90};
  const uint64_t Start = Code + PageSize - 5, Store = Start + 10;
  llvm::cantFail(CPU->write(Start, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + PageSize, PageSize}}));
  auto Exit = llvm::cantFail(S->run(Start, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch)
      << (Exit.CPU ? Exit.CPU->Diagnostic : "no CPU entry")
      << " PC=" << reg(*S, CPURegister::X64PC) << " fault="
      << (Exit.CPU && Exit.CPU->Fault ? Exit.CPU->Fault->Address.value_or(0)
                                      : 0)
      << " vector="
      << (Exit.CPU && Exit.CPU->Fault ? Exit.CPU->Fault->Interrupt.value_or(0)
                                      : 0);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Store);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 0u);
  Exit = llvm::cantFail(S->run(Store, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Store + 3);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ReadingAWatchedExecutablePageDoesNotTriggerAFetchWatch) {
  constexpr uint8_t Bytes[] = {0x48, 0x8b, 0x01, 0xeb, 0xfe};
  const uint64_t Address = Code + PageSize + 0x80;
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Address, Value, 8));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Address, 0}));
  auto S = start(50000);
  llvm::cantFail(S->watchExecution({{Code + PageSize, PageSize}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::Timeout);
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::Deadline);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, PartialPageWatchStopsOnlyAtTheSelectedInstruction) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0xc0, 0x48, 0xff, 0xc0, 0xeb, 0xf8};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 3, 1}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 1u);
  ASSERT_EQ(llvm::cantFail(S->run(Code + 3, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 3u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ResumedWatchCanRetireOneInstructionAcrossTwoWatchedPages) {
  constexpr uint8_t Bytes[] = {0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a,
                               0x78, 0x56, 0x34, 0x12, 0x90};
  const uint64_t Start = Code + PageSize - 5;
  llvm::cantFail(CPU->write(Start, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code, PageSize * 2}}));
  ASSERT_EQ(llvm::cantFail(S->run(Start, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Start);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  ASSERT_EQ(llvm::cantFail(S->run(Start, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Start + 10);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, WatchEndingAtTheLastAddressCanResumeWithoutWrapping) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0xc0, 0x48, 0xff, 0xc0, 0xeb, 0xf8};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 3, UINT64_MAX - (Code + 3) + 1}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 1u);
  ASSERT_EQ(llvm::cantFail(S->run(Code + 3, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 6);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 2u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, CommittedWriteStopsBeforeTheFollowingInstruction) {
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01, 0x48, 0xff, 0x01, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value);
  Exit = llvm::cantFail(S->run(Code + 3, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 6);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value + 1);
  llvm::cantFail(S->watchMemoryWrites({}));
  Exit = llvm::cantFail(S->run(Code + 6, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, UnobservedStoresInvalidatePhysicalWriteWitnesses) {
  // Two original stores run without a write watch. Cover changed contents,
  // identical-value stores and an ABA history ending with the original bytes.
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01, 0x48, 0x89, 0x11, 0x90};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto Space = CPU->addressSpace();
  auto Written = llvm::cantFail(Space->pinBacking(Data, 8));
  auto Untouched = llvm::cantFail(Space->pinBacking(Data + 8, 8));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 6, 1}}));
  for (unsigned Mode : {0u, 1u, 2u}) {
    SCOPED_TRACE(Mode);
    const uint64_t First = Mode == 1 ? Value : Value + 1;
    const uint64_t Last = Mode == 0 ? Value + 2 : Value;
    llvm::cantFail(S->cpu().writeInteger(Data, Value, 8));
    llvm::cantFail(S->cpu().writeInteger(Data + 8, Value, 8));
    llvm::cantFail(S->cpu().writeRegister(CPURegister::X64AX, {First, 0}));
    llvm::cantFail(S->cpu().writeRegister(CPURegister::X64DX, {Last, 0}));
    auto History = llvm::cantFail(Written.watchWrites());
    auto Copy = History;
    auto Other = llvm::cantFail(Untouched.watchWrites());
    ASSERT_TRUE(llvm::cantFail(History.intact()));
    ASSERT_TRUE(llvm::cantFail(Other.intact()));
    const auto Exit = llvm::cantFail(S->run(Code, 1));
    ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
    EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 6);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Data, 8)), Last);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Data + 8, 8)), Value);
    EXPECT_FALSE(llvm::cantFail(History.intact()));
    EXPECT_FALSE(llvm::cantFail(Copy.intact()));
    // Opaque execution cannot certify even an unchanged, unwatched range.
    EXPECT_FALSE(llvm::cantFail(Other.intact()));
    auto Fresh = llvm::cantFail(Written.watchWrites());
    EXPECT_TRUE(llvm::cantFail(Fresh.intact()));
    EXPECT_FALSE(llvm::cantFail(History.intact()));
  }
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ReadingWriteWatchedRAMDoesNotPublishAWrite) {
  constexpr uint8_t Bytes[] = {0x48, 0x8b, 0x01, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Data, Value, 8));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ResumedSelfBranchReachesTheSameWatchAgain) {
  constexpr uint8_t Bytes[] = {0xeb, 0xfe};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code, 2}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, PartialWatchCanRunALongLoopInTheSamePage) {
  // mov ecx, 10000000; dec ecx; jnz -4; jmp to the watched instruction.
  constexpr uint8_t Bytes[] = {0xb9, 0x80, 0x96, 0x98, 0x00, 0xff,
                               0xc9, 0x75, 0xfc, 0xeb, 0x75};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  auto S = start(5000000);
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, FragmentedWatchesAllowRepeatedPageCrossings) {
  constexpr uint8_t Start[] = {0xb9, 0xe8, 0x03, 0x00, 0x00,
                               0xe9, 0xf6, 0x0f, 0x00, 0x00};
  constexpr uint8_t Loop[] = {0xff, 0xc9, 0x0f, 0x85, 0xfd, 0xef, 0xff,
                              0xff, 0xe9, 0x73, 0xf0, 0xff, 0xff};
  llvm::cantFail(CPU->write(Code, Start));
  llvm::cantFail(CPU->write(Code + PageSize, Loop));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  std::vector<ExecutionWatch> Watches{{Code + 128, 1}};
  for (uint64_t I = 0; I < 100000; ++I)
    Watches.push_back({0x1000000 + I * 32, 1});
  auto S = start(10000000);
  llvm::cantFail(S->watchExecution(Watches));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, WatchedCrossPageInstructionStopsBeforeItsEffects) {
  constexpr uint8_t Jump[] = {0xe9, 0xf9, 0x0f, 0, 0};
  constexpr uint8_t Tail[] = {0x48, 0xff, 0xc0, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Jump));
  llvm::cantFail(CPU->write(Code + PageSize - 2, Tail));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + PageSize - 2, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + PageSize - 2);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, FullNamespacePageCoverDoesNotWrap) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0xc0, 0x90};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{0, UINT64_MAX}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 1u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, AliasedCodeWriteInvalidatesAPlannedBranch) {
  // Change a forward jump from the service instruction to the watch, through
  // another mapping of the code's physical page. The private code guard must
  // replan before that branch without publishing a public memory-write stop.
  constexpr uint64_t Alias = 0x30000;
  constexpr uint8_t Bytes[] = {0xc6, 0x01, 0x7b, 0xeb, 0x00, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  llvm::cantFail(
      CPU->mapAlias(Alias, Code, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Alias + 4, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Code + 4, 1)), 0x7bu);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, InactivePagePlansRevalidateWritesThroughPhysicalAliases) {
  constexpr uint64_t Alias = 0x30000;
  constexpr uint8_t Jump[] = {0xe9, 0xfb, 0x0f, 0, 0};
  constexpr uint8_t Watch[] = {0x48, 0xff, 0xc0, 0x0f, 0x0b};
  // Rewrite the first page's jump through an alias while the second page
  // executes. The old plan had no stop inside its page; the new one must
  // stop at the watched increment before its effects.
  constexpr uint8_t Rewrite[] = {0xc7, 0x05, 0xf7, 0xef, 0x01, 0,    0x7b, 0,
                                 0,    0,    0xe9, 0xf1, 0xef, 0xff, 0xff};
  llvm::cantFail(CPU->write(Code, Jump));
  llvm::cantFail(CPU->write(Code + 128, Watch));
  llvm::cantFail(CPU->write(Code + PageSize, Rewrite));
  llvm::cantFail(
      CPU->mapAlias(Alias, Code, PageSize, Read | Write | UserAccessible));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Code + 1, 4)), 0x7bu);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, UnwatchedPageCanReturnToAPartialPageWatch) {
  // A direct branch leaves the partially watched page; an indirect branch on
  // an unwatched page returns to the watch before the increment executes.
  constexpr uint8_t Branch[] = {0xe9, 0xfb, 0x0f, 0x00, 0x00};
  constexpr uint8_t Return[] = {0xff, 0xe1};
  constexpr uint8_t Increment[] = {0x48, 0xff, 0xc0};
  llvm::cantFail(CPU->write(Code, Branch));
  llvm::cantFail(CPU->write(Code + PageSize, Return));
  llvm::cantFail(CPU->write(Code + 128, Increment));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Code + 128, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, GuestTrapFlagOutranksAPartialPageWatch) {
  // push 0x302; popfq; nop; inc rax. TF takes effect after the NOP.
  constexpr uint8_t Bytes[] = {0x68, 0x02, 0x03, 0x00, 0x00,
                               0x9d, 0x90, 0x48, 0xff, 0xc0};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::CPU);
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap);
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 1u);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 7);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(reg(*S, CPURegister::X64FLAGS) & 0x100, 0x100u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, IndirectFrontierDoesNotLeakASingleStepFlag) {
  constexpr uint8_t Indirect[] = {0xff, 0xe0};          // jmp rax
  constexpr uint8_t Flags[] = {0x9c, 0x5a, 0xeb, 0x6c}; // pushfq; pop rdx; jmp
  llvm::cantFail(CPU->write(Code, Indirect));
  llvm::cantFail(CPU->write(Code + 16, Flags));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Code + 16, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64DX) & 0x100, 0u);
  EXPECT_EQ(reg(*S, CPURegister::X64FLAGS) & 0x100, 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ManyControlFlowFrontiersPreservePushedFlags) {
  // Five untaken conditional branches lead to independent indirect branches.
  // A bounded execution-stop plan must retain PUSHFQ's original guest flags.
  constexpr uint8_t Bytes[] = {0x39, 0xc9, 0x9c, 0x5a, 0x75, 0x3a, 0x75, 0x40,
                               0x75, 0x46, 0x75, 0x4c, 0x75, 0x52, 0xeb, 0x70};
  constexpr uint8_t Indirect[] = {0xff, 0xe0};
  llvm::cantFail(CPU->write(Code, Bytes));
  for (uint64_t Offset : {64, 72, 80, 88, 96})
    llvm::cantFail(CPU->write(Code + Offset, Indirect));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Code + 128, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64DX) & 0x100, 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ResumedPushFlagsWritesOnlyTheGuestFlags) {
  constexpr uint8_t Bytes[] = {0x9c, 0x5a, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code, 1}}));
  llvm::cantFail(S->watchMemoryWrites({{Data + 0x7f8, 8}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 1);
  EXPECT_EQ(reg(*S, CPURegister::X64SP), Data + 0x7f8);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data + 0x7f8, 8)), 0x202u);
  llvm::cantFail(S->watchMemoryWrites({}));
  const auto Exit = llvm::cantFail(S->run(Code + 1, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  EXPECT_EQ(reg(*S, CPURegister::X64DX), 0x202u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ResumedPopFlagsPreservesTheNextGuestDebugTrap) {
  constexpr uint8_t Bytes[] = {0x9d, 0x90, 0x48, 0xff, 0xc0};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Data + 0x800, 0x302, 8));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code, 2}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 1);
  EXPECT_EQ(reg(*S, CPURegister::X64SP), Data + 0x808);
  EXPECT_EQ(reg(*S, CPURegister::X64FLAGS) & 0x100, 0x100u);
  const auto Exit = llvm::cantFail(S->run(Code + 1, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap);
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 1u);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 2);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, WriteInAResumedFetchWatchIsCommittedExactlyOnce) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0x01, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  llvm::cantFail(S->watchExecution({{Code, 3}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 0u);
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
  llvm::cantFail(S->watchExecution({}));
  Exit = llvm::cantFail(S->run(Code + 3, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, WriteWatchesFollowPhysicalAliasesAndMappingChanges) {
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01, 0x0f, 0x05};
  constexpr uint64_t Alias = 0x30000, Other = Data + PageSize;
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  llvm::cantFail(CPU->map(Other, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(
      CPU->mapAlias(Alias, Data, PageSize, Read | Execute | UserAccessible));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Alias + 3, 2}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value);
  llvm::cantFail(S->cpu().replaceAliases(
      {{Alias, PageSize}},
      {{Alias, Other, PageSize, Read | Execute | UserAccessible}}));
  // Old backing no longer matches. A normal service boundary ends this run.
  llvm::cantFail(S->cpu().writeRegister(CPURegister::X64AX, {Value + 1, 0}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  llvm::cantFail(S->takeServiceRequest());
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value + 1);
  llvm::cantFail(S->cpu().writeRegister(CPURegister::X64CX, {Other, 0}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Other, 8)), Value + 1);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, RealWriteProtectionFaultOutranksAWriteWatch) {
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::CPU);
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap);
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->PC, Code);
  EXPECT_EQ(Exit.CPU->Fault->Address, Data);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 14u);
  EXPECT_EQ(llvm::cantFail(S->cpu().addressSpace()->readInteger(Data, 8)), 0u);
}

TEST_P(DirectX64, RepWriteWatchPreservesPartialProgressBeforeARealFault) {
  constexpr uint8_t Bytes[] = {0xf3, 0xaa, 0x0f, 0x05}; // rep stosb; syscall
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {0x7f, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {2, 0}));
  llvm::cantFail(
      CPU->writeRegister(CPURegister::X64DI, {Data + PageSize - 1, 0}));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, PageSize}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 1u);
  EXPECT_EQ(reg(*S, CPURegister::X64DI), Data + PageSize);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data + PageSize - 1, 1)),
            0x7fu);
  Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::CPU);
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap);
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->PC, Code);
  EXPECT_EQ(Exit.CPU->Fault->Address, Data + PageSize);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 14u);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 1u);
  EXPECT_EQ(reg(*S, CPURegister::X64DI), Data + PageSize);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ServiceRequestPreservesTheOriginalPCAndRegisters) {
  constexpr uint8_t Bytes[] = {0x0f, 0x05, 0x90};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest)
      << Exit.CPU->Diagnostic;
  auto Request = llvm::cantFail(S->takeServiceRequest());
  EXPECT_EQ(Request.PC, Code);
  EXPECT_EQ(Request.NextPC, Code + 2);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), Data);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, InvalidOpcodeIsAFaultAtItsOriginalPC) {
  constexpr uint8_t Bytes[] = {0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto View = llvm::cantFail(CPU->addressSpace()->pinBacking(Data, 8));
  auto History = llvm::cantFail(View.watchWrites());
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 6u);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  EXPECT_FALSE(llvm::cantFail(History.intact()));
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, DeadlinePublishesCommittedRegistersAtAnInstructionBoundary) {
  // inc rax; jmp back. No checked instruction credits are charged.
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0xc0, 0xeb, 0xfb};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto View = llvm::cantFail(CPU->addressSpace()->pinBacking(Data, 8));
  auto History = llvm::cantFail(View.watchWrites());
  auto S = start(50000);
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::Timeout);
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::Deadline);
  EXPECT_GT(reg(*S, CPURegister::X64AX), 0u);
  const auto PC = reg(*S, CPURegister::X64PC);
  EXPECT_TRUE(PC == Code || PC == Code + 3);
  EXPECT_FALSE(llvm::cantFail(History.intact()));
  EXPECT_EQ(Budget->instructions(), 0u);
}

class DirectDriverX64 : public testing::Test {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::shared_ptr<ExecutionBudget> Budget;
  void SetUp() override {
    auto Backend = createExecutionBackend(ExecutionBackendKind::KVM,
                                          ExecutionContract::DirectDriverX64,
                                          0x100000, GuestArchitecture::X64);
    if (!Backend) {
      auto E = Backend.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(Backend->CPU);
    llvm::cantFail(CPU->map(Code, PageSize * 2, Read | Write | Execute));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Data, 0}));
  }
  std::unique_ptr<ExecutionSession> start(uint64_t Timeout = 1000000) {
    Budget = llvm::cantFail(ExecutionBudget::create({1000000, 1000, Timeout}));
    return llvm::cantFail(ExecutionSession::create(std::move(CPU), Budget));
  }
  uint64_t reg(ExecutionSession &S, CPURegister R) {
    return llvm::cantFail(S.cpu().readRegister(R))[0];
  }
  void rejected(ExecutionSession &S, const SessionExit &Exit, uint64_t PC) {
    ASSERT_TRUE(Exit.CPU);
    EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::BackendFailure)
        << Exit.CPU->Diagnostic;
    EXPECT_EQ(reg(S, CPURegister::X64PC), PC);
    EXPECT_NE(Exit.CPU->Diagnostic.find("direct supervisor execution"),
              std::string::npos);
    auto Resume = S.cpu().runUntilExit(PC, 100000);
    ASSERT_FALSE(bool(Resume));
    llvm::consumeError(Resume.takeError());
  }
};

TEST_F(DirectDriverX64, SupervisorCodeRunsWithoutCheckedInstructionAdmission) {
  // BSWAP16 runs on the processor. Overwrite its undefined result before
  // checking CPL; this test intentionally asserts no BSWAP16 value.
  constexpr uint8_t Bytes[] = {0x66, 0x0f, 0xc8, 0x31, 0xc0,
                               0x8c, 0xc8, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 6u);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 7);
  EXPECT_EQ(reg(*S, CPURegister::X64AX) & 3, 0u);
  EXPECT_NE(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_F(DirectDriverX64, CanonicalSupervisorWatchPreservesCommittedRAM) {
  constexpr uint64_t HighCode = 0xffff800000100000;
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01, 0x90};
  llvm::cantFail(CPU->map(HighCode, PageSize, Read | Write | Execute));
  llvm::cantFail(CPU->write(HighCode, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{HighCode + 3, 1}}));
  const auto Exit = llvm::cantFail(S->run(HighCode, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch)
      << (Exit.CPU ? Exit.CPU->Diagnostic : "no CPU exit");
  EXPECT_EQ(reg(*S, CPURegister::X64PC), HighCode + 3);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value);
}

TEST_F(DirectDriverX64, PageFaultPreservesTheCommittedStringPrefix) {
  constexpr uint8_t Bytes[] = {0xf3, 0xaa, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {0x7f, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {2, 0}));
  llvm::cantFail(
      CPU->writeRegister(CPURegister::X64DI, {Data + PageSize - 1, 0}));
  auto S = start();
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 14u);
  EXPECT_EQ(Exit.CPU->Fault->Address, Data + PageSize);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 1u);
  // Normal CPU accesses remain forbidden after a terminal fault. Inspect
  // committed RAM through the diagnostic snapshot API without clearing it.
  auto Read = S->cpu().readInteger(Data + PageSize - 1, 1);
  ASSERT_FALSE(bool(Read));
  llvm::consumeError(Read.takeError());
  uint8_t Committed = 0;
  ASSERT_EQ(llvm::toString(
                S->cpu().snapshotBacking(Data + PageSize - 1, {&Committed, 1})),
            "");
  EXPECT_EQ(Committed, 0x7fu);
  EXPECT_TRUE(S->cpu().fault());
}

TEST_F(DirectDriverX64, FSBaseWriteSurvivesAWatchAndResume) {
  // WRMSR FS_BASE; mov rax, fs:[0]; ud2.
  constexpr uint8_t Bytes[] = {0xb9, 0x00, 0x01, 0x00, 0xc0, 0xb8, 0x00,
                               0x00, 0x02, 0x00, 0x31, 0xd2, 0x0f, 0x30,
                               0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00,
                               0x00, 0x00, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Data, Value, 8));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 14, 1}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch)
      << (Exit.CPU ? Exit.CPU->Diagnostic : "no CPU exit");
  EXPECT_EQ(reg(*S, CPURegister::X64FSBase), Data);
  llvm::cantFail(S->watchExecution({}));
  Exit = llvm::cantFail(S->run(Code + 14, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(reg(*S, CPURegister::X64FSBase), Data);
}

TEST_F(DirectDriverX64, UnretainedMSRStopsBeforeAccessWithCommittedContext) {
  // Commit a register and a RAM store before the forbidden KERNEL_GS_BASE
  // write.
  constexpr uint8_t Bytes[] = {0x41, 0xff, 0xc0, 0x48, 0x89, 0x01,
                               0xb9, 0x02, 0x01, 0x00, 0xc0, 0x31,
                               0xc0, 0x31, 0xd2, 0x0f, 0x30};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  rejected(*S, Exit, Code + 15);
  EXPECT_EQ(reg(*S, CPURegister::X64R8), 1u);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 0xc0000102u);
  auto Read = S->cpu().readInteger(Data, 8);
  ASSERT_FALSE(bool(Read));
  llvm::consumeError(Read.takeError());
  std::array<uint8_t, 8> Committed{};
  ASSERT_EQ(llvm::toString(S->cpu().snapshotBacking(Data, Committed)), "");
  EXPECT_EQ(Committed, (std::array<uint8_t, 8>{0xf0, 0xde, 0xbc, 0x9a, 0x78,
                                               0x56, 0x34, 0x12}));
  EXPECT_TRUE(S->cpu().fault());
}

TEST_F(DirectDriverX64, ChangedCR0IsRejectedAtTheOriginalExceptionBoundary) {
  // Toggle CR0.WP; ud2. A later entry must never silently reinstall CR0.
  constexpr uint8_t Bytes[] = {0x0f, 0x20, 0xc0, 0x48, 0x35, 0x00, 0x00,
                               0x01, 0x00, 0x0f, 0x22, 0xc0, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  rejected(*S, Exit, Code + 12);
}

TEST_F(DirectDriverX64,
       ChangedCR0IsRejectedWhenTheDeadlineInterruptsExecution) {
  constexpr uint8_t Bytes[] = {0x0f, 0x20, 0xc0, 0x48, 0x35, 0x00, 0x00,
                               0x01, 0x00, 0x0f, 0x22, 0xc0, 0xeb, 0xfe};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start(50000);
  auto Exit = llvm::cantFail(S->run(Code, 1));
  rejected(*S, Exit, Code + 12);
  EXPECT_TRUE(Exit.CPU->DeadlineReached);
}

TEST_F(DirectDriverX64, ChangedDebugRegisterIsRejected) {
  // mov dr0, rax; ud2.
  constexpr uint8_t Bytes[] = {0x0f, 0x23, 0xc0, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Data, 0}));
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  rejected(*S, Exit, Code + 3);
}

TEST_F(DirectDriverX64, ChangedDescriptorTableIsRejected) {
  // sgdt [rcx]; inc word [rcx]; lgdt [rcx]; ud2. Existing selectors remain
  // valid.
  constexpr uint8_t Bytes[] = {0x0f, 0x01, 0x01, 0x66, 0xff, 0x01,
                               0x0f, 0x01, 0x11, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  rejected(*S, Exit, Code + 9);
}

TEST_F(DirectDriverX64, DeadlineCapturesStableSupervisorContext) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0xc0, 0xeb, 0xfb};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start(50000);
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.Kind, SessionExitKind::Timeout);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::Deadline)
      << Exit.CPU->Diagnostic;
  EXPECT_GT(reg(*S, CPURegister::X64AX), 0u);
  const auto PC = reg(*S, CPURegister::X64PC);
  EXPECT_TRUE(PC == Code || PC == Code + 3);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_F(DirectDriverX64, InitializationTracksSameValuePartialAndAliasedStores) {
  constexpr uint64_t Alias = 0x40000;
  // Both stores write zeros into already zero RAM through another VA.
  constexpr uint8_t Bytes[] = {0x88, 0x41, 0x01, 0x66, 0x89,
                               0x41, 0x03, 0x0f, 0x0b};
  llvm::cantFail(CPU->mapAlias(Alias, Data, PageSize, Read | Write));
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Alias, 0}));
  auto Mark = llvm::cantFail(RAMInitializationWatch::create(
      llvm::cantFail(CPU->addressSpace()->pinBacking(Data, 8))));
  auto S = start();
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 7);
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_EQ(Mark->initialized(I), I == 1 || I == 3 || I == 4) << I;
  std::array<uint8_t, 8> After{};
  ASSERT_EQ(llvm::toString(S->cpu().snapshotBacking(Data, After)), "");
  EXPECT_EQ(After, (std::array<uint8_t, 8>{}));
}

TEST_F(DirectDriverX64,
       TemporaryWriteProtectionChangesCannotBypassInitialization) {
  // Save CR0, clear WP, store, restore CR0, then fault. Checking only the
  // final CR0 would miss the write; second-level protection must stop it.
  constexpr uint8_t Bytes[] = {0x0f, 0x20, 0xc0, 0x48, 0x89, 0xc2, 0x48,
                               0x0f, 0xba, 0xf0, 0x10, 0x0f, 0x22, 0xc0,
                               0x48, 0xc7, 0x01, 0x01, 0x00, 0x00, 0x00,
                               0x0f, 0x22, 0xc2, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto Mark = llvm::cantFail(RAMInitializationWatch::create(
      llvm::cantFail(CPU->addressSpace()->pinBacking(Data, 8))));
  auto S = start();
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::BackendFailure);
  EXPECT_NE(Exit.CPU->Diagnostic.find("protected physical write"),
            std::string::npos)
      << Exit.CPU->Diagnostic;
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_FALSE(Mark->initialized(I));
  std::array<uint8_t, 8> After{};
  ASSERT_EQ(llvm::toString(S->cpu().snapshotBacking(Data, After)), "");
  EXPECT_EQ(After, (std::array<uint8_t, 8>{}));
  auto Resume = S->cpu().runUntilExit(Code, 100000);
  ASSERT_FALSE(bool(Resume));
  llvm::consumeError(Resume.takeError());
}

TEST_F(DirectDriverX64,
       TemporaryWriteProtectionChangesCannotBypassWriteWatches) {
  constexpr uint8_t Bytes[] = {0x0f, 0x20, 0xc0, 0x48, 0x89, 0xc2, 0x48,
                               0x0f, 0xba, 0xf0, 0x10, 0x0f, 0x22, 0xc0,
                               0x48, 0xc7, 0x01, 0x01, 0x00, 0x00, 0x00,
                               0x0f, 0x22, 0xc2, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::BackendFailure);
  EXPECT_NE(Exit.CPU->Diagnostic.find("protected physical write"),
            std::string::npos)
      << Exit.CPU->Diagnostic;
  std::array<uint8_t, 8> After{};
  ASSERT_EQ(llvm::toString(S->cpu().snapshotBacking(Data, After)), "");
  EXPECT_EQ(After, (std::array<uint8_t, 8>{}));
}

TEST_F(DirectDriverX64, InitializationTracksScalarXorWritesAndNativeFlags) {
  // Same-value byte XOR, then word/dword/qword XOR through each scalar width.
  // The last XOR is locked so the original processor also owns atomicity.
  constexpr uint8_t Bytes[] = {
      0x80, 0x31, 0x00,                   // xor byte [rcx], 0
      0x66, 0x81, 0x71, 0x02, 0x5a, 0x00, // xor word [rcx+2], 0x5a
      0x31, 0x41, 0x04,                   // xor dword [rcx+4], eax
      0xf0, 0x48, 0x31, 0x41, 0x08,       // lock xor qword [rcx+8], rax
      0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto Mark = llvm::cantFail(RAMInitializationWatch::create(
      llvm::cantFail(CPU->addressSpace()->pinBacking(Data, 17))));
  auto S = start();
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + sizeof(Bytes) - 2);
  for (unsigned I = 0; I < 17; ++I)
    EXPECT_EQ(Mark->initialized(I), I != 1 && I != 16) << I;
  std::array<uint8_t, 17> After{};
  ASSERT_EQ(llvm::toString(S->cpu().snapshotBacking(Data, After)), "");
  EXPECT_EQ(After, (std::array<uint8_t, 17>{0, 0, 0x5a, 0, 0xf0, 0xde, 0xbc,
                                            0x9a, 0xf0, 0xde, 0xbc, 0x9a, 0x78,
                                            0x56, 0x34, 0x12, 0}));
  // Value is positive and nonzero, with even parity in its low byte.
  EXPECT_EQ(reg(*S, CPURegister::X64FLAGS) & 0x8c5u, 0x4u);
}

TEST_F(DirectDriverX64,
       InitializationMarksOneStoreAcrossDifferentPhysicalPages) {
  // Allocate a separate page between the two virtual neighbors.
  llvm::cantFail(CPU->map(0x40000, PageSize, Read | Write));
  llvm::cantFail(CPU->map(Data + PageSize, PageSize, Read | Write));
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(
      CPU->writeRegister(CPURegister::X64CX, {Data + PageSize - 4, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto Mark = llvm::cantFail(RAMInitializationWatch::create(
      llvm::cantFail(CPU->addressSpace()->pinBacking(Data + PageSize - 4, 8))));
  auto S = start();
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_TRUE(Mark->initialized(I)) << I;
  std::array<uint8_t, 8> After{};
  ASSERT_EQ(
      llvm::toString(S->cpu().snapshotBacking(Data + PageSize - 4, After)), "");
  EXPECT_EQ(After, (std::array<uint8_t, 8>{0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56,
                                           0x34, 0x12}));
}

TEST_F(DirectDriverX64, InitializationDoesNotMarkAFaultingCrossPageStore) {
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(
      CPU->writeRegister(CPURegister::X64CX, {Data + PageSize - 4, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto Mark = llvm::cantFail(RAMInitializationWatch::create(
      llvm::cantFail(CPU->addressSpace()->pinBacking(Data + PageSize - 4, 4))));
  auto S = start();
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 14u);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  for (unsigned I = 0; I < 4; ++I)
    EXPECT_FALSE(Mark->initialized(I)) << I;
  std::array<uint8_t, 4> After{};
  ASSERT_EQ(
      llvm::toString(S->cpu().snapshotBacking(Data + PageSize - 4, After)), "");
  EXPECT_EQ(After, (std::array<uint8_t, 4>{}));
}

TEST_F(DirectDriverX64, InitializationRejectsAnUnknownStoreBeforeItsEffects) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0x01, 0x0f, 0x0b}; // inc [rcx]
  llvm::cantFail(CPU->write(Code, Bytes));
  auto Mark = llvm::cantFail(RAMInitializationWatch::create(
      llvm::cantFail(CPU->addressSpace()->pinBacking(Data, 8))));
  auto S = start();
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::BackendFailure);
  EXPECT_NE(Exit.CPU->Diagnostic.find("initialization store"),
            std::string::npos);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_FALSE(Mark->initialized(I)) << I;
  std::array<uint8_t, 8> After{};
  ASSERT_EQ(llvm::toString(S->cpu().snapshotBacking(Data, After)), "");
  EXPECT_EQ(After, (std::array<uint8_t, 8>{}));
}

TEST_F(DirectDriverX64,
       InitializationWatchCreationAndRemovalInvalidateProjection) {
  constexpr uint8_t Move[] = {0x48, 0x89, 0x01, 0x90};
  llvm::cantFail(CPU->write(Code, Move));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 3, 1}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  auto Mark = llvm::cantFail(RAMInitializationWatch::create(
      llvm::cantFail(S->cpu().addressSpace()->pinBacking(Data, 8))));
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_FALSE(Mark->initialized(I));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch)
      << (Exit.CPU ? Exit.CPU->Diagnostic : "no CPU exit");
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_TRUE(Mark->initialized(I));
  Mark.reset();
  constexpr uint8_t Increment[] = {0x48, 0xff, 0x01, 0x90};
  llvm::cantFail(S->cpu().write(Code, Increment));
  Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch)
      << (Exit.CPU ? Exit.CPU->Diagnostic : "no CPU exit");
  auto Read = S->cpu().readInteger(Data, 8);
  ASSERT_TRUE(bool(Read)) << llvm::toString(Read.takeError());
  EXPECT_EQ(*Read, 1u);
}

TEST_F(DirectDriverX64, InitializationTracksTheCompleteUnalignedVectorStore) {
  constexpr uint8_t Bytes[] = {0xf3, 0x0f, 0x7f, 0x01, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Data + 1, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64V0, {Value, Value}));
  auto Mark = llvm::cantFail(RAMInitializationWatch::create(
      llvm::cantFail(CPU->addressSpace()->pinBacking(Data, 18))));
  auto S = start();
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 4);
  for (unsigned I = 0; I < 18; ++I)
    EXPECT_EQ(Mark->initialized(I), I > 0 && I < 17) << I;
}

TEST_F(DirectDriverX64, PublicWriteStepKeepsOtherInitializationPagesProtected) {
  llvm::cantFail(CPU->map(Data + PageSize, PageSize, Read | Write));
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(
      CPU->writeRegister(CPURegister::X64CX, {Data + PageSize - 4, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto Mark = llvm::cantFail(RAMInitializationWatch::create(
      llvm::cantFail(CPU->addressSpace()->pinBacking(Data + PageSize, 4))));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data + PageSize - 4, 4}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch)
      << (Exit.CPU ? Exit.CPU->Diagnostic : "no CPU exit");
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  for (unsigned I = 0; I < 4; ++I)
    EXPECT_TRUE(Mark->initialized(I)) << I;
  auto Read = S->cpu().readInteger(Data + PageSize - 4, 8);
  ASSERT_TRUE(bool(Read)) << llvm::toString(Read.takeError());
  EXPECT_EQ(*Read, Value);
}

INSTANTIATE_TEST_SUITE_P(Transports, DirectX64,
                         testing::Values(ExecutionBackendKind::Unicorn,
                                         ExecutionBackendKind::KVM,
                                         ExecutionBackendKind::WHP),
                         [](const auto &P) {
                           return executionBackendName(P.param);
                         });
} // namespace
} // namespace neverd::emulation
