//===- X86LegacyTransferJournal.cpp - Retained transfer RAM --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep completed return/far accesses and target bindings across host stops.
//===----------------------------------------------------------------------===//

#include "X86LegacyTransferJournal.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

namespace neverd::translate::detail {
namespace {

bool sameCache(const X86LegacySegmentCache &A, const X86LegacySegmentCache &B) {
  return std::tie(A.Base, A.Limit, A.Selector, A.Attributes, A.Unusable) ==
         std::tie(B.Base, B.Limit, B.Selector, B.Attributes, B.Unusable);
}

auto controls(const X86PagingState &C) {
  return std::tie(C.CR0, C.CR3, C.CR4, C.EFER, C.RFlags, C.PKRU, C.PKRS,
                  C.PDPTE);
}

bool sameDebug(const std::optional<X86LegacyDebugState> &A,
               const std::optional<X86LegacyDebugState> &B) {
  return bool(A) == bool(B) &&
         (!A ||
          std::tie(A->DR, A->DR6, A->DR7, A->DebugCTL, A->PendingData) ==
              std::tie(B->DR, B->DR6, B->DR7, B->DebugCTL, B->PendingData));
}

} // namespace

struct X86LegacyTransferContinuation {
  struct Access {
    uint64_t Address;
    uint64_t Value;
    uint8_t Bytes;
    bool Write;
  };
  X86LegacySegmentState Core;
  X86LegacySegmentCache TR;
  X86PagingState Controls;
  X86PagingFeatures Features;
  std::optional<X86LegacyDebugState> Debug;
  uint32_t EIP, Flags, CR2, IDTRBase;
  uint16_t IDTRLimit;
  bool STIBlocked, SSBlocked, NMIBlocked;
  X86LegacyTransferKind Kind;
  llvm::SmallVector<uint8_t> Instruction;
  llvm::SmallVector<Access> Accesses;
  std::optional<X86AlignmentPriority> Alignment;
  std::optional<X86FullSegmentLimitChoice> FullLimit;
  std::optional<X86LegacyDebugProfile> DebugProfile;
  std::optional<X86LegacyVM86Profile> Virtual8086;
  uint8_t DataBreakpoints = 0;
  std::optional<X86Non64ModeBinding> Mode;

  X86LegacyTransferContinuation(const X86LegacyExecutionState &S,
                                const X86PagedMemory &M,
                                const X86LegacyExecutionOptions &O,
                                llvm::ArrayRef<uint8_t> Bytes,
                                X86LegacyTransferKind K)
      : Core(S.Core), TR(S.TR), Controls(M.controls()), Features(M.features()),
        Debug(S.Debug), EIP(S.EIP), Flags(S.EFlags), CR2(S.CR2),
        IDTRBase(S.IDTRBase), IDTRLimit(S.IDTRLimit), STIBlocked(S.STIBlocked),
        SSBlocked(S.SSBlocked), NMIBlocked(S.NMIBlocked), Kind(K),
        Instruction(Bytes.begin(), Bytes.end()),
        Alignment(O.Segments.AlignmentPriority),
        FullLimit(O.Segments.FullLimitChoice), DebugProfile(O.Debug),
        Virtual8086(O.Virtual8086), Mode(O.Non64Mode) {}

  bool matches(const X86LegacyExecutionState &S, const X86PagedMemory &M,
               const X86LegacyExecutionOptions &O) const {
    const auto &C = S.Core;
    return EIP == S.EIP && Flags == S.EFlags && CR2 == S.CR2 &&
           IDTRBase == S.IDTRBase && IDTRLimit == S.IDTRLimit &&
           STIBlocked == S.STIBlocked && SSBlocked == S.SSBlocked &&
           NMIBlocked == S.NMIBlocked && Core.GPR == C.GPR &&
           Core.CPL == C.CPL && Core.GDTRBase == C.GDTRBase &&
           Core.GDTRLimit == C.GDTRLimit && sameCache(Core.LDTR, C.LDTR) &&
           sameCache(TR, S.TR) &&
           std::equal(Core.Segments.begin(), Core.Segments.end(),
                      C.Segments.begin(), sameCache) &&
           controls(Controls) == controls(M.controls()) &&
           Features == M.features() && sameDebug(Debug, S.Debug) &&
           Alignment == O.Segments.AlignmentPriority &&
           FullLimit == O.Segments.FullLimitChoice && DebugProfile == O.Debug &&
           Virtual8086 == O.Virtual8086 && Mode == O.Non64Mode;
  }
};

X86LegacyTransferJournal::X86LegacyTransferJournal(
    X86LegacyExecutionState &S, const X86PagedMemory &M,
    const X86LegacyExecutionOptions &O, llvm::ArrayRef<uint8_t> Instruction,
    X86LegacyTransferKind Kind, bool Active)
    : State(S) {
  if (Active)
    Progress = S.PendingTransfer
                   ? std::make_shared<X86LegacyTransferContinuation>(
                         *S.PendingTransfer)
                   : std::make_shared<X86LegacyTransferContinuation>(
                         S, M, O, Instruction, Kind);
}

llvm::Expected<bool> X86LegacyTransferJournal::replay(uint64_t Address,
                                                      unsigned Bytes,
                                                      bool Write,
                                                      uint64_t &Value) {
  if (!Progress || NextAccess == Progress->Accesses.size())
    return false;
  const auto &Done = Progress->Accesses[NextAccess++];
  if (Done.Address != Address || Done.Bytes != Bytes || Done.Write != Write ||
      (Write && Done.Value != Value))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "transfer journal disagrees with access");
  if (!Write)
    Value = Done.Value;
  return true;
}

void X86LegacyTransferJournal::record(uint64_t Address, unsigned Bytes,
                                      bool Write, uint64_t Value) {
  if (Progress) {
    Progress->Accesses.push_back({Address, Value, uint8_t(Bytes), Write});
    ++NextAccess;
  }
}

void X86LegacyTransferJournal::finish(X86PageWalkStatus Status) {
  if (Progress)
    State.PendingTransfer = Status == X86PageWalkStatus::BackingFault
                                ? std::move(Progress)
                                : nullptr;
}

llvm::Error validateLegacyTransferContinuation(
    const X86LegacyExecutionState &S, const X86PagedMemory &M,
    llvm::ArrayRef<uint8_t> Instruction, const X86LegacyExecutionOptions &O) {
  if (!S.PendingTransfer)
    return llvm::Error::success();
  const auto &P = *S.PendingTransfer;
  if (S.PendingEvent || S.PendingStack || S.PendingMpx || S.PendingTask ||
      S.PendingControl || Instruction != llvm::ArrayRef(P.Instruction) ||
      !P.matches(S, M, O) ||
      (P.Kind == X86LegacyTransferKind::Far
           ? O.FarTransfers != X86LegacyFarTransferProfile::OrderedFrame
           : O.Returns != X86LegacyReturnProfile::OrderedFrame))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "resume the retained transfer and state");
  return llvm::Error::success();
}

uint8_t legacyTransferWatchpoints(const X86LegacyExecutionState &S) {
  return S.PendingTransfer ? S.PendingTransfer->DataBreakpoints : 0;
}

void retainLegacyTransferWatchpoints(X86LegacyExecutionState &S,
                                     uint8_t Matches) {
  if (!S.PendingTransfer)
    return;
  auto P = std::make_shared<X86LegacyTransferContinuation>(*S.PendingTransfer);
  P->DataBreakpoints = Matches;
  S.PendingTransfer = std::move(P);
}

} // namespace neverd::translate::detail
