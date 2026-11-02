//===- signature-context-bench.cpp - Paired context measurements ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Alternates complete signature applications with both databases resident.
/// Parsing/loading is measured separately; discarded warmups build the indexes.
///
//===----------------------------------------------------------------------===//

#include "ResourceLimits.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace neverd;
using namespace neverd::sigs;

namespace {

llvm::cl::opt<std::string> Input(llvm::cl::Positional, llvm::cl::Required);
llvm::cl::opt<std::string> Before("before", llvm::cl::Required,
                                  llvm::cl::desc("Baseline signature root"));
llvm::cl::opt<std::string> After("after", llvm::cl::Required,
                                 llvm::cl::desc("Compared signature root"));
llvm::cl::opt<std::string>
    EntriesFile("entries", llvm::cl::Required,
                llvm::cl::desc("Hexadecimal function VAs"));
llvm::cl::opt<unsigned> Repetitions(
    "repetitions", llvm::cl::init(bench::kSignatureRepetitions),
    llvm::cl::desc("Measured alternating pairs after a discarded warmup"));
llvm::cl::opt<unsigned> Threads("threads",
                                llvm::cl::init(bench::kSignatureThreads));
using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point Start, Clock::time_point End) {
  return std::chrono::duration<double, std::milli>(End - Start).count();
}

std::string
namesDigest(const std::unordered_map<uint64_t, std::string> &Names) {
  std::vector<std::pair<uint64_t, std::string>> Sorted(Names.begin(),
                                                       Names.end());
  std::sort(Sorted.begin(), Sorted.end());
  llvm::SHA256 Hash;
  for (const auto &[Address, Name] : Sorted) {
    std::array<uint8_t, sizeof(uint64_t)> Encoded{};
    for (unsigned I = 0; I < Encoded.size(); ++I)
      Encoded[I] = Address >> (8 * I);
    Hash.update(Encoded);
    Hash.update(Name);
    Hash.update(llvm::StringRef("\0", 1));
  }
  return llvm::toHex(Hash.final(), true);
}

llvm::Expected<llvm::json::Object> benchmark() {
  auto Reader = Loader::create(std::filesystem::path(Input.getValue()));
  if (!Reader)
    return llvm::createStringError("no native loader for input");
  auto Image = Reader->load(std::filesystem::path(Input.getValue()));
  if (!Image)
    return Image.takeError();
  std::vector<uint64_t> Entries;
  std::ifstream File(EntriesFile.getValue());
  uint64_t Address;
  while (File >> std::hex >> Address)
    Entries.push_back(Address);
  if (!File.eof() || Entries.empty())
    return llvm::createStringError("empty or malformed entry file");
  std::sort(Entries.begin(), Entries.end());
  Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());
  SignatureDB DB[2];
  const std::array<std::string, 2> Roots{Before.getValue(), After.getValue()};
  double Load[2]{}, LoadCPU[2]{};
  for (unsigned V = 0; V < 2; ++V) {
    DB[V].setCache(SignatureCache::fromEnvironment());
    const auto Start = Clock::now();
    const auto CPU = std::clock();
    if (auto Err = DB[V].loadForImage(*Image, Roots[V]))
      return std::move(Err);
    LoadCPU[V] = 1000.0 * (std::clock() - CPU) / CLOCKS_PER_SEC;
    Load[V] = milliseconds(Start, Clock::now());
  }
  llvm::json::Array CPU[2], Wall[2];
  std::string Digests[2];
  size_t NamesCount[2]{};
  for (unsigned Rep = 0; Rep <= Repetitions; ++Rep)
    for (unsigned Order = 0; Order < 2; ++Order) {
      const unsigned V = (Rep + Order) % 2;
      const auto Start = Clock::now();
      const auto StartCPU = std::clock();
      DB[V].apply(*Image, Entries);
      const auto Names = DB[V].buildCandidateNameMap();
      const double ElapsedCPU =
          1000.0 * (std::clock() - StartCPU) / CLOCKS_PER_SEC;
      const double Elapsed = milliseconds(Start, Clock::now());
      const auto Digest = namesDigest(Names);
      if (Rep && Digest != Digests[V])
        return llvm::createStringError(
            "candidate names changed between repetitions");
      Digests[V] = Digest;
      NamesCount[V] = Names.size();
      if (Rep) {
        CPU[V].push_back(ElapsedCPU);
        Wall[V].push_back(Elapsed);
      }
    }
  llvm::json::Object Out{
      {"schema_version", 1},
      {"scope", "complete apply and candidate-name settlement"},
      {"threads", Threads.getValue()},
      {"entries", int64_t(Entries.size())}};
  for (unsigned V = 0; V < 2; ++V)
    Out[V ? "after" : "before"] =
        llvm::json::Object{{"root", Roots[V]},
                           {"load_ms", Load[V]},
                           {"load_cpu_ms", LoadCPU[V]},
                           {"files", int64_t(DB[V].fileCount())},
                           {"modules", int64_t(DB[V].moduleCount())},
                           {"cpu_ms", std::move(CPU[V])},
                           {"wall_ms", std::move(Wall[V])},
                           {"names", int64_t(NamesCount[V])},
                           {"name_digest", Digests[V]}};
  Out["peak_rss_bytes"] = nullptr;
#if defined(__unix__) || defined(__APPLE__)
  struct rusage Usage{};
  if (getrusage(RUSAGE_SELF, &Usage) == 0) {
    uint64_t Bytes = Usage.ru_maxrss;
#if !defined(__APPLE__)
    Bytes *= 1024;
#endif
    Out["peak_rss_bytes"] = Bytes;
  }
#endif
  return Out;
}

} // namespace

int main(int Argc, char **Argv) {
  llvm::cl::ParseCommandLineOptions(Argc, Argv,
                                    "Paired signature context benchmark\n");
  if (!Repetitions || !Threads) {
    llvm::errs() << "Repetitions and threads must be positive.\n";
    return 1;
  }
  setWorkerThreadCount(Threads);
  auto Result = benchmark();
  if (!Result) {
    llvm::errs() << llvm::toString(Result.takeError()) << '\n';
    return 1;
  }
  llvm::outs() << llvm::json::Value(std::move(*Result)) << '\n';
}
