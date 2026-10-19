///
/// \file
/// KernelAvlTests.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/ADT/StringExtras.h"

#include <cstdlib>
namespace neverd::emulation {
namespace {
class KernelAvl : public testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000, Table = S, Input = S + 0x1000,
                            Parent = S + 0x2000, Search = S + 0x2010,
                            Flag = S + 0x2020, PC = 0x180001000,
                            Compare = PC + 0x100, Allocate = PC + 0x200,
                            Free = PC + 0x300;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  bool FailAllocation = false, MutateInput = false;
  std::vector<std::string> Events;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T value(llvm::Expected<T> V) {
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return {};
    }
    return std::move(*V);
  }
  template <class T> void bad(llvm::Expected<T> V, llvm::StringRef Text) {
    ASSERT_FALSE(bool(V));
    auto M = llvm::toString(V.takeError());
    EXPECT_NE(M.find(Text.str()), std::string::npos) << M;
  }
  void bad(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    auto M = llvm::toString(std::move(E));
    EXPECT_NE(M.find(Text.str()), std::string::npos) << M;
  }
  void put(uint64_t A, uint64_t V, unsigned W = 8) {
    ok(Memory->writeInteger(A, V, W));
  }
  uint64_t raw(uint64_t A, unsigned W = 8) {
    return value(Memory->readInteger(A, W));
  }
  uint64_t get(uint64_t A, unsigned W = 8) {
    ok(Model->validateGuestAccess(A, W, false));
    return raw(A, W);
  }
  uint64_t nt(const char *N, std::initializer_list<uint64_t> A) {
    return value(Model->call(std::string(N), A));
  }
  void initialize() {
    Model.reset();
    Result = {};
    Events.clear();
    FailAllocation = MutateInput = false;
    Memory = value(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(S, 0x10000, Read | Write));
    ok(Memory->map(PC, 4096, Read | Execute));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    DriverImage I;
    I.Base = PC - 4096;
    I.Entry = PC;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    EXPECT_EQ(nt("RtlInitializeGenericTableAvl",
                 {Table, Compare, Allocate, Free, 0x12345678}),
              0u);
  }
  void SetUp() override { initialize(); }
  uint64_t invoke(const char *Name, std::initializer_list<uint64_t> A) {
    auto Started = Model->call(std::string(Name), A);
    if (!Started) {
      ADD_FAILURE() << llvm::toString(Started.takeError());
      return 0;
    }
    auto C = Model->takeGuestCall();
    if (!C)
      return *Started;
    for (unsigned Step = 0; Step < 128; ++Step) {
      ok(Model->beginGuestCall(C->Token));
      uint64_t R = 0;
      if (C->PC == Compare) {
        const auto L = get(C->Arguments[1], 4), Right = get(C->Arguments[2], 4);
        R = L < Right ? 0 : L > Right ? 1 : 2;
        Events.push_back("C:" + std::to_string(L) + ":" +
                         std::to_string(Right) + ":" + std::to_string(R));
      } else if (C->PC == Allocate) {
        EXPECT_EQ(get(C->Arguments[0] + 96), 0x12345678u);
        R = FailAllocation ? 0
                           : nt("ExAllocatePoolWithTag",
                                {0x200, C->Arguments[1], 0x414c564e});
        Events.push_back("A:" + std::to_string(C->Arguments[1]) + ":" +
                         (R ? "1" : "0") + ":" +
                         std::to_string(nt("RtlNumberGenericTableElementsAvl",
                                           {C->Arguments[0]})));
        if (MutateInput)
          put(Input, 77, 4);
      } else if (C->PC == Free) {
        const auto P = C->Arguments[1];
        Events.push_back("F:" + std::to_string(get(P + 32, 4)) + ":" +
                         std::to_string(nt("RtlNumberGenericTableElementsAvl",
                                           {C->Arguments[0]})) +
                         ":" + std::to_string(raw(C->Arguments[0] + 64, 4)));
        nt("ExFreePoolWithTag", {P, 0x414c564e});
      } else {
        ADD_FAILURE() << "unexpected AVL callback";
        return 0;
      }
      auto Completed = Model->finishGuestCall(C->Token, R);
      if (!Completed) {
        ADD_FAILURE() << llvm::toString(Completed.takeError());
        return 0;
      }
      if (*Completed)
        return **Completed;
      C = Model->takeGuestCall();
      if (!C) {
        ADD_FAILURE() << "missing AVL continuation";
        return 0;
      }
    }
    ADD_FAILURE() << "AVL continuation failed to complete";
    return 0;
  }
  uint64_t insert(unsigned Key) {
    put(Input, Key, 4);
    return invoke("RtlInsertElementGenericTableAvl", {Table, Input, 4, Flag});
  }
  void erase(unsigned Key) {
    put(Input, Key, 4);
    EXPECT_EQ(invoke("RtlDeleteElementGenericTableAvl", {Table, Input}), 1u);
  }
};
TEST_F(KernelAvl, OriginalCallbacksOutputsCountsDepthsAndRootSelectionMatch) {
  struct Step {
    unsigned Sequence;
    char Kind;
    unsigned Key, Result, Search;
    int ParentKey;
    unsigned Count, Depth, Deletes;
    int Root;
    const char *Events;
  };
  const Step Steps[] = {
#define NEVERD_AVL_NATIVE_STEP(S, K, V, R, SR, P, C, D, DC, ROOT, E)           \
  {S, #K[0], V, R, SR, P, C, D, DC, ROOT, E},
#include "AvlNativeCases.def"
#undef NEVERD_AVL_NATIVE_STEP
  };
  ASSERT_EQ(std::size(Steps), 247u);
  unsigned Sequence = UINT32_MAX;
  for (const auto &C : Steps) {
    SCOPED_TRACE(C.Sequence);
    SCOPED_TRACE(C.Kind);
    SCOPED_TRACE(C.Key);
    if (C.Sequence != Sequence) {
      initialize();
      Sequence = C.Sequence;
    }
    Events.clear();
    put(Input, C.Key, 4);
    put(Flag, 0xa5, 1);
    if (C.Kind == 'I' || C.Kind == 'F') {
      put(Parent, 0x11223344);
      invoke("RtlLookupElementGenericTableFullAvl",
             {Table, Input, Parent, Search});
      if (C.Kind == 'I') {
        EXPECT_EQ(get(Search, 4), C.Search);
        if (!C.Search)
          EXPECT_EQ(get(Parent), 0x11223344u);
        else
          EXPECT_EQ(get(get(Parent) + 32, 4), unsigned(C.ParentKey));
      } else {
        Events.clear();
        FailAllocation = true;
      }
      const auto P =
          invoke("RtlInsertElementGenericTableFullAvl",
                 {Table, Input, 4, Flag, get(Parent), get(Search, 4)});
      EXPECT_EQ(get(Flag, 1), C.Result);
      if (C.Kind == 'I') {
        ASSERT_NE(P, 0u);
        EXPECT_EQ(get(P, 4), C.Key);
      } else
        EXPECT_EQ(P, 0u);
      FailAllocation = false;
    } else
      EXPECT_EQ(invoke("RtlDeleteElementGenericTableAvl", {Table, Input}),
                C.Result);
    EXPECT_EQ(llvm::join(Events, ","), C.Events);
    EXPECT_EQ(nt("RtlNumberGenericTableElementsAvl", {Table}), C.Count);
    EXPECT_EQ(nt("RtlIsGenericTableEmptyAvl", {Table}), unsigned(!C.Count));
    EXPECT_EQ(raw(Table + 48, 4), C.Depth);
    EXPECT_EQ(raw(Table + 64, 4), C.Deletes);
    const auto Root = raw(Table + 16);
    if (C.Root < 0)
      EXPECT_EQ(Root, 0u);
    else
      EXPECT_EQ(get(Root + 32, 4), unsigned(C.Root));
  }
}
TEST_F(KernelAvl, CopiesAfterAllocationAndPreservesDuplicateDataAndOwnership) {
  MutateInput = true;
  auto P = insert(42);
  ASSERT_NE(P, 0u);
  EXPECT_EQ(get(P, 4), 77u);
  MutateInput = false;
  put(Input, 77, 4);
  auto Q = invoke("RtlInsertElementGenericTableAvl", {Table, Input, 4, Flag});
  EXPECT_EQ(P, Q);
  EXPECT_EQ(get(Flag, 1), 0u);
  bad(Model->validateGuestAccess(P - 32, 8, false), "opaque AVL node");
  bad(Model->validateGuestAccess(Table + 44, 4, true), "opaque table");
  bad(Model->call("ExFreePool", {P - 32}), "RtlDeleteElementGenericTableAvl");
  bad(Model->call("RtlInitializeGenericTableAvl",
                  {Table, Compare, Allocate, Free, 0}),
      "live table");
  erase(77);
  bad(Model->validateGuestAccess(P, 4, false), "freed");
  // Empty caller-owned storage may be reused and initialized again.
  put(Table, 0);
  bad(Model->call("RtlNumberGenericTableElementsAvl", {Table}),
      "metadata changed");
  nt("RtlInitializeGenericTableAvl", {Table, Compare, Allocate, Free, 0});
}
TEST_F(KernelAvl, InvalidFullPositionsArgumentsAndIrqlDoNotInvokeCallbacks) {
  bad(Model->call("RtlInsertElementGenericTableFullAvl",
                  {Table, Input, 4, Flag, 0, 9}),
      "valid owned search");
  bad(Model->call("RtlInsertElementGenericTableAvl",
                  {Table, Input, 0xffffffff, Flag}),
      "nonnegative");
  bad(Model->call("RtlLookupElementGenericTableFullAvl",
                  {Table, Input, Search, Search}),
      "aliased");
  bad(Model->call("RtlInsertElementGenericTableAvl", {Table, Input, 4, Input}),
      "aliased");
  auto P = insert(1);
  ASSERT_NE(P, 0u);
  bad(Model->call("RtlInsertElementGenericTableFullAvl",
                  {Table, Input, 4, Flag, 0, 0}),
      "valid owned search");
  nt("KfRaiseIrql", {3});
  bad(Model->call("RtlNumberGenericTableElementsAvl", {Table}), "IRQL");
  nt("KeLowerIrql", {0});
  EXPECT_FALSE(Model->takeGuestCall());
  erase(1);
}
TEST_F(KernelAvl,
       PendingCallbacksKeepInputsAndTablesAliveAndRejectRecursiveMutation) {
  const auto Buffer = nt("ExAllocatePoolWithTag", {0x200, 128, 0x414c564e});
  put(Buffer, 1, 4);
  auto Started =
      Model->call("RtlInsertElementGenericTableAvl", {Table, Buffer, 4, Flag});
  ASSERT_TRUE(bool(Started));
  auto C = Model->takeGuestCall();
  ASSERT_TRUE(C);
  ok(Model->beginGuestCall(C->Token));
  EXPECT_EQ(C->PC, Allocate);
  EXPECT_EQ(nt("RtlNumberGenericTableElementsAvl", {Table}), 0u);
  bad(Model->call("ExFreePool", {Buffer}), "input borrowed");
  bad(Model->call("RtlLookupElementGenericTableAvl", {Table, Buffer}),
      "in-flight");
  auto Done = Model->finishGuestCall(C->Token, 0);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  EXPECT_EQ(**Done, 0u);
  EXPECT_EQ(get(Flag, 1), 0u);
  nt("ExFreePool", {Buffer});
}
TEST_F(KernelAvl, UnloadRejectsLiveNodesButEmptyTablesNeedNoDestructor) {
  insert(1);
  put(Model->driverObject() + windows::DriverUnloadOffset, PC);
  ok(Model->finishEntry());
  auto U = Model->beginUnload();
  ASSERT_TRUE(bool(U));
  bad(Model->finishUnload(), "live AVL nodes");
  erase(1);
  ok(Model->finishUnload());
}
void checkAvlDriver(DriverOptions O) {
  auto R = emulateDriver(
      std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_avl.sys", O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->NTStatus, 0u);
  EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
  EXPECT_EQ(R->Messages, (std::vector<std::string>{"avl guest callbacks passed",
                                                   "avl unload passed"}));
}
TEST(AvlDriver, CompiledComparisonAllocationAndFreeCallbacksShareTheCaller) {
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        DriverOptions O;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.Contract = C;
        O.PageTables = Paging;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        checkAvlDriver(O);
      }
}
TEST(AvlDriver, ReferenceExportsUseTheSameGuestCallbackAndTreeOwner) {
  auto *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
       *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.Contract = C;
      O.KernelImage = Image;
      O.KernelPDB = PDB;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      checkAvlDriver(O);
    }
}
} // namespace
} // namespace neverd::emulation
