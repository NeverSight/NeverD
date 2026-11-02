//===- X86MpxMemoryTests.cpp - MPX memory completion tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise whole memory transactions, provider fault order and architectural
/// bank publication without assuming host MPX support or a hardware access
/// order.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/X86/X86MpxMemory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

using Mode = X86MpxMode;
using Result = X86MpxResult;
using Operation = X86MpxMemoryOperation;
using Outcome = X86MpxMemoryOutcome;
using Access = X86MpxMemoryAccess;
using Completion = X86MpxMemoryCompletion;

constexpr uint64_t PointerAddress = 0x1234000000401890;
constexpr uint64_t PointerValue = 0x87654321abcdef01;
constexpr uint64_t Lower = 0x13579bdf2468ace0;
constexpr uint64_t Upper = 0xfdb97531eca86420;
constexpr uint64_t Status = 0x55667702;

uint64_t mask(Mode M) { return M == Mode::Bits64 ? UINT64_MAX : UINT32_MAX; }

uint64_t directory(Mode M) { return M == Mode::Bits64 ? 0x8020 : 0x9004; }

uint64_t table(Mode M) { return M == Mode::Bits64 ? 0x1234b240 : 0x12347240; }

uint64_t directoryEntry(Mode M) {
  return M == Mode::Bits64 ? 0x12345007 : 0x12345003;
}

X86MpxConfiguration configuration() {
  X86MpxConfiguration C;
  C.Supported = true;
  C.OSXSAVE = true;
  C.XCR0 = 0x18;
  C.PrivilegeLevel = 3;
  C.UserConfig = 0x8001;
  C.UserAddressWidthAdjust = 0;
  C.InTransaction = false;
  return C;
}

X86MpxContext context() {
  X86MpxContext C;
  EXPECT_TRUE(C.configure(configuration()));
  for (unsigned I = 0; I != 4; ++I)
    EXPECT_TRUE(
        C.setBounds(I, {{Lower + I, UINT64_MAX}, {Upper + I, UINT64_MAX}}));
  C.setStatus(Status);
  return C;
}

std::array<X86MpxBounds, 4> bank(const X86MpxContext &C) {
  return {*C.bounds(0), *C.bounds(1), *C.bounds(2), *C.bounds(3)};
}

class Memory : public X86MpxMemoryProvider {
public:
  std::vector<Access> Requests;
  std::vector<Completion> Replies;
  std::function<Completion(const Access &)> OnAccess;

  Completion access(const Access &A) override {
    Requests.push_back(A);
    if (OnAccess)
      return OnAccess(A);
    if (Requests.size() > Replies.size()) {
      ADD_FAILURE() << "unexpected memory request";
      return {};
    }
    return Replies[Requests.size() - 1];
  }
};

Result expected(Outcome O) {
  return O == Outcome::Completed ? Result::Completed
         : O == Outcome::Fault   ? Result::MemoryFault
                                 : Result::UnknownMemory;
}

void expectRequest(const Access &A, Operation Op, Mode M, uint64_t Address,
                   unsigned Count, bool Write) {
  EXPECT_EQ(A.Operation, Op);
  EXPECT_EQ(A.Mode, M);
  EXPECT_EQ(A.Address, Address);
  EXPECT_EQ(A.wordBytes(), M == Mode::Bits64 ? 8u : 4u);
  EXPECT_EQ(A.wordCount(), Count);
  EXPECT_EQ(A.writesMemory(), Write);
}

TEST(X86MpxMemory, BoundsLoadPublishesOnlyAfterTheWholeAccessCompletes) {
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (unsigned R = 0; R != 4; ++R)
      for (Outcome O :
           {Outcome::Completed, Outcome::Fault, Outcome::Unavailable}) {
        auto C = context();
        auto Wanted = bank(C);
        unsigned ReadEffects = 0;
        Memory Mem;
        Mem.OnAccess = [&](const Access &A) {
          expectRequest(A, Operation::LoadBounds, M, PointerAddress & mask(M),
                        2, false);
          EXPECT_FALSE(A.Locked);
          EXPECT_EQ(bank(C), Wanted);
          // A fault after reading the first word can retain a device-read
          // effect, but cannot publish the first half of a BNDMOV destination.
          if (O != Outcome::Unavailable)
            ++ReadEffects;
          return Completion{O, {Upper, Lower, 0}};
        };
        EXPECT_EQ(C.load(M, R, PointerAddress, Mem), expected(O));
        if (O == Outcome::Completed)
          Wanted[R] = {{Upper & mask(M), mask(M)}, {Lower & mask(M), mask(M)}};
        EXPECT_EQ(bank(C), Wanted);
        EXPECT_EQ(C.status(), Status);
        EXPECT_EQ(ReadEffects, O == Outcome::Unavailable ? 0u : 1u);
        ASSERT_EQ(Mem.Requests.size(), 1u);
      }
}

TEST(X86MpxMemory, BoundsStoresRetainWidthLockAndProviderCompletion) {
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (unsigned R = 0; R != 4; ++R)
      for (bool Locked : {false, true})
        for (Outcome O :
             {Outcome::Completed, Outcome::Fault, Outcome::Unavailable}) {
          auto C = context();
          const auto Before = bank(C);
          Memory Mem;
          Mem.Replies = {{O, {}}};
          EXPECT_EQ(C.store(M, R, PointerAddress, Locked, Mem), expected(O));
          ASSERT_EQ(Mem.Requests.size(), 1u);
          const auto &A = Mem.Requests[0];
          expectRequest(A, Operation::StoreBounds, M, PointerAddress & mask(M),
                        2, true);
          EXPECT_EQ(A.Locked, Locked);
          EXPECT_EQ(A.Words,
                    (std::array<uint64_t, 3>{(Lower + R) & mask(M),
                                             (Upper + R) & mask(M), 0}));
          EXPECT_EQ(bank(C), Before);
          EXPECT_EQ(C.status(), Status);
        }
}

TEST(X86MpxMemory, TableLoadsAccessOnlyDirectoryAndEntryThenMatchThePointer) {
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (unsigned R = 0; R != 4; ++R)
      for (uint64_t Difference : {0ull, 1ull, 1ull << 32}) {
        auto C = context();
        auto Wanted = bank(C);
        Memory Mem;
        Mem.Replies = {
            {Outcome::Completed, {directoryEntry(M), 0, 0}},
            {Outcome::Completed, {Upper, Lower, PointerValue ^ Difference}}};
        EXPECT_EQ(C.loadTable(M, R, PointerAddress, PointerValue, Mem),
                  Result::Completed);
        ASSERT_EQ(Mem.Requests.size(), 2u);
        expectRequest(Mem.Requests[0], Operation::LoadDirectory, M,
                      directory(M), 1, false);
        expectRequest(Mem.Requests[1], Operation::LoadTable, M, table(M), 3,
                      false);
        for (const auto &A : Mem.Requests) {
          EXPECT_FALSE(A.Locked);
          EXPECT_EQ(A.Words, (std::array<uint64_t, 3>{}));
          EXPECT_NE(A.Address, PointerAddress);
          EXPECT_NE(A.Address, PointerValue);
        }
        const bool Match = (Difference & mask(M)) == 0;
        Wanted[R] = {{Match ? Upper & mask(M) : 0, mask(M)},
                     {Match ? Lower & mask(M) : 0, mask(M)}};
        EXPECT_EQ(bank(C), Wanted);
        EXPECT_EQ(C.status(), Status);
      }
}

TEST(X86MpxMemory, DirectoryFaultsAndInvalidEntriesNeverTouchTheTableOrBank) {
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (unsigned R = 0; R != 4; ++R)
      for (bool Store : {false, true})
        for (Outcome O :
             {Outcome::Completed, Outcome::Fault, Outcome::Unavailable}) {
          auto C = context();
          const auto Before = bank(C);
          Memory Mem;
          Mem.Replies = {{O, {0x100, 0, 0}}};
          const auto Got =
              Store ? C.storeTable(M, R, PointerAddress, PointerValue, Mem)
                    : C.loadTable(M, R, PointerAddress, PointerValue, Mem);
          EXPECT_EQ(Got, O == Outcome::Completed ? Result::BoundsFault
                                                 : expected(O));
          ASSERT_EQ(Mem.Requests.size(), 1u);
          expectRequest(Mem.Requests[0], Operation::LoadDirectory, M,
                        directory(M), 1, false);
          EXPECT_EQ(bank(C), Before);
          EXPECT_EQ(C.status(),
                    O == Outcome::Completed ? directory(M) | 2 : Status);
        }
}

TEST(X86MpxMemory, TableReadFaultForgetsOnlyItsDestinationAfterEveryReadOrder) {
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (unsigned R = 0; R != 4; ++R) {
      std::array<unsigned, 3> Order{0, 1, 2};
      do {
        for (unsigned CompletedWords = 0; CompletedWords != 3;
             ++CompletedWords) {
          auto C = context();
          auto Wanted = bank(C);
          Memory Mem;
          std::vector<unsigned> Reads;
          Mem.OnAccess = [&](const Access &A) {
            EXPECT_EQ(bank(C), Wanted);
            if (A.Operation == Operation::LoadDirectory)
              return Completion{Outcome::Completed, {directoryEntry(M), 0, 0}};
            expectRequest(A, Operation::LoadTable, M, table(M), 3, false);
            for (unsigned I = 0; I < CompletedWords; ++I)
              Reads.push_back(Order[I]);
            return Completion{Outcome::Fault, {Upper, Lower, PointerValue}};
          };
          EXPECT_EQ(C.loadTable(M, R, PointerAddress, PointerValue, Mem),
                    Result::MemoryFault);
          ASSERT_EQ(Mem.Requests.size(), 2u);
          Wanted[R] = {};
          EXPECT_EQ(bank(C), Wanted);
          EXPECT_EQ(C.status(), Status);
          EXPECT_EQ(Reads, (std::vector<unsigned>(
                               Order.begin(), Order.begin() + CompletedWords)));
        }
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
}

TEST(X86MpxMemory, UnavailableTableAccessIsNotAnArchitecturalFault) {
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (bool Store : {false, true}) {
      auto C = context();
      const auto Before = bank(C);
      Memory Mem;
      Mem.Replies = {{Outcome::Completed, {directoryEntry(M), 0, 0}}, {}};
      EXPECT_EQ(Store ? C.storeTable(M, 2, PointerAddress, PointerValue, Mem)
                      : C.loadTable(M, 2, PointerAddress, PointerValue, Mem),
                Result::UnknownMemory);
      EXPECT_EQ(Mem.Requests.size(), 2u);
      EXPECT_EQ(bank(C), Before);
      EXPECT_EQ(C.status(), Status);
    }
}

TEST(X86MpxMemory, TableStoresKeepEveryProviderPartialWriteAndTheFourthField) {
  constexpr uint64_t Canary = 0x1020304050607080;
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (unsigned R = 0; R != 4; ++R) {
      std::array<unsigned, 3> Order{0, 1, 2};
      do {
        for (unsigned CompletedWords = 0; CompletedWords <= 3;
             ++CompletedWords) {
          auto C = context();
          const auto Before = bank(C);
          std::array<uint64_t, 4> Image{Canary, Canary, Canary, Canary};
          auto Wanted = Image;
          const std::array<uint64_t, 3> Values{(Lower + R) & mask(M),
                                               (Upper + R) & mask(M),
                                               PointerValue & mask(M)};
          Memory Mem;
          Mem.OnAccess = [&](const Access &A) {
            if (A.Operation == Operation::LoadDirectory)
              return Completion{Outcome::Completed, {directoryEntry(M), 0, 0}};
            expectRequest(A, Operation::StoreTable, M, table(M), 3, true);
            EXPECT_FALSE(A.Locked);
            EXPECT_EQ(A.Words, Values);
            for (unsigned I = 0; I < CompletedWords; ++I)
              Image[Order[I]] = A.Words[Order[I]];
            return Completion{
                CompletedWords == 3 ? Outcome::Completed : Outcome::Fault, {}};
          };
          EXPECT_EQ(C.storeTable(M, R, PointerAddress, PointerValue, Mem),
                    CompletedWords == 3 ? Result::Completed
                                        : Result::MemoryFault);
          ASSERT_EQ(Mem.Requests.size(), 2u);
          for (unsigned I = 0; I < CompletedWords; ++I)
            Wanted[Order[I]] = Values[Order[I]];
          EXPECT_EQ(Image, Wanted);
          EXPECT_EQ(bank(C), Before);
          EXPECT_EQ(C.status(), Status);
        }
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
}

void expectNoMemory(X86MpxContext &C, Mode M, unsigned R,
                    std::optional<uint64_t> Address,
                    std::optional<uint64_t> Value, Result Expected) {
  const auto Before = bank(C);
  const auto BeforeStatus = C.status();
  Memory Mem;
  EXPECT_EQ(C.load(M, R, Address, Mem), Expected);
  EXPECT_EQ(C.store(M, R, Address, false, Mem), Expected);
  EXPECT_EQ(C.store(M, R, Address, true, Mem), Expected);
  EXPECT_EQ(C.loadTable(M, R, Address, Value, Mem), Expected);
  EXPECT_EQ(C.storeTable(M, R, Address, Value, Mem), Expected);
  EXPECT_TRUE(Mem.Requests.empty());
  EXPECT_EQ(bank(C), Before);
  EXPECT_EQ(C.status(), BeforeStatus);
}

TEST(X86MpxMemory, EnablementAndTransactionsPrecedeEveryMemoryRequest) {
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (unsigned R = 0; R != 4; ++R)
      for (unsigned Case = 0; Case != 4; ++Case) {
        auto C = context();
        auto Config = configuration();
        Result Wanted = Result::UnknownContext;
        if (Case == 0)
          Config = {};
        else if (Case == 1) {
          Config.Supported = false;
          Config.InTransaction.reset();
          Wanted = Result::Disabled;
        } else if (Case == 2)
          Config.InTransaction.reset();
        else {
          Config.InTransaction = true;
          Wanted = Result::UnsupportedTransaction;
        }
        ASSERT_TRUE(C.configure(Config));
        expectNoMemory(C, M, R, {}, {}, Wanted);
        expectNoMemory(C, M, R, PointerAddress, PointerValue, Wanted);
      }
}

TEST(X86MpxMemory, InvalidBanksModesAndMissingOperandsNeverStartMemory) {
  auto C = context();
  for (Mode M : {Mode::Bits32, Mode::Bits64}) {
    for (unsigned R : {4u, 255u})
      expectNoMemory(C, M, R, PointerAddress, PointerValue,
                     Result::InvalidInput);
    expectNoMemory(C, M, 0, {}, PointerValue, Result::UnknownOperand);
    Memory Mem;
    EXPECT_EQ(C.loadTable(M, 0, PointerAddress, {}, Mem),
              Result::UnknownOperand);
    EXPECT_EQ(C.storeTable(M, 0, PointerAddress, {}, Mem),
              Result::UnknownOperand);
    EXPECT_TRUE(Mem.Requests.empty());
  }
  expectNoMemory(C, Mode(255), 0, PointerAddress, PointerValue,
                 Result::InvalidInput);
}

TEST(X86MpxMemory, UnknownStoredBoundsAreNotReplacedWithZero) {
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (bool UpperUnknown : {false, true}) {
      auto C = context();
      auto Bound = *C.bounds(1);
      (UpperUnknown ? Bound.Upper : Bound.Lower).KnownMask &= ~uint64_t(1);
      ASSERT_TRUE(C.setBounds(1, Bound));
      const auto Before = bank(C);
      Memory Mem;
      EXPECT_EQ(C.store(M, 1, PointerAddress, true, Mem),
                Result::UnknownOperand);
      EXPECT_EQ(C.storeTable(M, 1, PointerAddress, PointerValue, Mem),
                Result::UnknownOperand);
      EXPECT_TRUE(Mem.Requests.empty());
      EXPECT_EQ(bank(C), Before);
      EXPECT_EQ(C.status(), Status);
    }
}

TEST(X86MpxMemory, InvalidSegmentsNeverReachMemoryOrAlterState) {
  for (Mode M : {Mode::Bits32, Mode::Bits64})
    for (unsigned S : {6u, 7u, 255u}) {
      auto C = context();
      const auto Before = bank(C);
      Memory Mem;
      EXPECT_EQ(C.load(M, 0, PointerAddress, Mem, X86SegmentRegister(S)),
                Result::InvalidInput);
      EXPECT_EQ(
          C.store(M, 0, PointerAddress, false, Mem, X86SegmentRegister(S)),
          Result::InvalidInput);
      EXPECT_TRUE(Mem.Requests.empty());
      EXPECT_EQ(bank(C), Before);
      EXPECT_EQ(C.status(), Status);
    }
}

} // namespace
