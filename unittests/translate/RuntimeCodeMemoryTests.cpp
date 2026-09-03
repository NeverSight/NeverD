//===- RuntimeCodeMemoryTests.cpp - W^X publication contract ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/RuntimeCodeMemory.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>

using namespace neverd::translate;

namespace {

std::string takeError(llvm::Error Error) {
  return llvm::toString(std::move(Error));
}

void expectErrorCode(llvm::Error Error, RuntimeCodeMemoryErrorCode Expected) {
  ASSERT_TRUE(static_cast<bool>(Error));
  bool Seen = false;
  llvm::handleAllErrors(std::move(Error),
                        [&](const RuntimeCodeMemoryError &Actual) {
                          Seen = true;
                          EXPECT_EQ(Actual.reason(), Expected);
                        });
  EXPECT_TRUE(Seen);
}

TEST(RuntimeCodeMemory, RejectsEmptyAndOverflowingAllocations) {
  llvm::Expected<RuntimeCodeMemory> Empty = RuntimeCodeMemory::allocate(0);
  ASSERT_FALSE(Empty);
  expectErrorCode(Empty.takeError(), RuntimeCodeMemoryErrorCode::InvalidSize);

  llvm::Expected<RuntimeCodeMemory> Overflow =
      RuntimeCodeMemory::allocate(std::numeric_limits<size_t>::max());
  ASSERT_FALSE(Overflow);
  expectErrorCode(Overflow.takeError(),
                  RuntimeCodeMemoryErrorCode::InvalidSize);
}

TEST(RuntimeCodeMemory, PublishesExactlyOnceFromWritableToExecutable) {
  llvm::Expected<RuntimeCodeMemory> Allocation = RuntimeCodeMemory::allocate(8);
  if (!Allocation)
    FAIL() << takeError(Allocation.takeError());
  RuntimeCodeMemory Memory = std::move(*Allocation);

  EXPECT_EQ(Memory.state(), RuntimeCodeMemoryState::Writable);
  EXPECT_EQ(Memory.size(), 8u);
  EXPECT_GE(Memory.allocatedSize(), Memory.size());

  llvm::Expected<llvm::MutableArrayRef<uint8_t>> Writable =
      Memory.writableBytes();
  if (!Writable)
    FAIL() << takeError(Writable.takeError());
  ASSERT_EQ(Writable->size(), 8u);
  EXPECT_EQ((*Writable)[0], 0u);

  constexpr std::array<uint8_t, 4> Bytes = {0x10, 0x20, 0x30, 0x40};
  EXPECT_FALSE(Memory.write(2, Bytes));
  EXPECT_EQ((*Writable)[2], 0x10);
  EXPECT_EQ((*Writable)[5], 0x40);

  llvm::Expected<uintptr_t> EarlyEntry = Memory.entryAddress();
  ASSERT_FALSE(EarlyEntry);
  expectErrorCode(EarlyEntry.takeError(),
                  RuntimeCodeMemoryErrorCode::NotExecutable);

  EXPECT_FALSE(Memory.publish());
  EXPECT_EQ(Memory.state(), RuntimeCodeMemoryState::Executable);

  llvm::Expected<llvm::MutableArrayRef<uint8_t>> Reopened =
      Memory.writableBytes();
  ASSERT_FALSE(Reopened);
  expectErrorCode(Reopened.takeError(),
                  RuntimeCodeMemoryErrorCode::NotWritable);
  expectErrorCode(Memory.write(0, Bytes),
                  RuntimeCodeMemoryErrorCode::NotWritable);
  expectErrorCode(Memory.publish(),
                  RuntimeCodeMemoryErrorCode::AlreadyExecutable);

  llvm::Expected<uintptr_t> Entry = Memory.entryAddress(2);
  if (!Entry)
    FAIL() << takeError(Entry.takeError());
  EXPECT_EQ(*Entry, reinterpret_cast<uintptr_t>(Memory.bytes().data()) + 2);
  EXPECT_EQ(Memory.bytes()[2], 0x10);
  EXPECT_EQ(Memory.bytes()[5], 0x40);
}

TEST(RuntimeCodeMemory, BoundsChecksWritesAndEntryOffsetsWithoutOverflow) {
  llvm::Expected<RuntimeCodeMemory> Allocation = RuntimeCodeMemory::allocate(4);
  if (!Allocation)
    FAIL() << takeError(Allocation.takeError());
  RuntimeCodeMemory Memory = std::move(*Allocation);

  constexpr std::array<uint8_t, 2> Bytes = {0xAA, 0xBB};
  expectErrorCode(Memory.write(3, Bytes),
                  RuntimeCodeMemoryErrorCode::WriteOutOfBounds);
  expectErrorCode(Memory.write(std::numeric_limits<size_t>::max(), Bytes),
                  RuntimeCodeMemoryErrorCode::WriteOutOfBounds);
  EXPECT_FALSE(Memory.publish());

  llvm::Expected<uintptr_t> PastEnd = Memory.entryAddress(4);
  ASSERT_FALSE(PastEnd);
  expectErrorCode(PastEnd.takeError(),
                  RuntimeCodeMemoryErrorCode::EntryOutOfBounds);
}

TEST(RuntimeCodeMemory, ExecutesOnlyAfterHostInstructionsArePublished) {
#if defined(__aarch64__) || defined(_M_ARM64)
  // mov w0, #42; ret
  constexpr std::array<uint8_t, 8> Code = {0x40, 0x05, 0x80, 0x52,
                                           0xC0, 0x03, 0x5F, 0xD6};
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||           \
    defined(_M_IX86)
  // mov eax, 42; ret
  constexpr std::array<uint8_t, 6> Code = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
#elif defined(__arm__) || defined(_M_ARM)
  // mov r0, #42; bx lr
  constexpr std::array<uint8_t, 8> Code = {0x2A, 0x00, 0xA0, 0xE3,
                                           0x1E, 0xFF, 0x2F, 0xE1};
#else
  constexpr std::array<uint8_t, 0> Code = {};
  GTEST_SKIP() << "no native smoke-test encoding for this build target";
#endif

  llvm::Expected<RuntimeCodeMemory> Allocation =
      RuntimeCodeMemory::allocate(Code.size());
  if (!Allocation)
    FAIL() << takeError(Allocation.takeError());
  RuntimeCodeMemory Memory = std::move(*Allocation);
  ASSERT_FALSE(Memory.write(0, Code));
  ASSERT_FALSE(Memory.publish());

  llvm::Expected<uintptr_t> Entry = Memory.entryAddress();
  if (!Entry)
    FAIL() << takeError(Entry.takeError());
  using Function = int (*)();
  const auto FunctionEntry = reinterpret_cast<Function>(*Entry);
  EXPECT_EQ(FunctionEntry(), 42);
}

} // namespace
