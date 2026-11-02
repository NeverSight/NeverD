//===- DigestTests.cpp - Native SHA-256 boundary contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Qualifies the selected digest implementation, bounded reads and
/// cancellation.
///
//===----------------------------------------------------------------------===//

#include "SHA256Internal.h"
#include "gtest/gtest.h"

#include "neverd/support/Limits.h"
#include "neverd/support/LoadProgress.h"
#include "neverd/support/SHA256.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SHA256.h"
#include "llvm/TargetParser/Host.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace neverd {
namespace {

#if defined(NEVERD_DIGEST_REQUIRE_ARM64) && !defined(__aarch64__) &&           \
    !defined(_M_ARM64)
#error "Native ARM64 qualification must not run an emulated x86 digest"
#endif
#if defined(NEVERD_DIGEST_REQUIRE_X86_64) &&                                   \
    ((!defined(__x86_64__) && !defined(_M_X64)) || defined(_M_ARM64EC))
#error "Native x86-64 qualification must compile the x86-64 digest"
#endif

TEST(Digest, SelectedImplementation) {
  const bool Hardware = detail::sha256UsesHardware();
  RecordProperty("hardware_sha256", Hardware ? "true" : "false");
#ifdef NEVERD_SHA256_PORTABLE
  EXPECT_FALSE(Hardware);
#else
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
  // The independently maintained LLVM detector must agree with the narrower
  // query, including hosts where hardware acceleration is unavailable.
  const auto Features = llvm::sys::getHostCPUFeatures();
  EXPECT_EQ(Hardware, Features.lookup("sha") && Features.lookup("sse4.1") &&
                          Features.lookup("ssse3"));
#elif (defined(__aarch64__) || defined(_M_ARM64)) && defined(_WIN32)
  EXPECT_EQ(Hardware, llvm::sys::getHostCPUFeatures().lookup("sha2"));
#endif
  // Native qualification requires acceleration explicitly, so a detector that
  // silently falls back cannot pass merely because the digest is correct.
  if (const char *Expected = std::getenv("NEVERD_TEST_SHA256_HARDWARE")) {
    ASSERT_TRUE(std::strcmp(Expected, "0") == 0 ||
                std::strcmp(Expected, "1") == 0);
    EXPECT_EQ(Hardware, std::strcmp(Expected, "1") == 0);
  }
#endif
}

TEST(Digest, StandardMultiBlockVectors) {
  const llvm::StringRef Message =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  EXPECT_EQ(llvm::toHex(sha256(llvm::arrayRefFromStringRef(Message)), true),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::vector<uint8_t> MillionAs(1000000, 'a');
  EXPECT_EQ(llvm::toHex(sha256(MillionAs), true),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Digest, EveryAlignmentAndPaddingLength) {
  std::vector<uint8_t> Bytes(64 + 4 * 64);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = static_cast<uint8_t>(I * 37 + I / 7);
  const auto Before = Bytes;
  for (size_t Offset = 0; Offset < 64; ++Offset)
    for (size_t Size = 0; Size <= 4 * 64; ++Size) {
      const llvm::ArrayRef<uint8_t> Data(Bytes.data() + Offset, Size);
      ASSERT_EQ(sha256(Data), llvm::SHA256::hash(Data))
          << Offset << ' ' << Size;
    }
  EXPECT_EQ(Bytes, Before);
}

TEST(Digest, InaccessibleTailAndReadOnlyInput) {
#ifdef _WIN32
  SYSTEM_INFO Info{};
  GetSystemInfo(&Info);
  const size_t Page = Info.dwPageSize;
  auto *Memory = static_cast<uint8_t *>(VirtualAlloc(
      nullptr, Page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  ASSERT_NE(Memory, nullptr);
  auto Release = llvm::scope_exit(
      [&] { EXPECT_TRUE(VirtualFree(Memory, 0, MEM_RELEASE)); });
  DWORD Old = 0;
  ASSERT_TRUE(VirtualProtect(Memory + Page, Page, PAGE_NOACCESS, &Old));
#else
  const long PageResult = sysconf(_SC_PAGESIZE);
  ASSERT_GT(PageResult, 0);
  const size_t Page = static_cast<size_t>(PageResult);
  auto *Memory =
      static_cast<uint8_t *>(mmap(nullptr, Page * 2, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0));
  ASSERT_NE(Memory, MAP_FAILED);
  auto Release =
      llvm::scope_exit([&] { EXPECT_EQ(munmap(Memory, Page * 2), 0); });
  ASSERT_EQ(mprotect(Memory + Page, Page, PROT_NONE), 0);
#endif
  ASSERT_GE(Page, 4 * 64U);
  for (size_t I = 0; I < Page; ++I)
    Memory[I] = static_cast<uint8_t>(I * 17 + I / 3);
#ifdef _WIN32
  ASSERT_TRUE(VirtualProtect(Memory, Page, PAGE_READONLY, &Old));
#else
  ASSERT_EQ(mprotect(Memory, Page, PROT_READ), 0);
#endif
  for (size_t Size = 0; Size <= 4 * 64; ++Size) {
    const llvm::ArrayRef<uint8_t> Data(Memory + Page - Size, Size);
    ASSERT_EQ(sha256(Data), llvm::SHA256::hash(Data)) << Size;
  }
  // A stop at the initial checkpoint must not touch even the first byte.
  LoadProgress Stopped;
  Stopped.Stop = [](void *) { return 1; };
  EXPECT_THROW(sha256({Memory + Page, Page}, Stopped), LoadCancelled);
}

struct ProgressState {
  unsigned long long Last = 0;
  unsigned long long Total = 0;
  bool StopAfterChunk = false;

  static void report(void *User, const char *Phase, unsigned long long Done,
                     unsigned long long Total, const char *) {
    auto &State = *static_cast<ProgressState *>(User);
    EXPECT_STREQ(Phase, "hash");
    EXPECT_EQ(Total, State.Total);
    EXPECT_GE(Done, State.Last);
    EXPECT_LE(Done - State.Last, limits::support::kLoadByteCheckpointBytes);
    EXPECT_LE(Done, Total);
    State.Last = Done;
  }
  static int stop(void *User) {
    const auto &State = *static_cast<ProgressState *>(User);
    return State.StopAfterChunk && State.Last != 0;
  }
  LoadProgress control() { return {report, this, stop, this}; }
};

TEST(Digest, CheckpointBoundariesCancellationAndRetry) {
  constexpr size_t Chunk = limits::support::kLoadByteCheckpointBytes;
  std::vector<uint8_t> Bytes(Chunk * 2 + 65, 0xa7);
  for (const size_t Base : {size_t(0), Chunk, Chunk * 2})
    for (const size_t Tail : {0U, 1U, 55U, 56U, 63U, 64U, 65U}) {
      const llvm::ArrayRef<uint8_t> Data(Bytes.data(), Base + Tail);
      ProgressState State{0, Data.size()};
      EXPECT_EQ(sha256(Data, State.control()), llvm::SHA256::hash(Data));
      EXPECT_EQ(State.Last, Data.size());
    }
  ProgressState State{0, Bytes.size(), true};
  EXPECT_THROW(sha256(Bytes, State.control()), LoadCancelled);
  EXPECT_EQ(State.Last, Chunk);
  State = {0, Bytes.size()};
  EXPECT_EQ(sha256(Bytes, State.control()), llvm::SHA256::hash(Bytes));
  EXPECT_EQ(State.Last, Bytes.size());
}

} // namespace
} // namespace neverd
