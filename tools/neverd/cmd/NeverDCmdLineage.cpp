//===- NeverDCmdLineage.cpp - Offline multi-version evidence workflow -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Multi-report input, bounded public queries and atomic source persistence.
/// The C ABI owns identity, validation, graph construction and stop outcomes.
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/diff/Limits.h"
#include "neverd/sdk/NeverDCAPILineage.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace neverd::cli {
namespace cl = llvm::cl;
cl::SubCommand LineageCmd("lineage",
                          "Trace functions across retained binary comparisons");
namespace {
namespace limits = ::neverd::limits::diff;
cl::list<std::string>
    Reports("report",
            cl::desc("Add an exact saved .nddiff report (repeatable)"),
            cl::sub(LineageCmd));
cl::opt<std::string> Reopen("reopen",
                            cl::desc("Open an offline .ndlineage collection"),
                            cl::sub(LineageCmd));
cl::opt<std::string> Save(
    "save",
    cl::desc("Atomically save the collection, or --export-report source bytes"),
    cl::sub(LineageCmd));
cl::opt<std::string>
    ReleaseEdit("release-edit",
                cl::desc("Apply a revision-bound release edit JSON file"),
                cl::sub(LineageCmd));
cl::opt<std::string>
    Options("options", cl::desc("Engine admission settings as a JSON object"),
            cl::init("{}"), cl::sub(LineageCmd));
cl::opt<std::string> Query(
    "query",
    cl::desc("Bounded query JSON; current revision is filled only when absent"),
    cl::sub(LineageCmd));
cl::opt<std::string>
    List("list",
         cl::desc("Page kind: versions, reports, functions, edges, conflicts, "
                  "reviews, trace_nodes, trace_edges, trace_conflicts, "
                  "releases, release_parents, release_children"),
         cl::sub(LineageCmd));
cl::opt<std::string>
    Snapshot("snapshot",
             cl::desc("Exact snapshot ID for functions or trace pages"),
             cl::sub(LineageCmd));
cl::opt<std::string> Address("address",
                             cl::desc("Function entry address to trace"),
                             cl::sub(LineageCmd));
cl::opt<std::string>
    Source("source", cl::desc("Exact report ID for source evidence pages"),
           cl::sub(LineageCmd));
cl::opt<std::string> Release("release",
                             cl::desc("Release ID for parent or child pages"),
                             cl::sub(LineageCmd));
cl::opt<std::string>
    ExportReport("export-report",
                 cl::desc("Export exact original bytes of this report ID"),
                 cl::sub(LineageCmd));
cl::opt<std::string>
    Search("search", cl::desc("Filter function names or entry addresses"),
           cl::sub(LineageCmd));
cl::opt<unsigned> Index("index", cl::desc("Conflict or ambiguity group index"),
                        cl::sub(LineageCmd));
cl::opt<unsigned long long> Offset("offset",
                                   cl::desc("First row of a bounded page"),
                                   cl::init(0), cl::sub(LineageCmd));
cl::opt<unsigned> Limit("limit", cl::desc("Rows per page"),
                        cl::init(limits::kDefaultPageSize),
                        cl::sub(LineageCmd));
cl::opt<bool> History("history",
                      cl::desc("Include superseded manual assessments"),
                      cl::sub(LineageCmd));
cl::opt<bool> Json("json", cl::desc("Output machine-readable JSON"),
                   cl::sub(LineageCmd));

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> Interrupted{false};
void interrupt(int) { Interrupted.store(true, std::memory_order_relaxed); }
struct Failure : std::runtime_error {
  int ExitCode;
  Failure(int Code, const std::string &Message)
      : std::runtime_error(Message), ExitCode(Code) {}
};
void check() {
  if (Interrupted.load(std::memory_order_relaxed))
    throw Failure(130, "lineage operation cancelled");
}
std::string take(const char *Text) {
  const std::unique_ptr<const char, decltype(&neverd_free_string)> Owned(
      Text, neverd_free_string);
  return Text ? Text : "";
}
llvm::json::Value parse(llvm::StringRef Text) {
  auto V = llvm::json::parse(Text);
  if (!V)
    throw std::runtime_error(llvm::toString(V.takeError()));
  return std::move(*V);
}
std::string render(llvm::json::Value V) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << V;
  return Text;
}
std::string read(llvm::StringRef Name, uint64_t &Aggregate) {
  const auto Path = std::filesystem::u8path(Name.begin(), Name.end());
  check();
  std::error_code SizeError;
  const auto Size = std::filesystem::file_size(Path, SizeError);
  if (!SizeError && Size > limits::kMaxSavedComparisonBytes - Aggregate)
    throw Failure(4, "lineage input exceeds the aggregate file byte allowance");
  std::ifstream File(Path, std::ios::binary);
  if (!File)
    throw std::runtime_error("cannot read lineage input: " + Name.str());
  std::string Out;
  std::array<char, limits::kSavedComparisonChunkBytes> Buffer;
  while (File) {
    check();
    File.read(Buffer.data(), Buffer.size());
    const auto Bytes = uint64_t(File.gcount());
    if (Bytes > limits::kMaxSavedComparisonBytes - Aggregate)
      throw Failure(4,
                    "lineage input exceeds the aggregate file byte allowance");
    Aggregate += Bytes;
    Out.append(Buffer.data(), Bytes);
  }
  if (!File.eof())
    throw std::runtime_error("cannot finish reading lineage input: " +
                             Name.str());
  check();
  return Out;
}
void save(llvm::StringRef Name, llvm::StringRef Text) {
  llvm::SmallString<256> Temporary;
  int FD = -1;
  if (auto E =
          llvm::sys::fs::createUniqueFile(Name + ".tmp-%%%%%%", FD, Temporary))
    throw std::runtime_error("cannot create lineage output: " + E.message());
  const auto Cleanup =
      llvm::scope_exit([&] { (void)llvm::sys::fs::remove(Temporary); });
  llvm::raw_fd_ostream OS(FD, true);
  OS << Text;
  OS.close();
  if (OS.has_error()) {
    const auto Error = OS.error();
    // The CLI owns this failure. Clear the stream's error before unwinding so
    // its destructor does not terminate the process and bypass temp cleanup.
    OS.clear_error();
    throw std::runtime_error("cannot write lineage output: " + Error.message());
  }
  check();
  if (auto E = llvm::sys::fs::rename(Temporary, Name))
    throw std::runtime_error("cannot publish lineage output: " + E.message());
}

class Monitor {
  std::atomic<bool> Done{false};
  std::thread Thread;

public:
  explicit Monitor(neverd_lineage_t Handle)
      : Thread([&, Handle] {
          while (!Done.load(std::memory_order_relaxed)) {
            if (Interrupted.load(std::memory_order_relaxed))
              neverd_lineage_cancel(Handle);
            std::this_thread::sleep_for(std::chrono::milliseconds(
                limits::kCancellationPollMilliseconds));
          }
        }) {}
  ~Monitor() {
    Done = true;
    Thread.join();
  }
};

std::string human(const llvm::json::Object &O) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  if (const auto *Items = O.getArray("items")) {
    OS << *O.getString("kind") << " — " << Items->size() << " of "
       << *O.getInteger("total") << " rows\n";
    for (const auto &V : *Items) {
      const auto *Row = V.getAsObject();
      if (Row && Row->getString("address")) {
        OS << *Row->getString("address") << "  "
           << Row->getString("name").value_or("") << "  "
           << Row->getString("version").value_or("").take_front(12);
        if (auto Distance = Row->getInteger("distance"))
          OS << "  distance " << *Distance;
        OS << '\n';
      } else if (Row && Row->getString("label")) {
        OS << *Row->getString("id") << "  " << *Row->getString("label");
        if (const auto Branch = Row->getString("branch");
            Branch && !Branch->empty())
          OS << "  [" << *Branch << ']';
        if (const auto Date = Row->getString("published_at"))
          OS << "  " << *Date;
        if (const auto Version = Row->getString("version"))
          OS << "  snapshot " << Version->take_front(12);
        OS << '\n';
      } else
        OS << V << '\n';
    }
    if (const auto Next = O.getInteger("next_offset"))
      OS << "Next page: --offset " << *Next << '\n';
  } else {
    OS << "Version comparison graph\n"
       << *O.getInteger("versions") << " snapshots, "
       << *O.getInteger("reports") << " source reports\n"
       << *O.getInteger("nodes") << " functions, " << *O.getInteger("edges")
       << " direct associations\n"
       << *O.getInteger("conflicts") << " conflict groups, "
       << *O.getInteger("incomplete_reports") << " incomplete reports\n"
       << "Revision: " << *O.getString("revision") << '\n'
       << "Evidence: retained reports; no new native authentication or "
          "inferred patch verdict.\n";
  }
  return Text;
}
} // namespace

int runLineage() {
  Interrupted = false;
  const auto Previous = std::signal(SIGINT, interrupt);
  const auto Restore = llvm::scope_exit([&] {
    if (Previous != SIG_ERR)
      std::signal(SIGINT, Previous);
  });
  std::unique_ptr<void, decltype(&neverd_lineage_destroy)> Handle(
      neverd_lineage_create(), neverd_lineage_destroy);
  try {
    if (!Handle)
      throw std::runtime_error("cannot allocate lineage handle");
    Monitor Stop(Handle.get());
    for (const auto *Option : {&Query, &List, &Address, &ExportReport, &Reopen,
                               &ReleaseEdit, &Release})
      if (Option->getNumOccurrences() && Option->empty())
        throw std::runtime_error("explicit lineage selectors cannot be empty");
    if (Reports.empty() && Reopen.empty())
      throw std::runtime_error("provide --report or --reopen");
    const bool PageFields =
        Snapshot.getNumOccurrences() || Source.getNumOccurrences() ||
        Release.getNumOccurrences() || Search.getNumOccurrences() ||
        Index.getNumOccurrences() || Offset.getNumOccurrences() ||
        Limit.getNumOccurrences() || History.getNumOccurrences();
    const bool PageQuery = Query.getNumOccurrences() ||
                           List.getNumOccurrences() ||
                           Address.getNumOccurrences();
    if (Query.getNumOccurrences() &&
        (List.getNumOccurrences() || Address.getNumOccurrences() || PageFields))
      throw std::runtime_error(
          "--query cannot be combined with individual page options");
    if (ExportReport.getNumOccurrences() && (PageQuery || PageFields))
      throw std::runtime_error(
          "--export-report cannot be combined with page options");
    if (PageFields && !PageQuery)
      throw std::runtime_error("page options require --list or --address");
    uint64_t InputBytes = 0;
    llvm::json::Value Collection =
        llvm::json::Object{{"schema", "neverd.bindiff.lineage"},
                           {"schema_version", 1},
                           {"reports", llvm::json::Array{}}};
    if (!Reopen.empty())
      Collection = parse(read(Reopen, InputBytes));
    auto *Root = Collection.getAsObject();
    if (!Root || !Root->getArray("reports"))
      throw std::runtime_error("lineage input must contain a reports array");
    for (const auto &Name : Reports)
      Root->getArray("reports")->push_back(read(Name, InputBytes));
    const auto Document = render(std::move(Collection));
    if (!neverd_lineage_load_json(Handle.get(), Document.c_str(),
                                  Options.c_str(), nullptr, nullptr))
      throw std::runtime_error(take(neverd_lineage_last_error(Handle.get())));
    auto Summary = parse(take(neverd_lineage_summary_json(Handle.get())));
    if (!ReleaseEdit.empty()) {
      auto Edit = parse(read(ReleaseEdit, InputBytes));
      auto *Object = Edit.getAsObject();
      if (!Object)
        throw std::runtime_error("--release-edit must contain a JSON object");
      if (!Object->get("revision"))
        (*Object)["revision"] = *Summary.getAsObject()->getString("revision");
      const auto Text = render(std::move(Edit));
      if (!neverd_lineage_edit_releases_json(Handle.get(), Text.c_str(),
                                             Options.c_str(), nullptr, nullptr))
        throw std::runtime_error(take(neverd_lineage_last_error(Handle.get())));
      Summary = parse(take(neverd_lineage_summary_json(Handle.get())));
    }
    const auto &S = *Summary.getAsObject();
    std::string Output, Saved;
    if (!ExportReport.empty()) {
      Output = take(neverd_lineage_report_json(
          Handle.get(), ExportReport.c_str(), nullptr, nullptr));
      if (Output.empty())
        throw std::runtime_error(take(neverd_lineage_last_error(Handle.get())));
      if (!Save.empty())
        Saved = Output;
    } else {
      llvm::json::Value Result = Summary;
      if (!Query.empty() || !List.empty() || !Address.empty()) {
        llvm::json::Value Request =
            Query.empty() ? llvm::json::Value(llvm::json::Object{})
                          : parse(Query);
        auto *Q = Request.getAsObject();
        if (!Q)
          throw std::runtime_error("--query must be an object");
        if (Query.empty()) {
          (*Q)["kind"] = List.empty() ? "trace_nodes" : List.getValue();
          (*Q)["offset"] = uint64_t(Offset);
          (*Q)["limit"] = unsigned(Limit);
          if (!Snapshot.empty())
            (*Q)["version"] = Snapshot.getValue();
          if (!Address.empty())
            (*Q)["address"] = Address.getValue();
          if (!Source.empty())
            (*Q)["report"] = Source.getValue();
          if (!Release.empty())
            (*Q)["release"] = Release.getValue();
          if (Search.getNumOccurrences())
            (*Q)["search"] = Search.getValue();
          if (Index.getNumOccurrences())
            (*Q)["index"] = unsigned(Index);
          if (History)
            (*Q)["latest"] = false;
        }
        if (!Q->get("revision"))
          (*Q)["revision"] = *S.getString("revision");
        const auto Encoded = render(std::move(Request));
        const auto Page = take(neverd_lineage_query_json(
            Handle.get(), Encoded.c_str(), nullptr, nullptr));
        if (Page.empty())
          throw std::runtime_error(
              take(neverd_lineage_last_error(Handle.get())));
        Result = parse(Page);
      } else if (!Snapshot.empty() || !Source.empty() || !Search.empty() ||
                 Index.getNumOccurrences() || History)
        throw std::runtime_error("page options require --list or --address");
      Output = Json ? render(std::move(Result)) + "\n"
                    : human(*Result.getAsObject());
      if (!Save.empty()) {
        Saved =
            take(neverd_lineage_export_json(Handle.get(), nullptr, nullptr));
        if (Saved.empty())
          throw std::runtime_error(
              take(neverd_lineage_last_error(Handle.get())));
      }
    }
    check();
    if (!Save.empty())
      save(Save, Saved);
    // No cancellation check after rename: publication is the final commit.
    llvm::outs() << Output;
    llvm::outs().flush();
    if (llvm::outs().has_error()) {
      llvm::outs().clear_error();
      throw std::runtime_error(
          "cannot write lineage stdout; a saved file remains published");
    }
    return *S.getInteger("incomplete_reports") ? 3
           : *S.getInteger("conflicts")        ? 2
                                               : 0;
  } catch (const std::exception &E) {
    llvm::errs() << "lineage: " << E.what() << '\n';
    if (const auto *Typed = dynamic_cast<const Failure *>(&E))
      return Typed->ExitCode;
    if (Handle) {
      auto Status =
          llvm::json::parse(take(neverd_lineage_status_json(Handle.get())));
      if (!Status) {
        llvm::consumeError(Status.takeError());
        return 1;
      }
      if (const auto *O = Status->getAsObject()) {
        const auto State = O->getString("operation_state").value_or("");
        if (State == "cancelled")
          return 130;
        if (State == "timed_out")
          return 124;
        if (State == "resource_limited")
          return 4;
      }
    }
    return 1;
  }
}
} // namespace neverd::cli
