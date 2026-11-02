//===- InstructionFlow.h - What each native instruction does ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What the C API publishes about each decoded native instruction: its
/// control transfer, the memory it reaches through constant addresses and
/// how it moves the stack pointer, all from the instruction's own LowIR lift
/// so that a listing agrees with the operations the pipeline consumes.  Also
/// the paged, parallel decode of whole functions that reference queries
/// share.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SDK_CAPI_INSTRUCTIONFLOW_H
#define NEVERD_LIB_SDK_CAPI_INSTRUCTIONFLOW_H

#include "neverd/DataAccessWidths.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <optional>
#include <vector>

namespace neverd::libc {
class NoReturnTargetIndex;
}

namespace neverd::sdk {

struct FuncInfo;
struct Session;

/// Linear native decode from \p Addr, bounded by \p Span bytes when nonzero
/// and by \p Limit instructions.  Mode selection, segment bounds and ARM state
/// checks are those of the published disassembly; \p Visit returns false to
/// stop after an instruction.
template <typename VisitorT>
bool decodeNativeRange(const BinaryImage &Img, Decoder &Dec, va_t Addr,
                       uint64_t Span, uint64_t Limit, VisitorT Visit) {
  va_t Cur = Addr;
  std::optional<InstructionMode> PathMode = Img.instructionModeAt(Addr);
  for (uint64_t I = 0; I < Limit; ++I) {
    if (Cur < Addr)
      break;
    const Segment *Seg = Img.getSegmentFor(Cur);
    if (!Seg || !Seg->isExecutable())
      break;

    uint64_t Consumed = Cur - Addr;
    if (Span > 0 && Consumed >= Span)
      break;

    uint64_t Off64 = Cur - Seg->VA;
    if (Off64 >= Seg->Data.size())
      break;
    size_t Off = static_cast<size_t>(Off64);
    uint64_t Avail64 = std::min<uint64_t>(
        16, std::min<uint64_t>(Seg->Size - Off64, Seg->Data.size() - Off));
    if (Span > 0)
      Avail64 = std::min(Avail64, Span - Consumed);
    if (Avail64 == 0)
      break;
    const uint8_t *Bytes = Seg->Data.data() + Off;

    if (!Dec.selectMode(Img, Cur, PathMode))
      return false; // Unknown or conflicting instruction mode.
    PathMode = Dec.currentMode();
    DecodedInsn DI;
    int Sz = Dec.decodeOne(Bytes, static_cast<size_t>(Avail64), Cur, DI);
    if (Sz <= 0)
      break;
    if (Img.Arch == Arch::ARM &&
        Img.instructionModeAt(Cur + Sz - 1, Dec.currentMode()) !=
            Dec.currentMode())
      break;
    if (!Visit(DI, Bytes, Sz))
      break;
    if (static_cast<uint64_t>(Sz) > InvalidVA - Cur)
      break;
    Cur += Sz;
  }
  return true;
}

/// Where an instruction leaves the stack pointer: its value before the
/// instruction plus Delta, or Base's value before it plus Delta.  Unknown
/// when the lift does not reduce the new value to either.
struct StackMove {
  int64_t Delta = 0;
  std::optional<uint64_t> Base;
  bool Unknown = false;
};

/// A constant address an instruction refers to, how it does ("read",
/// "write" or "offset"), and the bytes a read or write accesses there (0 for
/// an address taken or when the lift names no width).
struct InstructionRef {
  va_t To = InvalidVA;
  llvm::StringRef Kind;
  unsigned Bytes = 0;
};

/// Control transfer and constant memory references of one decoded native
/// instruction.  Kind is empty for a fall-through instruction.
struct InstructionFlow {
  llvm::StringRef Kind;
  va_t Target = InvalidVA;
  llvm::SmallVector<InstructionRef, 2> Refs;
  StackMove Stack;
  /// An unconditional call whose target's image contract ends ordinary flow.
  /// Collected only with a supplied NoReturnTargetIndex. Unknown targets and
  /// conditional calls keep their possible fallthrough.
  bool NoReturn = false;
};

/// The flow kind of an instruction the lifter cannot model.
inline constexpr llvm::StringLiteral UnliftedFlow = "unlifted";

/// The flow of \p DI, with its stack pointer move when \p StackRegisters is
/// given.
InstructionFlow
summarizeInstructionFlow(const BinaryImage &Img, Decoder &Dec,
                         const DecodedInsn &DI,
                         const TargetRegInfo *StackRegisters = nullptr);

/// One decoding worker's temporary lift storage. Only capacity survives a
/// call: every instruction starts with empty operations and fresh FPU state.
/// Results own their facts and never borrow this workspace.
class InstructionFlowScratch {
public:
  /// Flow consumers share an image index. Listing/reference-only consumers
  /// omit it, avoiding target lookups for facts they do not consume.
  explicit InstructionFlowScratch(
      const libc::NoReturnTargetIndex *NoReturnTargets = nullptr)
      : NoReturnTargets(NoReturnTargets) {}
  InstructionFlow summarize(const BinaryImage &Img, Decoder &Dec,
                            const DecodedInsn &DI,
                            const TargetRegInfo *StackRegisters = nullptr);
  /// References needed by the width inventory. Scalar x86 register/immediate
  /// forms cannot access static memory and need no temporary flag arithmetic.
  /// Every other form retains the complete LowIR reference computation.
  llvm::SmallVector<InstructionRef, 2>
  dataAccesses(const BinaryImage &Img, Decoder &Dec, const DecodedInsn &DI);

private:
  const libc::NoReturnTargetIndex *NoReturnTargets;
  std::vector<LowOp> Ops;
};

/// The pointer the loader stored in a relocated slot, as a code address when
/// the slot points to code.
inline std::optional<va_t> relocatedPointer(const BinaryImage &Img, va_t Slot) {
  const size_t Size = Img.getPointerSize();
  const uint8_t *Bytes = Size ? Img.readVA(Slot, Size) : nullptr;
  if (!Bytes)
    return std::nullopt;
  const va_t Target = readPtr(Bytes, Size == 8);
  return Img.Arch == Arch::ARM && Img.CodePtrRelocSlots.count(Slot)
             ? clearThumbBit(Target)
             : Target;
}

/// The functions one page of a reference query covers: from \p FirstEntry
/// in entry order, at most \p MaxFunctions (1 to MaxFunctionsPerPage), and
/// the entry the next page starts at.
struct FunctionPage {
  static constexpr int MaxFunctionsPerPage = kMaxFunctionsPerPage;
  std::vector<const FuncInfo *> Functions;
  std::optional<va_t> NextEntry;
  /// The entries of every function of the list the page was read from, in
  /// ascending order: where a function without a recorded size ends.
  std::vector<va_t> Entries;
};
FunctionPage functionPage(llvm::ArrayRef<FuncInfo> Functions, va_t FirstEntry,
                          int MaxFunctions);
FunctionPage functionPage(const Session &S, va_t FirstEntry, int MaxFunctions);

/// Coverage of the shared native traversal, independent of lift support.
struct FunctionDecodeStatus {
  uint64_t Instructions = 0;
  uint64_t Bytes = 0;
  bool Incomplete = false;
  bool Stopped = false;
};

/// Decodes each function of \p Page in \p Img with a decoder of its own,
/// functions in parallel, and calls Visit(Index, Decoder, Instruction, Scratch)
/// with one exclusive scratch workspace per decoding worker for each of its
/// instructions in address order: up to the function's recorded size, or,
/// when it has none, the instructions its own direct jumps reach from its
/// entry short of another function's entry (Page.Entries; on 32-bit ARM, up
/// to its first terminator), then the parts of it that lie apart from its
/// body (ExceptionInfo::outlyingChunks).  One function's visits run on one
/// thread, so writing only to its own output keeps the result independent of
/// the thread count.  False when a decoder could not be set up.
/// Multi-page readers share one immutable image index through NoReturnTargets;
/// a single-page reader may let this operation own its index.
bool decodeFunctions(
    const BinaryImage &Img, const FunctionPage &Page,
    llvm::function_ref<void(size_t Index, Decoder &, const DecodedInsn &,
                            InstructionFlowScratch &)>
        Visit,
    const std::function<bool()> &Stop = {},
    std::vector<FunctionDecodeStatus> *Status = nullptr,
    const libc::NoReturnTargetIndex *NoReturnTargets = nullptr);

/// The width of the reads and writes the instructions of each of \p
/// Functions state at each address in \p Img: the direct references
/// neverd_code_refs_json publishes, which the listing names data by.  None
/// when a decoder could not be set up.
std::optional<DataAccessWidths>
collectDataAccessWidths(const BinaryImage &Img,
                        llvm::ArrayRef<FuncInfo> Functions);

/// What collectDataAccessWidths read of a list of functions: the extent it
/// read each one with, and how many of them state each width at each
/// address.  Reading another list of the same image then decodes only the
/// functions whose extent differs, as when a lift gives a function its size.
class DataAccessScan {
public:
  /// The widths collectDataAccessWidths(\p Img, \p Functions) gives.  \p Img
  /// is the image every earlier update read.  A function without a size
  /// reads up to other functions' entries, so a list with other entries is
  /// read again whole.  None, and nothing kept, when a decoder could not be
  /// set up.
  std::optional<DataAccessWidths> update(const BinaryImage &Img,
                                         llvm::ArrayRef<FuncInfo> Functions);
  void clear() {
    Extents.clear();
    Entries.clear();
    Counts.clear();
  }

private:
  struct Count {
    va_t Address;
    unsigned Width;
    uint32_t Functions;
  };
  /// The extent each function was read with, by entry.
  std::vector<std::pair<va_t, uint64_t>> Extents;
  /// Every function's entry when they were read, in ascending order.
  std::vector<va_t> Entries;
  /// Each width the functions of Extents state at an address, and how many
  /// of them state it, by address and width.
  std::vector<Count> Counts;
};

} // namespace neverd::sdk

#endif // NEVERD_LIB_SDK_CAPI_INSTRUCTIONFLOW_H
