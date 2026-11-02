//===- SortedSearchBenchmark.cpp - Ordered lookup measurements ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compares complete lookup results over caller-supplied hexadecimal
/// keys and queries. Input parsing, reference work and allocation are untimed.
/// Each query runs once; alternate process order when collecting samples.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/SortedSearch.h"

#if NEVERD_BENCH_CODE_OWNER
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if NEVERD_BENCH_HIGHWAY
#include "hwy/targets.h"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
#ifdef _WIN32
uint64_t processCPUTime() {
  FILETIME Created, Exited, Kernel, User;
  if (!GetProcessTimes(GetCurrentProcess(), &Created, &Exited, &Kernel, &User))
    throw std::runtime_error("process CPU clock unavailable");
  const auto Ticks = [](FILETIME Time) {
    return (uint64_t{Time.dwHighDateTime} << 32) | Time.dwLowDateTime;
  };
  return Ticks(Kernel) + Ticks(User);
}
#endif

std::vector<uint64_t> readWords(const char *Path) {
  std::ifstream Input(Path);
  if (!Input)
    throw std::runtime_error("cannot open input");
  std::vector<uint64_t> Words;
  std::string Token;
  while (Input >> Token) {
    std::string_view Digits = Token;
    if (Digits.starts_with("0x") || Digits.starts_with("0X"))
      Digits.remove_prefix(2);
    uint64_t Value;
    const auto [End, Error] = std::from_chars(
        Digits.data(), Digits.data() + Digits.size(), Value, 16);
    if (Error != std::errc{} || End != Digits.data() + Digits.size())
      throw std::runtime_error("invalid hexadecimal unsigned integer");
    Words.push_back(Value);
  }
  if (!Input.eof() || Words.empty())
    throw std::runtime_error("expected nonempty hexadecimal integer input");
  return Words;
}

template <class Lookup>
int measure(const std::vector<uint64_t> &Keys,
            const std::vector<uint64_t> &Queries, const char *Mode, Lookup Find,
            bool Membership = false) {
  std::vector<size_t> Expected, Actual(Queries.size());
  Expected.reserve(Queries.size());
  for (const auto Key : Queries) {
    const auto It = std::lower_bound(Keys.begin(), Keys.end(), Key);
    Expected.push_back(Membership ? size_t(It != Keys.end() && *It == Key)
                                  : static_cast<size_t>(It - Keys.begin()));
  }
  // Resolve dispatch and touch the output before the timed, warm lookup pass.
  if (Find(Queries.front()) != Expected.front())
    throw std::runtime_error("warmup mismatch");
#ifdef _WIN32
  const auto CPUBegin = processCPUTime();
#else
  const auto CPUBegin = std::clock();
#endif
  const auto Begin = std::chrono::steady_clock::now();
  for (size_t I = 0; I != Queries.size(); ++I)
    Actual[I] = Find(Queries[I]);
  const auto End = std::chrono::steady_clock::now();
#ifdef _WIN32
  const auto CPUEnd = processCPUTime();
#else
  const auto CPUEnd = std::clock();
#endif
  if (Actual != Expected)
    throw std::runtime_error("complete result vector mismatch");
#ifndef _WIN32
  if (CPUBegin == std::clock_t(-1) || CPUEnd == std::clock_t(-1))
    throw std::runtime_error("process CPU clock unavailable");
#endif
  const double Wall =
      std::chrono::duration<double, std::nano>(End - Begin).count();
#ifdef _WIN32
  // MSVC clock() measures wall time. GetProcessTimes returns 100-ns CPU ticks.
  const double CPU = 100.0 * double(CPUEnd - CPUBegin);
#else
  const double CPU = 1e9 * double(CPUEnd - CPUBegin) / CLOCKS_PER_SEC;
#endif
  std::cout << "{\"mode\":\"" << Mode << "\",\"keys\":" << Keys.size()
            << ",\"queries\":" << Queries.size() << ",\"wall_ns\":" << Wall
            << ",\"cpu_ns\":" << CPU << ",\"matching\":true,\"sum\":"
            << std::accumulate(Actual.begin(), Actual.end(), uint64_t{0})
            << "}\n";
  return 0;
}
} // namespace

int main(int Argc, char **Argv) {
  try {
    if (Argc != 4 && Argc != 5)
      throw std::runtime_error("usage: neverd-sorted-search-bench "
                               "std|simd|std-contains|contains|owner keys.hex "
                               "queries.hex [SIMD-target]");
    if (Argc == 5) {
      bool Found = false;
#if NEVERD_BENCH_HIGHWAY
      for (const auto Target : hwy::SupportedAndGeneratedTargets()) {
        if (std::string(Argv[4]) == hwy::TargetName(Target)) {
          hwy::SetSupportedTargetsForTest(Target);
          Found = true;
          break;
        }
      }
#endif
      if (!Found)
        throw std::runtime_error("requested SIMD target is unavailable");
    }
    const auto Keys = readWords(Argv[2]);
    const auto Queries = readWords(Argv[3]);
    if (!std::is_sorted(Keys.begin(), Keys.end()))
      throw std::runtime_error("keys must be sorted");
    const std::string Mode = Argv[1];
    if (Mode == "simd")
      return measure(Keys, Queries, "simd", [&](uint64_t Key) {
        return neverd::sorted::lowerBound64(Keys.data(), Keys.size(), Key);
      });
    if (Mode == "std")
      return measure(Keys, Queries, "std", [&](uint64_t Key) {
        return static_cast<size_t>(
            std::lower_bound(Keys.begin(), Keys.end(), Key) - Keys.begin());
      });
    if (Mode == "contains")
      return measure(
          Keys, Queries, "contains",
          [&](uint64_t Key) {
            return neverd::sorted::contains64(Keys.data(), Keys.size(), Key);
          },
          true);
    if (Mode == "std-contains")
      return measure(
          Keys, Queries, "std-contains",
          [&](uint64_t Key) {
            return std::binary_search(Keys.begin(), Keys.end(), Key);
          },
          true);
#if NEVERD_BENCH_CODE_OWNER
    if (Mode == "owner") {
      neverd::BinaryImage Image;
      Image.Arch = neverd::Arch::X64;
      Image.Symbols.reserve(Keys.size());
      for (const auto Key : Keys) {
        neverd::Symbol Sym;
        Sym.Addr = Key;
        Sym.IsFunc = true;
        Image.Symbols.push_back(std::move(Sym));
      }
      const neverd::ExecutableCodeOwnerIndex Index(Image);
      return measure(
          Keys, Queries, "owner",
          [&](uint64_t Key) { return Image.hasFunctionSymbolAt(Key, &Index); },
          true);
    }
#endif
    throw std::runtime_error("mode unavailable in this build");
  } catch (const std::exception &Error) {
    std::cerr << Error.what() << '\n';
    return 1;
  }
}
