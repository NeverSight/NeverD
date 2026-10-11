//===- Unpack.cpp - Recover the image a packed executable builds ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "Format.h"
#ifdef NEVERD_UNPACK_EXECUTION
#include "../dynamic/ProcessImports.h"
#include "../dynamic/ProcessTransfer.h"
#include "../dynamic/RuntimeState.h"

#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#endif

#include <fstream>

namespace neverd::unpack {
UnpackOptions::UnpackOptions() {
#define NEVERD_UNPACK_PROCESS_DEFAULT(Field, Member, Default)                  \
  Process.Member = defaults::Default;
#define NEVERD_UNPACK_PROFILE_DEFAULT(Group, Member, JSONGroup, JSONField,     \
                                      Value)                                   \
  if (!Process.Group)                                                          \
    Process.Group.emplace();                                                   \
  Process.Group->Member = Value;
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_PROFILE_DEFAULT
#undef NEVERD_UNPACK_PROCESS_DEFAULT
}

llvm::Expected<std::vector<uint8_t>>
readInput(const std::filesystem::path &Path) {
  std::error_code Error;
  const auto Size = std::filesystem::file_size(Path, Error);
  if (Error)
    return failure(text::ReadFailed + Error.message());
  if (Size > defaults::InputBytes)
    return failure(text::InputTooLarge);
  std::vector<uint8_t> Bytes(Size);
  std::ifstream Stream(Path, std::ios::binary);
  if (!Stream.read(reinterpret_cast<char *>(Bytes.data()),
                   std::streamsize(Size)) ||
      Stream.gcount() != std::streamsize(Size))
    return failure(text::ReadFailed + Path.filename().string());
  return Bytes;
}

#ifdef NEVERD_UNPACK_EXECUTION
void recordDirectServices(UnpackRuntimeState &State,
                          const ObservedExecution &Run) {
  State.DirectServiceCalls =
      llvm::SaturatingAdd(State.DirectServiceCalls, Run.DirectServiceCalls);
}

/// Run the recovered image and collect witnessed export calls and address
/// loads. Failure leaves the image as the entry snapshot already rebuilt it.
llvm::Expected<std::vector<TailImport>> observeTailImports(
    llvm::ArrayRef<uint8_t> Image, const std::filesystem::path &Input,
    const InputImage &ContainerImage, const UnpackOptions &Options,
    UnpackResult::ImportRepairReport &Report, UnpackRuntimeState &State) {
  llvm::SmallString<128> Directory;
  if (std::error_code Error =
          llvm::sys::fs::createUniqueDirectory("neverd-unpack", Directory))
    return failure(text::ReadFailed + Error.message());
  const auto Path =
      std::filesystem::path(Directory.str().str()) / Input.filename();
  auto Finish = [&](llvm::Expected<std::vector<TailImport>> Result) {
    llvm::sys::fs::remove_directories(Directory);
    return Result;
  };
  {
    std::ofstream Stream(Path, std::ios::binary);
    if (!Stream.write(reinterpret_cast<const char *>(Image.data()),
                      std::streamsize(Image.size())))
      return Finish(failure(text::ReadFailed + Path.filename().string()));
  }
  // Discover actual export-call continuations before arming helper watches.
  // Each pass has its own declared process limits. Neither a byte pattern nor
  // an export call alone establishes a replacement's state effects.
  ExportObserver Exports;
  auto Discovery = observeImage(Path, ContainerImage, Options, Exports);
  if (!Discovery)
    return Finish(Discovery.takeError());
  recordDirectServices(State, *Discovery);
  Report.Stop = Discovery->Stop;
  Report.Diagnostic = Discovery->Diagnostic;
  Report.Instructions = Discovery->Instructions;
  Report.Events = Discovery->Events;
  std::vector<uint64_t> Continuations, Gates;
  for (const auto &Call : Exports.calls()) {
    Continuations.push_back(Call.ReturnAddress);
    Gates.push_back(Call.Gate);
  }
  Report.ObservedCalls = Continuations.size();
  if (Continuations.empty())
    return Finish(std::vector<TailImport>{});
  ImportObserver Observer(Continuations, Gates);
  auto Ran = observeImage(Path, ContainerImage, Options, Observer);
  if (!Ran)
    return Finish(Ran.takeError());
  recordDirectServices(State, *Ran);
  Report.Stop = Ran->Stop;
  Report.Diagnostic = Ran->Diagnostic;
  Report.Instructions =
      llvm::SaturatingAdd(Report.Instructions, Ran->Instructions);
  Report.Events = llvm::SaturatingAdd(Report.Events, Ran->Events);
  auto Calls = Observer.takeImports();
  Report.ObservedLoads =
      llvm::count_if(Calls, [](const auto &I) { return I.AddressLoad; });
  return Finish(std::move(Calls));
}
#endif

// The orchestration names no container, instruction set or guest system.
// Each is chosen from facts of the input, without identifying a protector.
llvm::Expected<UnpackResult> unpackFile(const std::filesystem::path &Input,
                                        const UnpackOptions &Options) {
  if (Options.Transfer > defaults::MaxTransfers)
    return failure(text::Limits);
  if (Options.SnapshotOnly && Options.RestoreRuntime)
    return failure("snapshot_only and restore_runtime are mutually exclusive");
  auto File = readInput(Input);
  if (!File)
    return File.takeError();
  const Format *Container = nullptr;
  auto Parsed = readImage(*File, Container);
  if (!Parsed)
    return Parsed.takeError();
  const InputImage &Image = **Parsed;
#ifdef NEVERD_UNPACK_EXECUTION
  UnpackResult Result;
  Result.Format = Container->kind();
  Result.Architecture = architectureName(Image.architecture());
  Result.ImageBase = Image.preferredBase();
  Result.PackedEntryRVA = Image.entryRVA();
  auto Traits = architectureTraits(Image.architecture());
  if (!Traits)
    return Traits.takeError();
  TransferObserver Observer(Image, *Traits, Options.Transfer,
                            Options.RestoreRuntime);
  auto Run = observeImage(Input, Image, Options, Observer);
  if (!Run)
    return Run.takeError();
  Result.Profile = Run->Profile;
  Result.Transfers = Observer.transfers();
  Result.Backend = emulation::executionBackendName(Run->Backend);
  Result.BackendSelectionReason = Run->BackendSelectionReason;
  Result.ProcessStop = Run->Stop;
  Result.ProcessDiagnostic = Run->Diagnostic;
  Result.PC = Run->PC;
  Result.Instructions = Run->Instructions;
  Result.Events = Run->Events;
  auto Observed = Observer.take();
  if (Observed)
    Result.RuntimeState = std::move(Observed->RuntimeState);
  recordDirectServices(Result.RuntimeState, *Run);
  if (!Observed) {
    Result.Diagnostic = text::NoEntry + Result.ProcessStop;
    return Result;
  }
  Result.ImageBase = Observed->Base;
  Result.EntryRVA = Observed->EntryRVA;
  Result.Source = Observed->Source;
  std::string RebuildDependency;
  auto UnsupportedState = [&] {
    Result.Diagnostic.clear();
    if (!Result.RuntimeState.HeapInventoryKnown ||
        Result.RuntimeState.PossibleHeapReferences)
      Result.Diagnostic = Result.RuntimeState.HeapInventoryKnown
                              ? text::ExternalHeapState
                              : text::UnknownHeapState;
    if (Result.RuntimeState.DirectServiceCalls) {
      if (!Result.Diagnostic.empty())
        Result.Diagnostic += "; ";
      Result.Diagnostic += text::DirectServiceState;
    }
    if (!Result.RuntimeState.EncodedPointerInventoryKnown ||
        Result.RuntimeState.PossibleEncodedPointers) {
      if (!Result.Diagnostic.empty())
        Result.Diagnostic += "; ";
      Result.Diagnostic += Result.RuntimeState.EncodedPointerInventoryKnown
                               ? text::EncodedPointerState
                               : text::UnknownEncodedPointerState;
    }
    if (!Result.RuntimeState.DynamicThreadLocalInventoryKnown ||
        Result.RuntimeState.LiveDynamicTLSSlots ||
        Result.RuntimeState.LiveDynamicFLSSlots) {
      if (!Result.Diagnostic.empty())
        Result.Diagnostic += "; ";
      Result.Diagnostic += Result.RuntimeState.DynamicThreadLocalInventoryKnown
                               ? text::DynamicThreadLocalState
                               : text::UnknownDynamicThreadLocalState;
    }
    if (!Result.RuntimeState.AdditionalStateInventoryKnown ||
        Result.RuntimeState.HasAdditionalDependencies) {
      if (!Result.Diagnostic.empty())
        Result.Diagnostic += "; ";
      Result.Diagnostic +=
          Result.RuntimeState.AdditionalStateInventoryKnown
              ? "additional owned OS resources require runtime restoration"
              : "the process supplies no additional runtime-state inventory";
    }
    if (!RebuildDependency.empty()) {
      if (!Result.Diagnostic.empty())
        Result.Diagnostic += "; ";
      Result.Diagnostic += RebuildDependency;
    }
    if (!Result.Diagnostic.empty() && !Options.SnapshotOnly &&
        !Options.RestoreRuntime) {
      Result.Outcome = UnpackOutcome::UnsupportedState;
      return true;
    }
    return false;
  };
  if (UnsupportedState())
    return Result;
  RebuildPlan Plan;
  auto Rebuilt = Container->rebuild(Image, *Observed, Plan);
  if (!Rebuilt)
    return Rebuilt.takeError();
  RebuildDependency = Rebuilt->RuntimeDependency;
  if (UnsupportedState())
    return Result;
  if (Options.RestoreRuntime) {
    // Materialized export identities retain calls in their observed form.
    // Replaying a bare entry in a fresh model would discard the very state
    // this path restores, so it cannot supply import-rewrite evidence.
    auto Restored = restoreRuntime(Image, *Observed, std::move(*Rebuilt));
    if (!Restored) {
      Result.Outcome = UnpackOutcome::UnsupportedState;
      Result.Diagnostic = llvm::toString(Restored.takeError());
      return Result;
    }
    Result.Outcome = UnpackOutcome::Restored;
    Result.Diagnostic = "native runtime state materialized; fixed addresses, "
                        "matching explicit PEB version and captured inputs "
                        "are required; unreached paths are not certified";
    Result.MaterializedTLSCallbacks = Restored->MaterializedTLSCallbacks;
    Result.Sections = std::move(Restored->Sections);
    Result.Imports = std::move(Restored->Imports);
    Result.Image = std::move(Restored->File);
    return Result;
  }
  // The entry snapshot still has protector import calls. Running it shows
  // which export each one reaches, and a second rebuild turns those sites
  // into ordinary import calls when the container recognizes them.
  auto Tails = observeTailImports(Rebuilt->File, Input, Image, Options,
                                  Result.ImportRepair, Result.RuntimeState);
  if (Tails && !Tails->empty()) {
    Plan.TailImports = std::move(*Tails);
    auto Repaired = Container->rebuild(Image, *Observed, Plan);
    if (!Repaired)
      return Repaired.takeError();
    Rebuilt = std::move(Repaired);
    RebuildDependency = Rebuilt->RuntimeDependency;
  } else if (!Tails) {
    Result.ImportRepair.Stop = text::ImportRepairSetupFailed;
    Result.ImportRepair.Diagnostic = llvm::toString(Tails.takeError());
  }
  // A direct binding may first execute after the captured entry. Import
  // discovery must not turn that unresolved dependency into an ordinary image.
  if (UnsupportedState())
    return Result;
  Result.ImportRepair.RepairedCalls = Rebuilt->RepairedTailCalls;
  Result.ImportRepair.RepairedLoads = Rebuilt->RepairedImportLoads;
  Result.ImportRepair.ConflictingCalls = Rebuilt->ConflictingTailCalls;
  Result.MaterializedTLSCallbacks = Rebuilt->MaterializedTLSCallbacks;
  Result.Outcome =
      Options.SnapshotOnly ? UnpackOutcome::Snapshot : UnpackOutcome::Unpacked;
  Result.Sections = std::move(Rebuilt->Sections);
  Result.Imports = std::move(Rebuilt->Imports);
  Result.Image = std::move(Rebuilt->File);
  return Result;
#else
  (void)Image;
  return failure(text::Disabled);
#endif
}
} // namespace neverd::unpack
