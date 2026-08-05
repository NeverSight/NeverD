//===- neverd-bench.cpp - Benchmark harness for NeverD pipeline -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs the NeverD pipeline on a binary and emits JSON benchmark data
/// (functions, imports, strings, lift results, timings).
///
/// All analysis is performed through the NeverD C API (libneverd).
///
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace llvm;
namespace fs = std::filesystem;

//===----------------------------------------------------------------------===//
// Command-line options
//===----------------------------------------------------------------------===//

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<binary>"),
                                      cl::Required);

static cl::opt<std::string> OutDir("out-dir",
                                   cl::desc("Output directory (default: "
                                            "alongside binary)"),
                                   cl::init(""));

static cl::opt<int> MaxFunc("max-func",
                            cl::desc("Limit to first N functions (0=all)"),
                            cl::init(0));

static cl::opt<bool> Quiet("q", cl::desc("Quiet (warnings only)"));

static cl::opt<bool>
    DecodeOnly("decode-only",
               cl::desc("Only run the raw decode-throughput benchmark "
                        "(skip the full lift/decompile pipeline)"));

//===----------------------------------------------------------------------===//
// Utility helpers
//===----------------------------------------------------------------------===//

namespace {

#ifdef __APPLE__
double getRSSMB() {
  rusage RU{};
  if (getrusage(RUSAGE_SELF, &RU) != 0)
    return 0.0;
  return static_cast<double>(RU.ru_maxrss) / (1024.0 * 1024.0);
}
#else
double getRSSMB() {
#ifndef _WIN32
  rusage RU{};
  if (getrusage(RUSAGE_SELF, &RU) != 0)
    return 0.0;
  return static_cast<double>(RU.ru_maxrss) / 1024.0;
#else
  return 0.0;
#endif
}
#endif

std::string takeLastError(neverd_session_t Sess) {
  const char *Error = neverd_last_error(Sess);
  std::string Result = Error ? Error : "";
  neverd_free_string(Error);
  return Result;
}

void writeFile(const fs::path &Path, const char *Data) {
  if (!Data)
    return;
  std::ofstream F(Path);
  F << Data;
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Main driver
//===----------------------------------------------------------------------===//

int main(int Argc, char *Argv[]) {
  InitLLVM X(Argc, Argv);

  cl::ParseCommandLineOptions(Argc, Argv, "NeverD Benchmark Harness\n");

  fs::path Inp(InputFile.getValue());
  if (!fs::exists(Inp)) {
    WithColor::error() << "file not found: " << Inp.string() << "\n";
    return 1;
  }

  fs::path OutputDir =
      OutDir.empty() ? Inp.parent_path() : fs::path(OutDir.getValue());
  fs::create_directories(OutputDir);

  neverd_session_t Sess = neverd_session_create();
  auto T0 = std::chrono::steady_clock::now();

  if (!neverd_session_load(Sess, Inp.string().c_str())) {
    WithColor::error() << "load failed: " << takeLastError(Sess) << "\n";
    neverd_session_destroy(Sess);
    return 1;
  }

  long LoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - T0)
                    .count();

  if (!Quiet) {
    const char *ArchName = neverd_session_arch_name(Sess);
    const char *FormatName = neverd_session_format_name(Sess);
    outs() << "[" << Inp.filename().string() << "] arch=" << ArchName
           << " format=" << FormatName << " loading...\n";
    neverd_free_string(ArchName);
    neverd_free_string(FormatName);
  }

  auto Base = OutputDir / Inp.stem();

  // Raw decode-throughput benchmark.  Runs independently of the full pipeline
  // so it can be measured on inputs whose later (LLVM) stages are not yet
  // stable; --decode-only stops after it.
  const char *DecodeJson = neverd_bench_decode(Sess);
  if (DecodeJson) {
    writeFile(Base.string() + ".nd.decode.json", DecodeJson);
    if (!Quiet)
      outs() << "  decode bench: " << DecodeJson << "\n";
    neverd_free_string(DecodeJson);
  }

  if (DecodeOnly) {
    neverd_session_destroy(Sess);
    return 0;
  }

  const char *BenchJson = neverd_bench_run(Sess, Inp.string().c_str(), MaxFunc);
  if (!BenchJson) {
    WithColor::error() << "bench failed: " << takeLastError(Sess) << "\n";
    neverd_session_destroy(Sess);
    return 1;
  }

  writeFile(Base.string() + ".nd.bench.json", BenchJson);

  const char *ImportsJson = neverd_imports_json(Sess);
  writeFile(Base.string() + ".nd.imports.json", ImportsJson);

  const char *StringsJson = neverd_strings_json(Sess, 4);
  writeFile(Base.string() + ".nd.strings.json", StringsJson);

  if (!Quiet) {
    outs() << "[" << Inp.filename().string() << "] "
           << "load_ms=" << LoadMs
           << " rss=" << llvm::format("%.1f", getRSSMB()) << "MB\n"
           << "  bench data: " << (Base.string() + ".nd.bench.json") << "\n";
  }

  if (ImportsJson)
    neverd_free_string(ImportsJson);
  if (StringsJson)
    neverd_free_string(StringsJson);
  neverd_free_string(BenchJson);
  neverd_session_destroy(Sess);
  return 0;
}
