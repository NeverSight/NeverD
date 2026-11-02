//===- Benchmark.cpp - Shared digest throughput probe --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Measures the first call and warmed throughput of separately linked owners.
/// Timing excludes setup and independent output verification.
///
//===----------------------------------------------------------------------===//

#include "SHA256Internal.h"

#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int Argc, char **Argv) {
  if (Argc != 3)
    return 2;
  const size_t Size = std::stoull(Argv[1]);
  const size_t Iterations = std::stoull(Argv[2]);
  if (Iterations == 0)
    return 2;
  std::vector<uint8_t> Bytes(Size);
  for (size_t I = 0; I < Size; ++I)
    Bytes[I] = static_cast<uint8_t>(I * 37 + I / 7);
  const auto Expected = llvm::SHA256::hash(Bytes);
  const auto FirstStart = std::chrono::steady_clock::now();
  const auto FirstDigest = neverd::sha256(Bytes);
  const auto FirstWall = std::chrono::steady_clock::now() - FirstStart;
  if (FirstDigest != Expected)
    return 1;

  llvm::sys::TimePoint<> Elapsed;
  std::chrono::nanoseconds UserBefore, SystemBefore, UserAfter, SystemAfter;
  llvm::sys::Process::GetTimeUsage(Elapsed, UserBefore, SystemBefore);
  const auto Start = std::chrono::steady_clock::now();
  uint64_t Consumed = 0;
  std::array<uint8_t, 32> Digest{};
  for (size_t I = 0; I < Iterations; ++I) {
    Digest = neverd::sha256(Bytes);
    Consumed += Digest[I % Digest.size()];
  }
  const auto Wall = std::chrono::steady_clock::now() - Start;
  llvm::sys::Process::GetTimeUsage(Elapsed, UserAfter, SystemAfter);
  if (Digest != Expected)
    return 1;
  std::cout
      << "{\"hardware\":"
      << (neverd::detail::sha256UsesHardware() ? "true" : "false")
      << ",\"bytes\":" << Size << ",\"iterations\":" << Iterations
      << ",\"first_wall_ns\":"
      << std::chrono::duration_cast<std::chrono::nanoseconds>(FirstWall).count()
      << ",\"wall_ns\":"
      << std::chrono::duration_cast<std::chrono::nanoseconds>(Wall).count()
      << ",\"cpu_ns\":"
      << (UserAfter + SystemAfter - UserBefore - SystemBefore).count()
      << ",\"digest\":\"" << llvm::toHex(Digest, true)
      << "\",\"consumed\":" << Consumed << "}\n";
}
