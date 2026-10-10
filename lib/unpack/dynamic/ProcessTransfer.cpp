//===- ProcessTransfer.cpp - Transfers into generated code ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessTransfer.h"

#include "neverd/emulation/ProcessRuntimeState.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace neverd::unpack {
using namespace emulation;

llvm::Error TransferObserver::snapshot(ProcessView &Process,
                                       std::vector<uint8_t> &Bytes,
                                       std::vector<uint8_t> *Access) {
  const uint64_t Size = Extent;
  Bytes.assign(Size, 0);
  if (Access)
    Access->assign(Size / value::PageSize, 0);
  auto Mappings = Process.mappings();
  if (!Mappings)
    return Mappings.takeError();
  // Unmapped image pages have no bytes; they stay zero and without access.
  for (const auto &M : *Mappings) {
    if (M.Device || M.Address >= Base + Size || M.Address + M.Size <= Base)
      continue;
    const uint64_t Begin = std::max(M.Address, Base) - Base;
    const uint64_t End = std::min(M.Address + M.Size, Base + Size) - Base;
    if (auto E = Process.read(Base + Begin, llvm::MutableArrayRef(Bytes).slice(
                                                Begin, End - Begin)))
      return E;
    if (Access)
      for (uint64_t Page = Begin / value::PageSize;
           Page < llvm::alignTo(End, value::PageSize) / value::PageSize; ++Page)
        (*Access)[Page] |= M.Permissions & GuestAccessPermissions;
  }
  return llvm::Error::success();
}

void TransferObserver::refreshWatches() {
  Watches.clear();
  auto Add = [&](uint64_t Begin, uint64_t End) {
    const uint64_t Address = Base + Begin;
    if (!Watches.empty() &&
        Address - Watches.back().Address <= Watches.back().Size)
      Watches.back().Size =
          std::max(Watches.back().Size, Base + End - Watches.back().Address);
    else
      Watches.push_back({Address, End - Begin});
  };
  for (uint64_t Page = 0; Page < Visited.size(); ++Page) {
    const uint64_t Begin = Page * value::PageSize;
    const uint64_t End = Begin + value::PageSize;
    if (!Visited[Page]) {
      // A visited predecessor can fetch operand bytes from this page. Its
      // write watch includes this prefix; resuming() refreshes those bytes.
      // Expand only a changed prefix, keeping unchanged stub pages executable
      // in direct runs rather than single-stepping their entire contents.
      if (Page && Visited[Page - 1])
        for (uint64_t At = Begin;
             At < std::min(End, Begin + Traits.InstructionWindow - 1); ++At)
          if (generation(llvm::ArrayRef(Current).slice(At, 1), At) != Running)
            Add(At - std::min(At, Traits.InstructionWindow - 1), At + 1);
      Add(Begin, End);
      continue;
    }
    // Visiting another page need not reclassify this one byte by byte. The
    // history is append-only, so its length, the running generation and the
    // complete current page are the inputs that can invalidate this union.
    auto &Cached = CachedPages[Page];
    if (!Cached || Cached->Running != Running ||
        Cached->HistorySize != Images.size() ||
        !std::equal(Current.begin() + Begin, Current.begin() + End,
                    Cached->Bytes.begin())) {
      if (!Cached)
        Cached = std::make_unique<PageWatchCache>();
      Cached->Running = Running;
      Cached->HistorySize = Images.size();
      llvm::copy(llvm::ArrayRef(Current).slice(Begin, value::PageSize),
                 Cached->Bytes.begin());
      Cached->Ranges.clear();
      const bool Unchanged =
          !Running && llvm::all_of(Images, [&](const auto &Image) {
            return std::equal(Current.begin() + Begin, Current.begin() + End,
                              Image.begin() + Begin);
          });
      if (!Unchanged)
        for (uint64_t At = Begin; At < End; ++At) {
          if (generation(llvm::ArrayRef(Current).slice(At, 1), At) == Running)
            continue;
          const uint64_t First =
              At - std::min(At, Traits.InstructionWindow - 1);
          if (!Cached->Ranges.empty() &&
              First - Cached->Ranges.back().Address <=
                  Cached->Ranges.back().Size)
            Cached->Ranges.back().Size = At + 1 - Cached->Ranges.back().Address;
          else
            Cached->Ranges.push_back({First, At + 1 - First});
        }
    }
    for (const auto &Range : Cached->Ranges)
      Add(Range.Address, Range.Address + Range.Size);
  }
  // A generated instruction can retain old opcode bytes. Its observed extent
  // proves its generation even when the byte-level conservative union watches
  // its start. Reuse that proof only while every byte is still identical.
  std::erase_if(Instructions, [&](const auto &I) {
    const auto Bytes = llvm::ArrayRef(Current).slice(I.first, I.second.size());
    return !llvm::equal(Bytes, I.second);
  });
  auto Pending = std::move(Watches);
  Watches.clear();
  auto I = Instructions.begin();
  for (const auto &W : Pending) {
    uint64_t Begin = W.Address;
    const uint64_t End = Begin + W.Size;
    while (I != Instructions.end() && Base + I->first < End) {
      const uint64_t PC = Base + I->first;
      const auto Bytes =
          llvm::ArrayRef(Current).slice(I->first, I->second.size());
      if (PC >= Begin && Visited[I->first / value::PageSize] &&
          generation(Bytes, I->first) == Running) {
        if (PC > Begin)
          Watches.push_back({Begin, PC - Begin});
        Begin = PC + 1;
      }
      ++I;
    }
    if (Begin < End)
      Watches.push_back({Begin, End - Begin});
  }
  for (const auto &Call : Calls) {
    const uint64_t PC = Call.Frame.ReturnAddress;
    auto I = llvm::lower_bound(
        Watches, PC, [](const auto &W, uint64_t PC) { return W.Address < PC; });
    if (I != Watches.end() && I->Address == PC)
      continue;
    if (I != Watches.begin()) {
      const auto &Previous = *std::prev(I);
      if (PC - Previous.Address < Previous.Size)
        continue;
    }
    Watches.insert(I, {PC, 1});
  }
}

void TransferObserver::removeInstructionWatch(uint64_t Offset) {
  const uint64_t PC = Base + Offset;
  if (llvm::any_of(Calls, [&](const auto &Call) {
        return Call.Frame.ReturnAddress == PC;
      }))
    return;
  auto I = llvm::upper_bound(
      Watches, PC, [](uint64_t PC, const auto &W) { return PC < W.Address; });
  if (I == Watches.begin())
    return;
  --I;
  if (PC - I->Address >= I->Size)
    return;
  const uint64_t End = I->Address + I->Size;
  if (PC == I->Address) {
    ++I->Address;
    if (!--I->Size)
      Watches.erase(I);
  } else {
    I->Size = PC - I->Address;
    if (PC + 1 < End)
      Watches.insert(std::next(I), {PC + 1, End - PC - 1});
  }
}

std::vector<MemoryWriteWatch> TransferObserver::writeWatches() const {
  std::vector<MemoryWriteWatch> Result;
  for (uint64_t Page = 0; Page < Visited.size(); ++Page) {
    if (!Visited[Page])
      continue;
    const uint64_t Offset = Page * value::PageSize, Address = Base + Offset;
    const uint64_t Size = std::min(
        value::PageSize + Traits.InstructionWindow - 1, Extent - Offset);
    if (!Result.empty() &&
        Address - Result.back().Address <= Result.back().Size)
      Result.back().Size = Address + Size - Result.back().Address;
    else
      Result.push_back({Address, Size});
  }
  return Result;
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
TransferObserver::resuming(ProcessView &Process) {
  if (!RefreshNeeded && Process.watchedMemoryUnchanged())
    return std::nullopt;
  if (llvm::none_of(Visited, [](bool Seen) { return Seen; })) {
    RefreshNeeded = false;
    return std::nullopt;
  }
  auto Mappings = Process.mappings();
  if (!Mappings)
    return Mappings.takeError();
  bool Changed = false;
  std::array<uint8_t, value::PageSize> Bytes;
  auto Mapped = [&](uint64_t Offset) {
    return llvm::any_of(*Mappings, [&](const auto &M) {
      return !M.Device && Base + Offset >= M.Address &&
             Base + Offset - M.Address < M.Size;
    });
  };
  auto Refresh = [&](uint64_t Offset, uint64_t Size) -> llvm::Error {
    auto Read = llvm::MutableArrayRef(Bytes).take_front(Size);
    if (Mapped(Offset)) {
      if (auto E = Process.read(Base + Offset, Read))
        return E;
    } else
      std::fill(Read.begin(), Read.end(), 0);
    auto Old = llvm::MutableArrayRef(Current).slice(Offset, Size);
    if (!std::equal(Read.begin(), Read.end(), Old.begin())) {
      llvm::copy(Read, Old.begin());
      Changed = true;
    }
    return llvm::Error::success();
  };
  for (uint64_t Page = 0; Page < Visited.size(); ++Page) {
    if (!Visited[Page])
      continue;
    const uint64_t Offset = Page * value::PageSize;
    if (!Mapped(Offset)) {
      Visited[Page] = false;
      Changed = true;
      continue;
    }
    if (auto E = Refresh(Offset, value::PageSize))
      return std::move(E);
    if (Page + 1 < Visited.size() && !Visited[Page + 1])
      if (auto E =
              Refresh(Offset + value::PageSize, Traits.InstructionWindow - 1))
        return std::move(E);
  }
  RefreshNeeded = false;
  if (!Changed)
    return std::nullopt;
  refreshWatches();
  return Watches;
}

uint64_t TransferObserver::generation(llvm::ArrayRef<uint8_t> Bytes,
                                      uint64_t Offset) const {
  // Bytes belong to the generation after the last image they differ from;
  // bytes the loader mapped and nobody changed are generation zero.
  for (uint64_t N = Images.size(); N; --N)
    if (!std::equal(Bytes.begin(), Bytes.end(), Images[N - 1].begin() + Offset))
      return N;
  return 0;
}

llvm::Expected<std::vector<ExecutionWatch>>
TransferObserver::started(ProcessView &Process) {
  const auto Main = Process.inputModule();
  if (!Main)
    return std::vector<ExecutionWatch>();
  if (Main->Size != Extent)
    return failure(text::ImageChanged);
  Base = Main->Base;
  Initialized = true;
  if (auto E = snapshot(Process, Images.emplace_back()))
    return std::move(E);
  auto SP = Process.readRegister(Traits.StackPointer);
  if (!SP)
    return SP.takeError();
  InitialSP = (*SP)[0];
  EnteredProgram = Process.programInvocation();
  Visited.assign(Extent / value::PageSize, false);
  Instructions.clear();
  CachedPages.clear();
  CachedPages.resize(Visited.size());
  RefreshNeeded = true;
  Current = Images.front();
  refreshWatches();
  return Watches;
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
TransferObserver::invoking(ProcessView &Process) {
  if (!Initialized) {
    auto Initial = started(Process);
    if (!Initial)
      return Initial.takeError();
    return std::move(*Initial);
  }
  if (!EnteredProgram && Process.programInvocation()) {
    auto SP = Process.readRegister(Traits.StackPointer);
    if (!SP)
      return SP.takeError();
    InitialSP = (*SP)[0];
    EnteredProgram = true;
  }
  // An invocation starts in generation zero. Previously observed instruction
  // bytes remain reusable only after current write invalidation and generation
  // classification; they cannot hide code generated by another invocation.
  Running = 0;
  refreshWatches();
  return Watches;
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
TransferObserver::watched(ProcessView &Process, uint64_t PC) {
  bool Returning = false;
  if (llvm::any_of(Calls, [&](const auto &Call) {
        return Call.Frame.ReturnAddress == PC;
      })) {
    Returning = true;
    auto SP = Process.readRegister(Traits.StackPointer);
    if (!SP)
      return SP.takeError();
    for (size_t I = Calls.size(); I; --I) {
      const auto &Call = Calls[I - 1];
      if (Call.Frame.ReturnAddress != PC ||
          Call.Frame.ReturnStackPointer != (*SP)[0])
        continue;
      CompletedCalls.push_back({Call.Entry, Call.Frame.Arguments});
      // A matching outer continuation also retires abandoned inner frames;
      // those frames have no completion witness of their own.
      Calls.erase(Calls.begin() + I - 1, Calls.end());
      refreshWatches();
      break;
    }
  }
  if (PC < Base || PC - Base >= Extent) {
    if (Returning)
      return Watches;
    return failure(text::WatchOutside);
  }
  const uint64_t Offset = PC - Base;
  auto Size = Process.instructionSize(PC);
  if (!Size)
    return Size.takeError();
  if (!*Size || *Size > Traits.InstructionWindow || *Size > Extent - Offset)
    return failure(text::InstructionExtent);
  std::vector<uint8_t> Instruction(*Size);
  if (auto E = Process.read(PC, Instruction))
    return std::move(E);
  const uint64_t Generation = generation(Instruction, Offset);
  if (Generation <= Running) {
    // Older code resumed, or more code of the running generation.
    const bool ChangedGeneration = Generation != Running;
    Running = Generation;
    const uint64_t Page = Offset / value::PageSize;
    const bool NewlyVisited = !Visited[Page];
    if (NewlyVisited) {
      // The preceding run did not watch this page or its fetch tail. Even a
      // clean execution-watch stop cannot establish their older snapshots.
      RefreshNeeded = true;
      auto Bytes = llvm::MutableArrayRef(Current).slice(Page * value::PageSize,
                                                        value::PageSize);
      if (auto E = Process.read(Base + Page * value::PageSize, Bytes))
        return std::move(E);
      Visited[Page] = true;
    }
    llvm::copy(Instruction, Current.begin() + Offset);
    Instructions[Offset] = std::move(Instruction);
    if (NewlyVisited || ChangedGeneration)
      refreshWatches();
    else
      removeInstructionWatch(Offset);
    return Watches;
  }
  auto SP = Process.readRegister(Traits.StackPointer);
  if (!SP)
    return SP.takeError();
  if (Seen.size() == defaults::MaxTransfers)
    return failure(text::TransferLimit);
  Seen.push_back(
      {Offset, (*SP)[0] == InitialSP, Generation, Process.programInvocation()});
  Capture Observed{};
  Observed.Base = Base;
  Observed.EntryRVA = Offset;
  Observed.Source = EntrySource::Transfer;
  if (auto E = snapshot(Process, Observed.Memory, &Observed.PageAccess))
    return std::move(E);
  const bool Accepted =
      Wanted ? Seen.size() == Wanted
             : Seen.back().StackBalanced && Seen.back().ProgramInvocation;
  if (!Accepted) {
    auto Frame = Process.callFrame();
    if (!Frame)
      return Frame.takeError();
    if (*Frame) {
      llvm::erase_if(CompletedCalls, [&](const auto &Call) {
        return Call.Entry == PC && Call.Arguments == (**Frame).Arguments;
      });
      Calls.push_back({PC, **Frame});
    }
    Images.push_back(std::move(Observed.Memory));
    Current = Images.back();
    Running = Images.size() - 1;
    Visited[Offset / value::PageSize] = true;
    Instructions[Offset] = std::move(Instruction);
    refreshWatches();
    return Watches;
  }
  Observed.Baseline = std::move(Images.front());
  Observed.Transfers = Seen;
  Observed.CompletedCalls = CompletedCalls;
  // An explicit transfer may stop inside initialization. Its snapshot still
  // needs the effects of earlier initializers and the current thread's TLS.
  Observed.Initializers = Process.completedInitializers();
  auto ThreadLocal = Process.threadLocalMemory();
  if (!ThreadLocal)
    return ThreadLocal.takeError();
  Observed.ThreadLocal = std::move(*ThreadLocal);
  auto OwnedState = Process.runtimeState(CaptureRuntime);
  if (!OwnedState)
    return OwnedState.takeError();
  Observed.OwnedState = std::move(*OwnedState);
  if (Observed.OwnedState) {
    Observed.RuntimeState.AdditionalStateInventoryKnown = true;
    Observed.RuntimeState.HasAdditionalDependencies =
        Observed.OwnedState->hasAdditionalDependencies();
    Observed.RuntimeState.AdditionalDependencyReasons =
        Observed.OwnedState->additionalDependencyReasons();
  }
  if (CaptureRuntime)
    Observed.Modules = Process.modules();
  auto DynamicState = Process.dynamicThreadLocalState();
  if (!DynamicState)
    return DynamicState.takeError();
  if (*DynamicState) {
    Observed.RuntimeState.DynamicThreadLocalInventoryKnown = true;
    Observed.RuntimeState.LiveDynamicTLSSlots = (**DynamicState).ThreadSlots;
    Observed.RuntimeState.LiveDynamicFLSSlots = (**DynamicState).FiberSlots;
  }
  if (auto Allocations = Process.heapAllocations()) {
    Observed.RuntimeState.HeapInventoryKnown = true;
    llvm::sort(*Allocations, [](const auto &A, const auto &B) {
      return A.Address < B.Address;
    });
    uint64_t End = 0;
    for (const auto &A : *Allocations) {
      if (!A.Size || A.Address < End || A.Size > UINT64_MAX - A.Address)
        return failure(text::HeapInventory);
      End = A.Address + A.Size;
    }
    auto Scan = [&](llvm::ArrayRef<uint8_t> Bytes,
                    UnpackHeapReference::Storage Location) {
      if (Allocations->empty())
        return;
      for (uint64_t Offset = 0; Offset + value::PointerBytes <= Bytes.size();
           ++Offset) {
        const uint64_t Address =
            llvm::support::endian::read64le(Bytes.data() + Offset);
        if (Address < Allocations->front().Address || Address >= End)
          continue;
        auto Next = llvm::upper_bound(
            *Allocations, Address,
            [](uint64_t At, const auto &A) { return At < A.Address; });
        if (Next == Allocations->begin())
          continue;
        const auto &A = *std::prev(Next);
        if (Address - A.Address >= A.Size)
          continue;
        auto &State = Observed.RuntimeState;
        ++State.PossibleHeapReferences;
        if (State.HeapReferences.size() < value::HeapReferenceRecords)
          State.HeapReferences.push_back(
              {Offset, Address, A.Address, A.Size, Location});
      }
    };
    Scan(Observed.Memory, UnpackHeapReference::Storage::Image);
    if (Observed.ThreadLocal)
      Scan(*Observed.ThreadLocal, UnpackHeapReference::Storage::ThreadLocal);
  }
  if (auto Encoded = Process.encodedPointers()) {
    auto &State = Observed.RuntimeState;
    State.EncodedPointerInventoryKnown = true;
    llvm::sort(*Encoded);
    Encoded->erase(std::unique(Encoded->begin(), Encoded->end()),
                   Encoded->end());
    auto Scan = [&](llvm::ArrayRef<uint8_t> Bytes,
                    UnpackHeapReference::Storage Location) {
      if (Encoded->empty())
        return;
      for (uint64_t Offset = 0; Offset + value::PointerBytes <= Bytes.size();
           ++Offset) {
        const uint64_t Value =
            llvm::support::endian::read64le(Bytes.data() + Offset);
        if (!std::binary_search(Encoded->begin(), Encoded->end(), Value))
          continue;
        ++State.PossibleEncodedPointers;
        if (State.EncodedPointerReferences.size() <
            value::EncodedPointerRecords)
          State.EncodedPointerReferences.push_back({Offset, Value, Location});
      }
    };
    Scan(Observed.Memory, UnpackHeapReference::Storage::Image);
    if (Observed.ThreadLocal)
      Scan(*Observed.ThreadLocal, UnpackHeapReference::Storage::ThreadLocal);
  }
  // Several identities may share one address. Keep a named one, in a stable
  // order, so the rebuilt directory does not depend on enumeration order.
  for (auto &Export : Process.exports()) {
    ExportBinding Candidate{std::move(Export.Module), std::move(Export.Name),
                            Export.Ordinal};
    auto [Slot, Inserted] =
        Observed.Exports.try_emplace(Export.Address, Candidate);
    if (!Inserted && Candidate < Slot->second)
      Slot->second = std::move(Candidate);
  }
  Captured = std::move(Observed);
  return std::nullopt;
}
} // namespace neverd::unpack
