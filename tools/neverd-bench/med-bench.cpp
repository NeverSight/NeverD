//===- med-bench.cpp - LowIR to MedIR conversion measurements -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Repeated standalone conversion of the same decoded functions. Loading,
/// decoding, verification and textual output are outside the timed region.
/// This measures the converter without pipeline-owned interprocedural hints.
///
//===----------------------------------------------------------------------===//

#include "ResourceLimits.h"

#include "neverd/ir/med/LowToMed.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

llvm::cl::opt<std::string> Input(llvm::cl::Positional, llvm::cl::Required,
                                 llvm::cl::desc("<input image>"));
llvm::cl::opt<unsigned> Repetitions(
    "repetitions", llvm::cl::init(bench::kMedRepetitions),
    llvm::cl::desc("Measured conversions after one discarded warmup"));
llvm::cl::opt<unsigned>
    MaxFunctions("max-functions", llvm::cl::init(bench::kMedMaxFunctions),
                 llvm::cl::desc("Maximum decoded functions (0 means all)"));
llvm::cl::opt<bool> Interleave(
    "interleave",
    llvm::cl::desc("Publish JSON lines and wait for run on stdin per sample"));

void writeRow(llvm::json::Object Row) {
  llvm::outs() << llvm::json::Value(std::move(Row)) << '\n';
  llvm::outs().flush();
}

int fail(llvm::StringRef Message) {
  llvm::errs() << "neverd-med-bench: " << Message << '\n';
  return 1;
}

int run() {
  if (Repetitions == 0)
    return fail("repetitions must be positive");
  setWorkerThreadCount(bench::kMedThreads);
  auto Image = loadBinary(Input.getValue());
  if (!Image)
    return fail(llvm::toString(Image.takeError()));

  PipelineOptions Options;
  Options.DeferMedIR = true;
  Options.EmitDumpOutput = false;
  Options.MaxFunctions = MaxFunctions;
  llvm::LLVMContext Context;
  auto Prepared = Pipeline().run(*Image, Context, Options);
  if (!Prepared.Success)
    return fail(Prepared.Error);
  if (!Prepared.MedIRDeferred || Prepared.LowFuncs.empty())
    return fail("input did not produce native LowIR");

  size_t Blocks = 0, Ops = 0;
  for (const auto &Func : Prepared.LowFuncs)
    for (const auto &Block : Func.Blocks) {
      ++Blocks;
      Ops += Block.Ops.size();
    }

  llvm::json::Array Samples;
  std::string ExpectedDigest;
  llvm::json::Object Result{
      {"input", Input.getValue()},
      {"functions", static_cast<int64_t>(Prepared.LowFuncs.size())},
      {"blocks", static_cast<int64_t>(Blocks)},
      {"ops", static_cast<int64_t>(Ops)}};
  // Use a separate warmup rather than adding one to a caller-supplied count.
  bool Warmup = true;
  unsigned Remaining = Repetitions;
  while (Warmup || Remaining != 0) {
    if (Interleave && !Warmup) {
      std::string Command;
      if (!std::getline(std::cin, Command) || Command != "run")
        return fail("expected run on stdin before each sample");
    }
    llvm::json::Object Sample;
    {
      std::vector<MedFunc> Converted;
      Converted.reserve(Prepared.LowFuncs.size());
      llvm::sys::TimePoint<> Ignored;
      std::chrono::nanoseconds UserBefore, SystemBefore, UserAfter, SystemAfter;
      llvm::sys::Process::GetTimeUsage(Ignored, UserBefore, SystemBefore);
      const auto Start = std::chrono::steady_clock::now();
      for (const auto &Low : Prepared.LowFuncs) {
        LowToMedConverter Converter;
        Converter.setBinaryImage(&*Image);
        Converted.push_back(
            Converter.convert(Low, Image->Arch, Image->abiFormat()));
      }
      const auto End = std::chrono::steady_clock::now();
      llvm::sys::Process::GetTimeUsage(Ignored, UserAfter, SystemAfter);

      for (const auto &Func : Converted)
        if (Func.SkippedSSA || !verifyMedFunc(Func, "med-bench"))
          return fail("conversion skipped SSA or produced invalid MedIR");
      std::string Dump;
      llvm::raw_string_ostream OS(Dump);
      Pipeline::dumpMedIR(Converted, OS);
      const auto Digest = llvm::toHex(
          llvm::SHA256::hash(llvm::arrayRefFromStringRef(Dump)), true);
      if (Warmup)
        ExpectedDigest = Digest;
      else if (Digest != ExpectedDigest)
        return fail("MedIR dump changed between repetitions");
      Sample = llvm::json::Object{
          {"wall_ms",
           std::chrono::duration<double, std::milli>(End - Start).count()},
          {"cpu_ms", std::chrono::duration<double, std::milli>(
                         UserAfter + SystemAfter - UserBefore - SystemBefore)
                         .count()}};
    }
    // Release the conversion and dump before notifying an interleaving driver,
    // so their cleanup cannot compete with the other process's next sample.
    if (Warmup) {
      Result["med_dump_sha256"] = ExpectedDigest;
      if (Interleave) {
        auto Ready = Result;
        Ready["ready"] = true;
        writeRow(std::move(Ready));
      }
      Warmup = false;
      continue;
    }
    Samples.push_back(llvm::json::Object(Sample));
    --Remaining;
    if (Interleave)
      writeRow(std::move(Sample));
  }
  Result["samples"] = std::move(Samples);
  writeRow(std::move(Result));
  return 0;
}

} // namespace

int main(int Argc, char **Argv) {
  llvm::cl::ParseCommandLineOptions(Argc, Argv, "NeverD MedIR benchmark\n");
  try {
    return run();
  } catch (const std::exception &Error) {
    return fail(Error.what());
  }
}
