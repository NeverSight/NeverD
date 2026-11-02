//===- signature-bench.cpp - Signature matching measurements -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Measure selected pattern parsing, indexing and repeated matching against
/// explicit function entries or a deliberately dense negative scan. This does
/// not discover functions or settle names through reference verification.
///
//===----------------------------------------------------------------------===//

#include "ResourceLimits.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/sigs/SignatureMatcher.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace neverd;
using namespace neverd::sigs;

namespace {

llvm::cl::opt<std::string> Input(llvm::cl::Positional, llvm::cl::Required,
                                 llvm::cl::desc("<input image>"));
llvm::cl::opt<std::string>
    Root("sig-base", llvm::cl::init(bench::kSignatureRoot),
         llvm::cl::desc("Signature tree selected by the image profile"));
llvm::cl::opt<std::string> EntriesFile(
    "entries",
    llvm::cl::desc("File of hexadecimal function VAs, one per line"));
llvm::cl::opt<uint64_t>
    Stride("stride", llvm::cl::init(0),
           llvm::cl::desc("Instead test every N bytes of executable segments"));
llvm::cl::opt<unsigned>
    Repetitions("repetitions", llvm::cl::init(bench::kSignatureRepetitions),
                llvm::cl::desc("Measured scans after one discarded warmup"));
llvm::cl::opt<unsigned> IndexRepetitions(
    "index-repetitions", llvm::cl::init(bench::kSignatureIndexRepetitions),
    llvm::cl::desc("Fresh index builds, including the first, before scanning"));
llvm::cl::opt<unsigned> Threads(
    "threads", llvm::cl::init(bench::kSignatureThreads),
    llvm::cl::desc("Worker threads for parsing, indexing and scanning"));

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point Start, Clock::time_point End) {
  return std::chrono::duration<double, std::milli>(End - Start).count();
}

std::string digest(llvm::StringRef Bytes) {
  return llvm::toHex(
      llvm::SHA256::hash(llvm::ArrayRef(
          reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size())),
      true);
}

llvm::Expected<std::vector<uint64_t>> entries(const BinaryImage &Image) {
  std::vector<uint64_t> Result;
  if (Stride != 0) {
    for (const auto &Segment : Image.Segments) {
      if (!Segment.isExecutable())
        continue;
      for (uint64_t Offset = 0; Offset < Segment.Data.size();) {
        if (Offset > UINT64_MAX - Segment.VA)
          return llvm::createStringError("executable address overflow");
        Result.push_back(Segment.VA + Offset);
        if (Stride >= Segment.Data.size() - Offset)
          break;
        Offset += Stride;
      }
    }
  } else {
    auto Text = llvm::MemoryBuffer::getFile(EntriesFile);
    if (!Text)
      return llvm::errorCodeToError(Text.getError());
    llvm::StringRef Remaining = (*Text)->getBuffer();
    while (!Remaining.empty()) {
      auto [Line, Rest] = Remaining.split('\n');
      Remaining = Rest;
      Line = Line.trim();
      if (Line.empty())
        continue;
      Line.consume_front("0x");
      uint64_t Address = 0;
      if (Line.getAsInteger(16, Address))
        return llvm::createStringError("invalid hexadecimal function address");
      Result.push_back(Address);
    }
  }
  std::sort(Result.begin(), Result.end());
  Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
  if (Result.empty())
    return llvm::createStringError("no addresses to scan");
  return Result;
}

struct ScanResult {
  double Milliseconds = 0;
  double CPUMilliseconds = 0;
  uint64_t Hits = 0;
  std::string Digest;
  SignatureMatcher::ScanStatistics Statistics;
};

/// Fingerprint the decisions and candidate order independently of their
/// storage layout. Hashing is outside the measured index construction.
std::string indexDigest(const SignatureMatcher::HashIndex &Index) {
  llvm::SHA256 Hash;
  const auto Add = [&](uint64_t Value) {
    std::array<uint8_t, sizeof(Value)> Bytes;
    llvm::support::endian::write64le(Bytes.data(), Value);
    Hash.update(Bytes);
  };
  Add(Index.Root);
  Add(Index.Nodes.size());
  for (const auto &Node : Index.Nodes) {
    Add(Node.Offset);
    Add(Node.WildcardChild);
    Add(Node.ExactChildren.size());
    for (const auto &Edge : Node.ExactChildren) {
      Add(Edge.Value);
      Add(Edge.Child);
    }
    Add(Node.Candidates.size());
    for (size_t Candidate : Node.Candidates)
      Add(Candidate);
  }
  return llvm::toHex(Hash.final(), true);
}

ScanResult scan(const BinaryImage &Image, llvm::ArrayRef<uint64_t> Entries,
                llvm::ArrayRef<StoredModule> Modules,
                const SignatureMatcher::HashIndex &Index, bool Record) {
  ScanResult Result;
  llvm::SHA256 Hash;
  for (const auto &Segment : Image.Segments) {
    if (!Segment.isExecutable() || Segment.Data.empty())
      continue;
    SignatureMatcher::ScanStatistics Statistics;
    llvm::sys::TimePoint<> Ignored;
    std::chrono::nanoseconds UserBefore, SystemBefore, UserAfter, SystemAfter;
    llvm::sys::Process::GetTimeUsage(Ignored, UserBefore, SystemBefore);
    const auto Start = Clock::now();
    const auto Hits = Record
                          ? SignatureMatcher::findAtAddresses(
                                Segment.Data.data(), Segment.Data.size(),
                                Segment.VA, Entries, Modules, Index, Statistics)
                          : SignatureMatcher::findAtAddresses(
                                Segment.Data.data(), Segment.Data.size(),
                                Segment.VA, Entries, Modules, Index);
    Result.Milliseconds += milliseconds(Start, Clock::now());
    llvm::sys::Process::GetTimeUsage(Ignored, UserAfter, SystemAfter);
    Result.CPUMilliseconds +=
        std::chrono::duration<double, std::milli>(UserAfter + SystemAfter -
                                                  UserBefore - SystemBefore)
            .count();
    Result.Hits += Hits.size();
    for (const auto &Hit : Hits) {
      std::array<uint8_t, 2 * sizeof(uint64_t)> Bytes;
      llvm::support::endian::write64le(Bytes.data(), Hit.Address);
      llvm::support::endian::write64le(Bytes.data() + sizeof(uint64_t),
                                       Hit.Module);
      Hash.update(Bytes);
    }
    Result.Statistics.Entries += Statistics.Entries;
    Result.Statistics.Candidates += Statistics.Candidates;
    Result.Statistics.CRCRequests += Statistics.CRCRequests;
    Result.Statistics.CRCComputations += Statistics.CRCComputations;
    Result.Statistics.CRCBytes += Statistics.CRCBytes;
  }
  Result.Digest = llvm::toHex(Hash.final(), true);
  return Result;
}

llvm::Expected<llvm::json::Object> benchmark() {
  auto Reader = Loader::create(std::filesystem::path(Input.getValue()));
  if (!Reader)
    return llvm::createStringError("no native loader for input");
  auto Image = Reader->load(std::filesystem::path(Input.getValue()));
  if (!Image)
    return Image.takeError();
  auto Entries = entries(*Image);
  if (!Entries)
    return Entries.takeError();
  auto Tree = SignatureDB::treeDirectory(*Image);
  if (!Tree)
    return llvm::createStringError("image has no signature tree");
  auto Available = SignatureDB::listDirectory(
      std::filesystem::path(Root.getValue()) / *Tree);
  if (!Available)
    return Available.takeError();
  const auto Files = SignatureDB::selectForImage(*Image, *Available);
  if (Files.empty())
    return llvm::createStringError("no selected pattern files");

  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Texts;
  std::vector<PatternChunk> Chunks;
  const auto Start = Clock::now();
  for (const auto &File : Files) {
    auto Text = llvm::MemoryBuffer::getFile(File.string());
    if (!Text)
      return llvm::errorCodeToError(Text.getError());
    auto Parts = PatternParser::splitChunks((*Text)->getBuffer());
    for (auto &Part : Parts)
      Chunks.push_back(std::move(Part));
    Texts.push_back(std::move(*Text));
  }
  const auto Read = Clock::now();
  auto Bytes = PatternParser::reserveBytes(Chunks);
  PatternParser::parseChunks(Chunks);
  if (auto Error = PatternParser::firstError(Chunks))
    return std::move(Error);
  std::vector<StoredModule> Modules;
  size_t Count = 0;
  for (const auto &Chunk : Chunks)
    Count += Chunk.Modules.size();
  Modules.reserve(Count);
  for (const auto &Chunk : Chunks)
    Modules.insert(Modules.end(), Chunk.Modules.begin(), Chunk.Modules.end());
  const auto Parsed = Clock::now();

  // Fingerprint the exact bytes read, outside the measured stages.
  llvm::json::Array Sources;
  for (size_t I = 0; I < Texts.size(); ++I)
    Sources.push_back(
        llvm::json::Object{{"path", Files[I].string()},
                           {"bytes", int64_t(Texts[I]->getBufferSize())},
                           {"sha256", digest(Texts[I]->getBuffer())}});
  Texts.clear();
  SignatureMatcher::HashIndex Index;
  llvm::json::Array IndexTimes, IndexCPUTimes;
  std::string IndexDigest;
  double FirstIndexTime = 0;
  for (unsigned I = 0; I < IndexRepetitions; ++I) {
    // Destroy the previous index outside the timed region. Each build starts
    // with empty storage, instead of benefiting from retained node capacity.
    Index = SignatureMatcher::HashIndex{};
    llvm::sys::TimePoint<> Ignored;
    std::chrono::nanoseconds UserBefore, SystemBefore, UserAfter, SystemAfter;
    llvm::sys::Process::GetTimeUsage(Ignored, UserBefore, SystemBefore);
    const auto IndexStart = Clock::now();
    Index.build(Modules);
    const double Elapsed = milliseconds(IndexStart, Clock::now());
    llvm::sys::Process::GetTimeUsage(Ignored, UserAfter, SystemAfter);
    IndexTimes.push_back(Elapsed);
    IndexCPUTimes.push_back(
        std::chrono::duration<double, std::milli>(UserAfter + SystemAfter -
                                                  UserBefore - SystemBefore)
            .count());
    const auto Digest = indexDigest(Index);
    if (I == 0) {
      FirstIndexTime = Elapsed;
      IndexDigest = Digest;
    } else if (IndexDigest != Digest) {
      return llvm::createStringError("index changed between repetitions");
    }
  }

  const auto Warmup = scan(*Image, *Entries, Modules, Index, false);
  llvm::json::Array Times;
  llvm::json::Array CPUTimes;
  for (unsigned I = 0; I < Repetitions; ++I) {
    const auto Run = scan(*Image, *Entries, Modules, Index, false);
    if (Run.Hits != Warmup.Hits || Run.Digest != Warmup.Digest)
      return llvm::createStringError("matching changed between repetitions");
    Times.push_back(Run.Milliseconds);
    CPUTimes.push_back(Run.CPUMilliseconds);
  }
  const auto Recorded = scan(*Image, *Entries, Modules, Index, true);
  if (Recorded.Hits != Warmup.Hits || Recorded.Digest != Warmup.Digest)
    return llvm::createStringError("instrumented matching disagrees");
  const auto &Statistics = Recorded.Statistics;
  if (Statistics.Entries == 0)
    return llvm::createStringError("no entries lie in executable input bytes");
  llvm::json::Object Result{{"schema_version", 1},
                            {"image", Input.getValue()},
                            {"threads", Threads.getValue()},
                            {"stride", Stride.getValue()},
                            {"entries", int64_t(Entries->size())},
                            {"scanned_entries", Statistics.Entries},
                            {"modules", int64_t(Modules.size())},
                            {"selected_files", std::move(Sources)},
                            {"read_ms", milliseconds(Start, Read)},
                            {"parse_ms", milliseconds(Read, Parsed)},
                            {"index_ms", FirstIndexTime},
                            {"index_samples_ms", std::move(IndexTimes)},
                            {"index_samples_cpu_ms", std::move(IndexCPUTimes)},
                            {"index_sha256", IndexDigest},
                            {"index_nodes", int64_t(Index.Nodes.size())},
                            {"scan_ms", std::move(Times)},
                            {"scan_cpu_ms", std::move(CPUTimes)},
                            {"hits", Warmup.Hits},
                            {"hit_sha256", Warmup.Digest},
                            {"candidates", Statistics.Candidates},
                            {"crc_requests", Statistics.CRCRequests},
                            {"crc_computations", Statistics.CRCComputations},
                            {"crc_bytes", Statistics.CRCBytes},
                            {"peak_rss_bytes", nullptr}};
  Result["image_sha256"] =
      Image->InputFileSHA256
          ? llvm::json::Value(llvm::toHex(*Image->InputFileSHA256, true))
          : llvm::json::Value(nullptr);
  llvm::SHA256 EntryHash;
  for (uint64_t Address : *Entries) {
    std::array<uint8_t, sizeof(uint64_t)> Encoded;
    llvm::support::endian::write64le(Encoded.data(), Address);
    EntryHash.update(Encoded);
  }
  Result["entries_sha256"] = llvm::toHex(EntryHash.final(), true);
#if defined(__unix__) || defined(__APPLE__)
  struct rusage Usage{};
  if (getrusage(RUSAGE_SELF, &Usage) == 0) {
    uint64_t Bytes = Usage.ru_maxrss;
#if !defined(__APPLE__)
    Bytes *= 1024;
#endif
    Result["peak_rss_bytes"] = Bytes;
  }
#endif
  return Result;
}

} // namespace

int main(int Argc, char **Argv) {
  llvm::cl::ParseCommandLineOptions(Argc, Argv,
                                    "Signature matching benchmark\n");
  if (Repetitions == 0 || IndexRepetitions == 0 || Threads == 0 ||
      (EntriesFile.empty() == (Stride == 0))) {
    llvm::errs() << "Use positive scan/index repetitions and threads, and "
                    "exactly one of --entries or --stride.\n";
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
