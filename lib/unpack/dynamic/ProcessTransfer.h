//===- ProcessTransfer.h - Transfers into generated code --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_DYNAMIC_PROCESSTRANSFER_H
#define NEVERD_UNPACK_DYNAMIC_PROCESSTRANSFER_H

#include "../core/Capture.h"
#include "Platform.h"

#include <array>
#include <map>
#include <memory>

namespace neverd::unpack {
/// Follows the generations of code inside one image. Generation zero is the
/// image as the guest loader mapped it. Every transfer into code that is newer
/// than the code that was running starts the next generation, measured against
/// the image at that transfer. Control may also fall back to older code, as a
/// callback does when it returns to the stub that called it.
///
/// A transfer is the program's entry when it belongs to the OS model's main
/// entry invocation and the stack pointer has returned to that invocation's
/// initial value: the stub has given back the stack it received.
/// A transfer on a deeper stack is a call the stub makes into the code it
/// produced, such as an initialization callback. Execution continues until
/// an actual transfer on the entry stack, without predicting a final jump.
///
/// The observer knows no container and no guest system. The image supplies
/// its extent, and the instruction set's traits supply the stack pointer and
/// a bound on instruction windows. The stopped CPU supplies decoded extents.
class TransferObserver final : public emulation::ProcessObserver {
public:
  /// \p Wanted selects a transfer by its one-based position; zero accepts the
  /// first one that has the program invocation's entry stack.
  TransferObserver(const InputImage &Image, const ArchitectureTraits &Traits,
                   uint64_t Wanted, bool CaptureRuntime = false)
      : Extent(Image.extent()), Traits(Traits), Wanted(Wanted),
        CaptureRuntime(CaptureRuntime) {}
  llvm::Expected<std::vector<emulation::ExecutionWatch>>
  started(emulation::ProcessView &Process) override;
  llvm::Expected<std::optional<std::vector<emulation::ExecutionWatch>>>
  watched(emulation::ProcessView &Process, uint64_t PC) override;
  llvm::Expected<std::optional<std::vector<emulation::ExecutionWatch>>>
  invoking(emulation::ProcessView &Process) override;
  llvm::Expected<std::optional<std::vector<emulation::ExecutionWatch>>>
  resuming(emulation::ProcessView &Process) override;
  std::vector<emulation::MemoryWriteWatch> writeWatches() const override;
  const std::vector<UnpackTransfer> &transfers() const { return Seen; }
  std::optional<Capture> take() { return std::move(Captured); }

private:
  llvm::Error snapshot(emulation::ProcessView &Process,
                       std::vector<uint8_t> &Bytes,
                       std::vector<uint8_t> *Access = nullptr);
  void refreshWatches();
  void removeInstructionWatch(uint64_t Offset);
  /// The generation of \p Bytes, which were read at image offset \p Offset.
  uint64_t generation(llvm::ArrayRef<uint8_t> Bytes, uint64_t Offset) const;
  const uint64_t Extent;
  const ArchitectureTraits Traits;
  const uint64_t Wanted;
  const bool CaptureRuntime;
  uint64_t Base = 0, InitialSP = 0;
  bool Initialized = false;
  bool EnteredProgram = false;
  bool RefreshNeeded = true;
  /// Images[0] is the loaded image; Images[N] is the image at transfer N.
  std::vector<std::vector<uint8_t>> Images;
  /// The generation of the current instruction, and the visited pages whose
  /// bytes (including possible cross-page fetch tails) are checked on resume.
  uint64_t Running = 0;
  std::vector<bool> Visited;
  /// Last stopped observations of visited pages. A write invalidates their
  /// execution classification even when it arrives through another alias or
  /// through a stopped OS service.
  std::vector<uint8_t> Current;
  // Cached byte-level unions exclude instruction and call-return evidence.
  // Those proofs are still revalidated independently for every refresh.
  struct PageWatchCache {
    uint64_t Running, HistorySize;
    std::array<uint8_t, value::PageSize> Bytes;
    std::vector<emulation::ExecutionWatch> Ranges;
  };
  std::vector<std::unique_ptr<PageWatchCache>> CachedPages;
  // Only an observed start is exempted, never alternative entries inside its
  // bytes. A resume revalidates the complete decoded extent after writes.
  std::map<uint64_t, std::vector<uint8_t>> Instructions;
  struct PendingCall {
    uint64_t Entry;
    emulation::ProcessCallFrame Frame;
  };
  std::vector<PendingCall> Calls;
  std::vector<Capture::CompletedCall> CompletedCalls;
  std::vector<emulation::ExecutionWatch> Watches;
  std::vector<UnpackTransfer> Seen;
  std::optional<Capture> Captured;
};
} // namespace neverd::unpack
#endif
