//===- X64UnwindFrame.cpp - Windows x64 fixed frames --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/X64UnwindFrame.h"

#include "llvm/Support/Errc.h"

#include <limits>

namespace neverd {

llvm::Expected<X64UnwindFrame> decodeX64UnwindFrame(const ExceptionFunction &EH,
                                                    bool AllowFrameRegister,
                                                    size_t *WorkRemaining) {
  const auto Fail = [](const char *Why) {
    return llvm::createStringError(llvm::errc::not_supported, Why);
  };
  const bool UnwindV1 =
      EH.Encoding == ExceptionEncoding::X64UnwindV1 && EH.UnwindVersion == 1;
  const bool UnwindV2 =
      EH.Encoding == ExceptionEncoding::X64UnwindV2 && EH.UnwindVersion == 2;
  // A frame register establishes the frame from that register instead; its
  // allocations after UWOP_SET_FPREG do not move the establisher.
  const bool HasFrameRegister = EH.FrameRegister != 0;
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      (!UnwindV1 && !UnwindV2) || EH.Kind != RuntimeFunctionKind::Primary ||
      EH.ChainedPrimaryRange || EH.ChainedUnwindInfoRVA ||
      EH.PrimaryFunctionIndex || (EH.UnwindFlags & ~3u) ||
      (HasFrameRegister && !AllowFrameRegister) ||
      (!HasFrameRegister && EH.FrameOffset))
    return Fail("unsupported unwind or frame-register contract");

  X64UnwindFrame Frame;
  struct Allocation {
    bool Push = false;
    uint64_t Bytes = 0;
  };
  // In the codes' order: from the prologue's last instruction back to its
  // first.
  std::vector<Allocation> Allocations;
  std::vector<std::pair<uint64_t, uint64_t>> Saves;
  // X64 CodeOffset is the byte offset of the prologue instruction end.
  uint32_t PreviousOffset = EH.PrologueSize;
  bool SawSetFramePointer = false;
  for (const UnwindOperation &Op : EH.UnwindOperations) {
    if (WorkRemaining) {
      if (!*WorkRemaining)
        return Fail("work budget exhausted");
      --*WorkRemaining;
    }
    if (UnwindV2 && Op.Kind == UnwindOperationKind::Epilog)
      continue;
    if (!Op.CodeOffset || Op.CodeOffset > PreviousOffset)
      return Fail("invalid unwind instruction offset");
    PreviousOffset = Op.CodeOffset;
    uint64_t Bytes = 0;
    bool Push = false;
    switch (Op.Kind) {
    case UnwindOperationKind::PushNonVolatile:
      Bytes = 8;
      Push = true;
      break;
    case UnwindOperationKind::AllocateSmall:
    case UnwindOperationKind::AllocateLarge:
      Bytes = Op.StackOffset;
      if (!Bytes || Bytes % 8)
        return Fail("invalid fixed allocation");
      break;
    case UnwindOperationKind::SaveNonVolatile:
    case UnwindOperationKind::SaveNonVolatileFar:
      Saves.push_back({Op.StackOffset, 8});
      break;
    case UnwindOperationKind::SaveXMM128:
    case UnwindOperationKind::SaveXMM128Far:
      Saves.push_back({Op.StackOffset, 16});
      break;
    case UnwindOperationKind::SetFramePointer:
      if (!HasFrameRegister || SawSetFramePointer)
        return Fail("frame register disagrees with UWOP_SET_FPREG");
      SawSetFramePointer = true;
      Frame.FrameRegister = EH.FrameRegister;
      Frame.SetFramePointerOffset = Op.CodeOffset;
      break;
    default:
      return Fail("unwind action has no certified fixed SP effect");
    }
    if (Bytes && (!Frame.Adjustments.emplace(Op.CodeOffset, Bytes).second ||
                  Bytes > uint64_t(std::numeric_limits<int64_t>::max()) -
                              Frame.EstablisherBytes ||
                  Bytes > uint64_t(std::numeric_limits<int64_t>::max()) -
                              Frame.AllocatedBytes))
      return Fail("conflicting or overflowing allocation");
    // Below the frame register the establisher does not move.
    if (!HasFrameRegister || SawSetFramePointer)
      Frame.EstablisherBytes += Bytes;
    Frame.AllocatedBytes += Bytes;
    if (Bytes)
      Allocations.push_back({Push, Bytes});
  }
  if (HasFrameRegister && !SawSetFramePointer)
    return Fail("frame register disagrees with UWOP_SET_FPREG");

  // A pushed register sits just below what the prologue allocated before it.
  uint64_t Below = 0;
  for (auto It = Allocations.rbegin(); It != Allocations.rend(); ++It) {
    if (It->Push)
      Frame.SavedRegisterSlots.push_back(
          {-static_cast<int64_t>(Below + 8), -static_cast<int64_t>(Below)});
    Below += It->Bytes;
  }
  // RtlVirtualUnwind reads a saved register at its offset from the
  // EstablisherFrame.
  for (const auto &[Offset, Bytes] : Saves) {
    if (Offset > uint64_t(std::numeric_limits<int32_t>::max()))
      return Fail("saved register lies outside any frame");
    const int64_t Begin = static_cast<int64_t>(Offset) -
                          static_cast<int64_t>(Frame.EstablisherBytes);
    Frame.SavedRegisterSlots.push_back(
        {Begin, Begin + static_cast<int64_t>(Bytes)});
  }
  return Frame;
}

} // namespace neverd
