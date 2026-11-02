//===- NeverDCAPILineage.cpp - Offline evidence handles -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owns loaded collection lifetime, operation status and one completed trace.
/// Matching, identities, conflicts and projection semantics remain in Diff.
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPILineage.h"

#include "neverd/diff/LineageJSON.h"

#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace neverd::diff;
namespace limits = neverd::limits::diff;

struct Handle {
  std::mutex DataMutex, StatusMutex;
  std::atomic<bool> Cancel{false};
  bool Running = false;
  std::string State = "idle", Phase = "idle", Error, Revision;
  uint64_t Done = 0, Total = 0;
  std::shared_ptr<const Lineage> Data;
  std::shared_ptr<const LineageTrace> Trace;
};

void require(bool Condition, llvm::StringRef Message) {
  if (!Condition)
    throw std::invalid_argument(Message.str());
}

using String = std::unique_ptr<char, decltype(&std::free)>;
String copy(llvm::StringRef Text) {
  String Out(static_cast<char *>(std::malloc(Text.size() + 1)), std::free);
  if (!Out)
    throw std::bad_alloc();
  std::memcpy(Out.get(), Text.data(), Text.size());
  Out.get()[Text.size()] = '\0';
  return Out;
}

std::string render(llvm::json::Value Value) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << Value;
  return Text;
}

llvm::StringRef bounded(const char *Text, uint64_t Maximum,
                        const ComparisonControl &Control) {
  require(Text, "lineage JSON is required");
  size_t Size = 0;
  Control.check();
  while (Size <= Maximum && Text[Size]) {
    ++Size;
    if (Size % limits::kSavedComparisonChunkBytes == 0)
      Control.report("lineage:scan", Size);
  }
  require(Size <= Maximum, "lineage text exceeds its byte allowance");
  Control.check();
  return {Text, Size};
}

llvm::json::Object object(const char *Text, const ComparisonControl &Control) {
  auto Value = llvm::json::parse(
      bounded(Text ? Text : "{}", limits::kMaxOptionsBytes, Control));
  if (!Value)
    throw std::invalid_argument(llvm::toString(Value.takeError()));
  const auto *O = Value->getAsObject();
  require(O, "lineage options must be an object");
  Control.check();
  return *O;
}

void deadline(ComparisonControl &Control, uint64_t Milliseconds,
              std::chrono::steady_clock::time_point Start) {
  if (Milliseconds)
    Control.Deadline = Start + std::chrono::milliseconds(Milliseconds);
  Control.check();
}

struct Replacement {
  std::shared_ptr<const Lineage> Data;
  std::string Revision;
};

LineageSettings admissionSettings(Handle &H, const char *Options,
                                  ComparisonControl &Control,
                                  std::chrono::steady_clock::time_point Start) {
  const auto O = object(Options, Control);
  auto Settings = readLineageSettings(O);
  deadline(Control, O.getInteger("timeout_ms").value_or(0), Start);
  if (H.Trace) {
    if (H.Trace->nodes().size() >= Settings.MaxNodes ||
        H.Trace->edges().size() + H.Trace->conflicts().size() >=
            Settings.MaxEdges)
      throw ComparisonInterrupted(ComparisonState::ResourceLimited);
    Settings.MaxNodes -= H.Trace->nodes().size();
    Settings.MaxEdges -= H.Trace->edges().size() + H.Trace->conflicts().size();
  }
  return Settings;
}

int publish(Handle &H, Replacement Next) {
  H.Data.swap(Next.Data);
  H.Revision.swap(Next.Revision);
  H.Trace.reset();
  return 1;
}

/// No user callback or throwing preparation occurs after Commit. The final
/// check and publication share the status lock; later cancellation is merely a
/// request for a now-finished operation and cannot relabel it as stopped.
template <typename Result, typename Prepare, typename Commit>
Result operation(neverd_lineage_t Raw, neverd_lineage_progress_t Progress,
                 void *Context, Prepare Run, Commit Publish) noexcept {
  if (!Raw)
    return {};
  auto &H = *static_cast<Handle *>(Raw);
  bool Admitted = false;
  std::unique_lock Data(H.DataMutex, std::defer_lock);
  try {
    require(Data.try_lock(), "lineage operation is already running");
    {
      std::lock_guard Status(H.StatusMutex);
      H.Cancel = false;
      H.Running = true;
      H.State = "running";
      H.Error.clear();
      H.Phase = "lineage:start";
      H.Done = H.Total = 0;
      Admitted = true;
    }
    ComparisonControl Control;
    Control.Cancel = &H.Cancel;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      const auto Name = Phase.str();
      {
        std::lock_guard Status(H.StatusMutex);
        H.Phase = Name;
        H.Done = Done;
        H.Total = Total;
      }
      if (Progress)
        Progress(Context, Name.c_str(), Done, Total);
    };
    const auto Start = std::chrono::steady_clock::now();
    Control.report("lineage:start");
    std::string Complete = "complete";
    auto Prepared = Run(H, Control, Start);
    Control.report("lineage:commit");
    std::lock_guard Status(H.StatusMutex);
    Control.check();
    Result Out = Publish(H, std::move(Prepared));
    H.State.swap(Complete);
    H.Running = false;
    return Out;
  } catch (const ComparisonInterrupted &Stopped) {
    try {
      std::lock_guard Status(H.StatusMutex);
      if (Admitted) {
        H.State = stateName(Stopped.State).str();
        H.Running = false;
      }
      H.Error = Stopped.what();
    } catch (...) {
    }
  } catch (const std::exception &Error) {
    try {
      std::lock_guard Status(H.StatusMutex);
      if (Admitted) {
        H.State = "failed";
        H.Running = false;
      }
      H.Error = Error.what();
    } catch (...) {
    }
  } catch (...) {
    try {
      std::lock_guard Status(H.StatusMutex);
      if (Admitted) {
        H.State = "failed";
        H.Running = false;
      }
      H.Error = "unknown lineage failure";
    } catch (...) {
    }
  }
  return {};
}

template <typename Read>
const char *inspect(neverd_lineage_t Raw, Read Run) noexcept {
  if (!Raw)
    return nullptr;
  try {
    auto &H = *static_cast<Handle *>(Raw);
    std::lock_guard Lock(H.StatusMutex);
    return copy(Run(H)).release();
  } catch (...) {
    return nullptr;
  }
}
} // namespace

neverd_lineage_t neverd_lineage_create(void) {
  try {
    return new Handle;
  } catch (...) {
    return nullptr;
  }
}
void neverd_lineage_destroy(neverd_lineage_t L) {
  delete static_cast<Handle *>(L);
}
void neverd_lineage_cancel(neverd_lineage_t L) {
  if (L)
    static_cast<Handle *>(L)->Cancel = true;
}
const char *neverd_lineage_last_error(neverd_lineage_t L) {
  return inspect(L, [](const Handle &H) { return H.Error; });
}
const char *neverd_lineage_status_json(neverd_lineage_t L) {
  return inspect(L, [](const Handle &H) {
    return render(
        llvm::json::Object{{"schema", "neverd.bindiff.lineage.status"},
                           {"schema_version", LineageSchemaVersion},
                           {"running", H.Running},
                           {"cancel_requested", H.Cancel.load()},
                           {"operation_state", H.State},
                           {"loaded_revision", H.Revision},
                           {"result_available", !H.Revision.empty()},
                           {"phase", H.Phase},
                           {"done", H.Done},
                           {"total", H.Total},
                           {"error", H.Error}});
  });
}

int neverd_lineage_load_json(neverd_lineage_t L, const char *Document,
                             const char *Options,
                             neverd_lineage_progress_t Progress,
                             void *Context) {
  return operation<int>(
      L, Progress, Context,
      [&](Handle &H, ComparisonControl &Control, auto Start) {
        const auto Settings = admissionSettings(H, Options, Control, Start);
        std::string Error;
        auto Data = readLineage(
            bounded(Document, limits::kMaxSavedComparisonBytes, Control),
            Settings, Error, Control, H.Data.get());
        require(bool(Data), Error);
        return Replacement{Data, Data->revision().str()};
      },
      publish);
}

int neverd_lineage_edit_releases_json(neverd_lineage_t L, const char *Edit,
                                      const char *Options,
                                      neverd_lineage_progress_t Progress,
                                      void *Context) {
  return operation<int>(
      L, Progress, Context,
      [&](Handle &H, ComparisonControl &Control, auto Start) {
        require(bool(H.Data), "no lineage is loaded");
        require(Edit, "lineage release edit is required");
        const auto Settings = admissionSettings(H, Options, Control, Start);
        const auto Request = object(Edit, Control);
        std::string Error;
        auto Data =
            editLineageReleases(*H.Data, Request, Settings, Error, Control);
        require(bool(Data), Error);
        return Replacement{Data, Data->revision().str()};
      },
      publish);
}

const char *neverd_lineage_summary_json(neverd_lineage_t L) {
  return operation<const char *>(
      L, nullptr, nullptr,
      [](Handle &H, ComparisonControl &Control, auto) {
        require(bool(H.Data), "no lineage is loaded");
        return copy(render(lineageSummary(*H.Data, Control)));
      },
      [](Handle &, String Text) { return Text.release(); });
}

const char *neverd_lineage_query_json(neverd_lineage_t L, const char *Query,
                                      neverd_lineage_progress_t Progress,
                                      void *Context) {
  struct Prepared {
    String Text;
    std::shared_ptr<const LineageTrace> Trace;
  };
  return operation<const char *>(
      L, Progress, Context,
      [&](Handle &H, ComparisonControl &Control, auto Start) {
        require(bool(H.Data), "no lineage is loaded");
        require(Query, "lineage query is required");
        const auto Q = readLineageQuery(object(Query, Control));
        deadline(Control, Q.TimeoutMilliseconds, Start);
        auto Page = lineagePage(*H.Data, Q, Control, H.Trace);
        const auto Text = render(std::move(Page.JSON));
        if (Text.size() > limits::kMaxLineagePageBytes)
          throw ComparisonInterrupted(ComparisonState::ResourceLimited);
        return Prepared{copy(Text), std::move(Page.Trace)};
      },
      [](Handle &H, Prepared Next) {
        if (Next.Trace)
          H.Trace = std::move(Next.Trace);
        return Next.Text.release();
      });
}

const char *neverd_lineage_export_json(neverd_lineage_t L,
                                       neverd_lineage_progress_t Progress,
                                       void *Context) {
  return operation<const char *>(
      L, Progress, Context,
      [](Handle &H, ComparisonControl &Control, auto) {
        require(bool(H.Data), "no lineage is loaded");
        return copy(writeLineage(*H.Data, Control));
      },
      [](Handle &, String Text) { return Text.release(); });
}

const char *neverd_lineage_report_json(neverd_lineage_t L, const char *Report,
                                       neverd_lineage_progress_t Progress,
                                       void *Context) {
  return operation<const char *>(
      L, Progress, Context,
      [&](Handle &H, ComparisonControl &Control, auto) {
        require(bool(H.Data), "no lineage is loaded");
        const auto ID = bounded(Report, limits::kMaxOptionsBytes, Control);
        for (const auto &R : H.Data->reports()) {
          Control.check();
          if (R.Evidence->ID == ID)
            return copy(R.Evidence->Text);
        }
        throw std::invalid_argument("unknown lineage source report");
      },
      [](Handle &, String Text) { return Text.release(); });
}
