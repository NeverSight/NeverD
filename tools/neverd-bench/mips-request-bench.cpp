//===- mips-request-bench.cpp - Repeated native source requests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Warm SDK request profiling with exact source identity on every iteration.

#include "neverd/sdk/NeverDCAPI.h"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

int main(int Argc, char **Argv) {
  if (Argc != 4 && Argc != 5) {
    std::fprintf(stderr, "usage: %s IMAGE REPEATS OUTPUT [HEX_ENTRY]\n",
                 Argv[0]);
    return 2;
  }
  unsigned Repeats = 0;
  const std::string_view Count(Argv[2]);
  const auto Parsed =
      std::from_chars(Count.data(), Count.data() + Count.size(), Repeats);
  if (Parsed.ec != std::errc() || Parsed.ptr != Count.data() + Count.size() ||
      !Repeats)
    return 2;
  neverd_va_t Entry = 0;
  if (Argc == 5) {
    std::string_view Address(Argv[4]);
    if (Address.starts_with("0x") || Address.starts_with("0X"))
      Address.remove_prefix(2);
    const auto End = Address.data() + Address.size();
    const auto VA = std::from_chars(Address.data(), End, Entry, 16);
    if (VA.ec != std::errc() || VA.ptr != End)
      return 2;
  }
  std::unique_ptr<void, decltype(&neverd_session_destroy)> Session(
      neverd_session_create(), neverd_session_destroy);
  if (!Session || !neverd_session_load(Session.get(), Argv[1]))
    return 1;
  std::string Expected;
  const auto CPUStart = std::clock();
  const auto Start = std::chrono::steady_clock::now();
  for (unsigned I = 0; I < Repeats; ++I) {
    std::unique_ptr<const char, decltype(&neverd_free_string)> Source(
        Argc == 5 ? neverd_decompile_functions(Session.get(), Argv[1],
                                               NEVERD_OUTPUT_C, &Entry, 1)
                  : neverd_decompile_all(Session.get(), Argv[1], 0, 0, 250),
        neverd_free_string);
    if (!Source) {
      std::unique_ptr<const char, decltype(&neverd_free_string)> Error(
          neverd_last_error(Session.get()), neverd_free_string);
      std::fprintf(stderr, "%s\n",
                   Error ? Error.get() : "decompilation failed");
      return 1;
    }
    if (I == 0)
      Expected = Source.get();
    else if (Expected != Source.get()) {
      std::fprintf(stderr, "source changed at iteration %u\n", I);
      return 1;
    }
  }
  const double Seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - Start)
          .count();
  const double CPUSeconds = double(std::clock() - CPUStart) / CLOCKS_PER_SEC;
  Session.reset();
  FILE *Output = std::fopen(Argv[3], "wb");
  if (!Output)
    return 1;
  const bool Written = std::fwrite(Expected.data(), 1, Expected.size(),
                                   Output) == Expected.size();
  const int Closed = std::fclose(Output);
  if (!Written || Closed)
    return 1;
  std::printf("{\"requests\":%u,\"wall_seconds\":%.9f,\"cpu_seconds\":%.9f,"
              "\"source_bytes\":%zu}\n",
              Repeats, Seconds, CPUSeconds, Expected.size());
  return 0;
}
