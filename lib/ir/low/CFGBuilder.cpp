//===- CFGBuilder.cpp - Control-flow graph construction ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements recursive-descent disassembly for single-function CFG
/// construction: the build entry point, the exploration worklist, and the
/// multi-stage retry of indirect branches.  Basic-block formation and ordinary
/// successor linking live in CFGBuilderBlocks.cpp, instruction classification
/// and call/tail-call rewriting in CFGBuilderInsn.cpp, exceptional successor
/// linking in CFGBuilderException.cpp, function entry-point detection in
/// FuncDetector.cpp, and jump-table resolution in jumptable/.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"

#include "PrologueStackAllocation.h"
#include "X86/RegistrationSEHControl.h"

#include "neverd/Limits.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/AArch64ImmediateMemoryImage.h"
#include "neverd/ir/low/ImportedMemoryCopy.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <compare>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <queue>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

bool CFGBuilder::isKnownFunctionEntry(va_t Addr) const {
  if (CurrentImg && !CurrentImg->isFunctionEntryCandidateAt(Addr))
    return false;
  if (KnownFuncEntries && KnownFuncEntries->count(Addr) != 0)
    return true;
  return CurrentImg && CurrentImg->hasKnownFunctionEntryAt(Addr);
}

bool CFGBuilder::isCurrentOwnedFragment(va_t Addr) const {
  return CurrentImg &&
         isExplicitlyOwnedFunctionFragment(*CurrentImg, CurrentFuncEntry, Addr,
                                           ExecutableCodeOwners);
}

bool CFGBuilder::isCurrentOwnedCode(va_t Address) const {
  if (AuthoritativeCurrentFuncRange &&
      Address >= AuthoritativeCurrentFuncRange->first) {
    if (Address < AuthoritativeCurrentFuncRange->second)
      return true;
    // Interior-query bounds may be shortened by a coarse frame span. That
    // cannot erase independently declared symbol/code-range size evidence.
    const va_t MetadataEnd = CurrentImg->getFunctionMetadataEnd(
        CurrentFuncEntry, ExecutableCodeOwners);
    if (MetadataEnd != InvalidVA && Address < MetadataEnd)
      return true;
  }
  return isCurrentOwnedFragment(Address);
}

bool CFGBuilder::instructionFitsCurrentOwnership(va_t Address,
                                                 uint64_t Size) const {
  if (!Size || Size > InvalidVA - Address)
    return false;
  if (AuthoritativeCurrentFuncRange &&
      Address >= AuthoritativeCurrentFuncRange->first &&
      Address < AuthoritativeCurrentFuncRange->second &&
      Size <= AuthoritativeCurrentFuncRange->second - Address)
    return true;
  if (!isCurrentOwnedCode(Address))
    return true;
  // Only boundary-crossing instructions and owned fragments need these
  // queries. Use the same end policy as calls/traps: a coarse unwind range
  // can support interior ownership without declaring one function's end.
  // Adjacent owned fragments continue each other; ownership gaps do not.
  for (uint64_t Offset = 1; Offset != Size; ++Offset)
    if (codeRangeEndsAt(Address, Address + Offset))
      return false;
  return true;
}

bool CFGBuilder::isFramelessLeafColdPart(const BinaryImage &Img,
                                         va_t Addr) const {
  if (!CurrentFuncIsFramelessLeaf || Addr == InvalidVA || Addr < Img.Base ||
      Img.ExceptionMetadata.findFunction(Addr))
    return false;
  // A restricted load decodes only the records it needs; the raw directory
  // still lists every one.
  const uint64_t RVA = Addr - Img.Base;
  const auto Record = std::upper_bound(
      Img.COFFPDataRecords.begin(), Img.COFFPDataRecords.end(), RVA,
      [](uint64_t Value, const BinaryImage::COFFPDataRecord &Rec) {
        return Value < Rec.BeginRVA;
      });
  return Record == Img.COFFPDataRecords.begin() ||
         RVA >= std::prev(Record)->EndRVA;
}

va_t CFGBuilder::nextKnownFunctionEntry(va_t After) const {
  va_t Best = InvalidVA;
  auto Consider = [&](va_t Addr) {
    if (Addr == InvalidVA || Addr <= After || isCurrentExceptionalEntry(Addr) ||
        isCurrentOwnedFragment(Addr))
      return;
    if (Best == InvalidVA || Addr < Best)
      Best = Addr;
  };
  if (KnownFuncEntries) {
    auto It = KnownFuncEntries->upper_bound(After);
    while (It != KnownFuncEntries->end()) {
      if (!isCurrentExceptionalEntry(*It) && !isCurrentOwnedFragment(*It)) {
        Consider(*It);
        break;
      }
      ++It;
    }
  }
  if (CurrentImg) {
    va_t Probe = After;
    for (unsigned I = 0;
         I < ::neverd::limits::ir::
                 kCFGBuilderCFGBuilderNextKnownFunctionEntryILimit;
         ++I) {
      const va_t Next = CurrentImg->nextKnownFunctionEntryAfter(Probe);
      if (Next == InvalidVA)
        break;
      if (!isCurrentExceptionalEntry(Next) && !isCurrentOwnedFragment(Next)) {
        Consider(Next);
        break;
      }
      Probe = Next;
    }
  }
  return Best;
}

size_t CFGBuilder::knownFunctionEntryCount() const {
  size_t N = KnownFuncEntries ? KnownFuncEntries->size() : 0;
  if (!CurrentImg)
    return N;
  N = std::max(N, CurrentImg->RuntimeFunctionAddrs.size());
  N = std::max(N, CurrentImg->KnownCodeRanges.size());
  N = std::max(N, CurrentImg->ExceptionMetadata.FunctionIndex.size());
  N = std::max(N, CurrentImg->COFFPDataRecords.size());
  return N;
}

void detail::retireReplayedI386GOTPCAmbiguities(
    std::set<I386GOTOFFAmbiguityReplayKey> &Pending,
    const std::set<I386GOTOFFAmbiguityReplayKey> &Replayed,
    const std::set<va_t> *SafelyPublishedBranches) {
  for (auto It = Pending.begin(); It != Pending.end();) {
    const bool SafelyPublished =
        SafelyPublishedBranches &&
        SafelyPublishedBranches->count(std::get<0>(*It));
    if (Replayed.count(*It) || SafelyPublished)
      It = Pending.erase(It);
    else
      ++It;
  }
}

namespace {

// A shared ARM64 epilogue still executes in the branching caller's frame.
// Admit only this complete machine shape, retaining every load and SP update
// in LowIR. This is edge evidence, not a callee ABI or exclusive range owner.
bool isBackwardSharedEpilogue(const BinaryImage &Image, va_t FunctionEntry,
                              va_t Source, va_t Target,
                              const std::set<va_t> *FunctionEntries) {
  if (Image.Arch != Arch::AArch64 || Image.Format != BinaryFormat::MachO ||
      Source % 4 || Target % 4 || Target >= FunctionEntry ||
      Image.findImportAt(Target))
    return false;
  // Negative-only shape filter: ordinary tail calls need not pay the full
  // immutable mapping/fixup proof. A match still proves every byte below.
  const auto *FirstBytes = Image.readVA(Target, 4);
  if (!FirstBytes)
    return false;
  const uint32_t FirstWord = readLE<uint32_t>(FirstBytes);
  if ((FirstWord & 0xffc003e0) != 0xa94003e0u &&
      (FirstWord & 0xffc003e0) != 0xa8c003e0u)
    return false;
  const auto HasInteriorEntry = [&](va_t End) {
    const auto Contains = [&](const auto &Entries) {
      const auto Next = Entries.upper_bound(Target);
      return Next != Entries.end() && *Next < End;
    };
    if ((FunctionEntries && Contains(*FunctionEntries)) ||
        Contains(Image.RuntimeFunctionAddrs) ||
        Contains(Image.VerifiedFunctionEntries))
      return true;
    return std::any_of(
        Image.Symbols.begin(), Image.Symbols.end(), [&](const Symbol &Symbol) {
          return Symbol.IsFunc && Symbol.Addr > Target && Symbol.Addr < End;
        });
  };
  const auto BranchTarget = [](uint32_t Word,
                               va_t Address) -> std::optional<va_t> {
    if (!branch::A64Branch.matches(Word))
      return std::nullopt;
    return branch::a64BranchTarget(Word, Address);
  };
  const auto SourceBytes = readImmutableCodeBytes(Image, Source, 4);
  if (!SourceBytes ||
      BranchTarget(readLE<uint32_t>(SourceBytes->data()), Source) != Target)
    return false;

  uint32_t RestoredRegisters = 0;
  bool ReleasedStack = false;
  for (unsigned I = 0; I < 5; ++I) {
    const uint32_t Size = (I + 1) * 4;
    if (FunctionEntry - Target < Size)
      return false;
    const auto Bytes = readImmutableCodeBytes(Image, Target, Size);
    if (!Bytes)
      return false;
    const va_t Address = Target + I * 4;
    const uint32_t Word = readLE<uint32_t>(Bytes->data() + I * 4);
    // The caller may have restored X30 before branching to this shared tail.
    // Decoding preserves the physical RET; the complete caller-frame proof
    // must still establish saved register identities and the restored SP.
    if (Word == 0xd65f03c0u)
      return RestoredRegisters && ReleasedStack &&
             !HasInteriorEntry(Target + Size);
    if (const auto Exit = BranchTarget(Word, Address)) {
      // The existing tail-call owner handles this final external transfer.
      // Never follow another local function, veneer-like guess, or data slot.
      return RestoredRegisters && ReleasedStack &&
             !HasInteriorEntry(Target + Size) && Image.findImportAt(*Exit) &&
             Image.hasExecutableCodeOwnerRange(*Exit, 4) &&
             Image.readVA(*Exit, 4);
    }
    const bool Pair = (Word & 0xffc003e0) == 0xa94003e0u;
    const bool PostPair = (Word & 0xffc003e0) == 0xa8c003e0u;
    if (Pair || PostPair) {
      const unsigned First = Word & 31;
      const unsigned Second = (Word >> 10) & 31;
      const unsigned Offset = (Word >> 15) & 127;
      if (ReleasedStack || First < 19 || First > 30 || Second < 19 ||
          Second > 30 || First == Second || Offset >= 64 ||
          (RestoredRegisters & ((1u << First) | (1u << Second))) ||
          (PostPair && (!Offset || Offset % 2)))
        return false;
      RestoredRegisters |= (1u << First) | (1u << Second);
      ReleasedStack = PostPair;
      continue;
    }
    // ADD sp, sp, #imm (unshifted), once, after all full-width restores.
    if ((Word & 0xffc003ff) == 0x910003ffu) {
      const unsigned Amount = (Word >> 10) & 4095;
      if (!RestoredRegisters || ReleasedStack || !Amount || Amount % 16)
        return false;
      ReleasedStack = true;
      continue;
    }
    return false;
  }
  return false;
}

InstructionMode effectiveInstructionMode(Arch Architecture,
                                         InstructionMode Mode) {
  if (Architecture == Arch::ARM && Mode == InstructionMode::Default)
    return InstructionMode::ARM;
  return Mode;
}

// Stack-pointer offsets for calls into a function's own body (see
// CFGBuilder::rewriteOwnInteriorCall).  A state maps the registers and
// temporaries known to lie within a range of offsets from a base to that base
// and range.  Base zero is the analysis' reference stack pointer; a stack
// pointer loaded from anywhere else starts a base of its own, named after the
// operation that loaded it, which only ever compares with itself.
struct StackOffsetRange {
  uint64_t Base = 0;
  int64_t Lo = 0;
  int64_t Hi = 0;
  bool operator==(const StackOffsetRange &O) const {
    return Base == O.Base && Lo == O.Lo && Hi == O.Hi;
  }
  bool operator!=(const StackOffsetRange &O) const { return !(*this == O); }
};
struct StackOffsetKey {
  VnodeSpace Space;
  uint64_t Offset;
  uint16_t Size;
  auto operator<=>(const StackOffsetKey &) const = default;
};
using StackOffsetState = std::map<StackOffsetKey, StackOffsetRange>;
/// What the analysis needs of the target: its stack pointer, the width of a
/// pushed return address and of a full register, and the registers a call
/// keeps under the image's calling convention.
struct StackFrameModel {
  const TargetRegInfo *TRI = nullptr;
  BinaryFormat Format = BinaryFormat::Unknown;
  StackOffsetKey StackPointer{VnodeSpace::REG, 0, 0};
  uint16_t Width = 0;
  /// Every call leaves the stack pointer as it found it: the callee pops its
  /// own return address and nothing else, as under both x86-64 conventions.
  /// An i386 callee may pop its arguments too (`ret imm`).
  bool CallsKeepStackPointer = false;

  StackFrameModel(const TargetRegInfo &Info, BinaryFormat F)
      : TRI(&Info), Format(F),
        StackPointer{VnodeSpace::REG, Info.StackPointer, Info.PointerSize},
        Width(Info.PointerSize) {}
};
constexpr int64_t kNoLowerBound = std::numeric_limits<int64_t>::min();
constexpr int64_t kNoUpperBound = std::numeric_limits<int64_t>::max();

int64_t shiftBound(int64_t Bound, int64_t Delta) {
  if (Bound == kNoLowerBound || Bound == kNoUpperBound)
    return Bound;
  return Bound + Delta;
}

StackOffsetRange shiftRange(StackOffsetRange R, int64_t Delta) {
  return {R.Base, shiftBound(R.Lo, Delta), shiftBound(R.Hi, Delta)};
}

/// Follows offsets through constant adjustments and full-width copies; any
/// other write forgets what it overwrites.  A call keeps the stack pointer
/// (the callee pops its own return address) and the registers the calling
/// convention preserves.
void transferStackOffsets(const StackFrameModel &M, const LowOp &Op,
                          StackOffsetState &S) {
  auto Forget = [&S](const NdVar &Out) {
    for (auto It = S.begin(); It != S.end();)
      It = It->first.Space == Out.Space &&
                   Out.Offset < It->first.Offset + It->first.Size &&
                   It->first.Offset < Out.Offset + Out.Size
               ? S.erase(It)
               : std::next(It);
  };
  if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
    for (auto It = S.begin(); It != S.end();)
      It = It->first.Space == VnodeSpace::REG &&
                   (M.TRI->isCallPreserved(It->first.Offset, It->first.Size,
                                           M.Format) ||
                    (M.CallsKeepStackPointer && It->first == M.StackPointer))
               ? std::next(It)
               : S.erase(It);
    if (Op.Output.Size)
      Forget(Op.Output);
    return;
  }
  if (!Op.Output.Size)
    return;
  auto Known = [&S, &M](const NdVar &V) -> std::optional<StackOffsetRange> {
    if (V.Size != M.Width && !(M.Width == 4 && V.Size == 8 && V.isTemp()))
      return std::nullopt;
    auto It = S.find({V.Space, V.Offset, V.Size});
    if (It == S.end())
      return std::nullopt;
    return It->second;
  };
  std::optional<StackOffsetRange> Value;
  if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
      Op.Output.Size == Op.Inputs[0].Size &&
      (Op.Output.Size == M.Width ||
       (M.Width == 4 && Op.Output.Size == 8 && Op.Output.isTemp()))) {
    Value = Known(Op.Inputs[0]);
  } else if (M.Width == 4 && Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs == 1 &&
             Op.Inputs[0].Size == 4 && Op.Output.isTemp() &&
             Op.Output.Size == 8) {
    // The i386 lifter widens a computed 32-bit address for LowIR memory
    // operations. Retain its coordinate without granting a 64-bit register
    // or arithmetic result the same identity.
    Value = Known(Op.Inputs[0]);
  } else if (M.Width == 4 && Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
             Op.Output.Size == 4 && Op.Inputs[0].isTemp() &&
             Op.Inputs[0].Size == 8 && Op.Inputs[1].isConst() &&
             Op.Inputs[1].Size == 4 && Op.Inputs[1].Offset == 0) {
    // LEA writes the low word of that same widened i386 address. Only
    // carriers originating in the zero extension above can supply a fact.
    Value = Known(Op.Inputs[0]);
  } else if (Op.Output.Size == M.Width &&
             (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
             Op.NumInputs == 2) {
    for (unsigned I = 0; I < 2 && !Value; ++I) {
      // `constant - offset` is not an offset.
      if (Op.Opcode == NdOp::INT_SUB && I != 0)
        break;
      const NdVar &C = Op.Inputs[1 - I];
      if (!C.isConst() || C.Size != M.Width)
        continue;
      const auto Base = Known(Op.Inputs[I]);
      const int64_t Delta = llvm::SignExtend64(C.Offset, M.Width * 8);
      if (Base && Delta > -limits::kMaxFrameSize &&
          Delta < limits::kMaxFrameSize)
        Value = shiftRange(*Base, Op.Opcode == NdOp::INT_ADD ? Delta : -Delta);
    }
  }
  // Any other full-width stack pointer is a base of its own.
  if (!Value && Op.Output.Space == M.StackPointer.Space &&
      Op.Output.Offset == M.StackPointer.Offset && Op.Output.Size == M.Width)
    Value = StackOffsetRange{(Op.Addr << 6 | (Op.Seq & 63)) + 1, 0, 0};
  Forget(Op.Output);
  if (Value)
    S[{Op.Output.Space, Op.Output.Offset, Op.Output.Size}] = *Value;
}

/// A matching SP at RET is insufficient if any reaching effect may replace
/// the physical PC. Only writes disjoint from this invocation's entire slot
/// preserve it. ABI register preservation does not bound a callee's stores.
bool mayWriteOwnReturnSlot(const StackFrameModel &M, const LowOp &Op,
                           const StackOffsetState &State) {
  if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
      Op.Opcode == NdOp::INTRINSIC)
    return true;
  if (Op.Opcode != NdOp::STORE && Op.Opcode != NdOp::ATOMIC_XCHG &&
      Op.Opcode != NdOp::ATOMIC_ADD && Op.Opcode != NdOp::ATOMIC_CMPXCHG)
    return false;
  const auto Memory = lowMemoryOperands(Op);
  if (!Memory.Complete || !Memory.Address ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      (Memory.Address->Size != M.Width &&
       !(M.Width == 4 && Memory.Address->Size == 8 &&
         Memory.Address->isTemp())))
    return true;
  const auto Address = State.find(
      {Memory.Address->Space, Memory.Address->Offset, Memory.Address->Size});
  if (Address == State.end() || Address->second.Base != 0)
    return true;
  const auto &Range = Address->second;
  // Use subtraction on the bounded access width, not addition to an
  // unbounded/near-limit address. Half-width stores can still destroy a PC.
  return Range.Lo < M.Width && Range.Hi > -int64_t(Memory.AccessSize);
}

/// Forward fixpoint from the given block-entry states.  A join keeps the
/// values every incoming path knows, as the hull of their ranges.  A bound
/// still moving after limits::kStackOffsetWideningJoins joins at one block
/// becomes unbounded; otherwise states only lose entries or grow, so it ends.
void solveStackOffsets(const StackFrameModel &M, const LowFunc &Func,
                       std::vector<std::optional<StackOffsetState>> &In,
                       const std::set<va_t> &Returns = {}) {
  const size_t N = Func.Blocks.size();
  std::vector<unsigned> Joins(N, 0);
  std::vector<int> Work;
  for (size_t B = 0; B < N; ++B)
    if (In[B])
      Work.push_back(static_cast<int>(B));
  while (!Work.empty()) {
    const int B = Work.back();
    Work.pop_back();
    StackOffsetState Out = *In[B];
    for (const LowOp &Op : Func.Blocks[B].Ops)
      transferStackOffsets(M, Op, Out);
    if (!Func.Blocks[B].Ops.empty() &&
        Returns.count(Func.Blocks[B].Ops.back().Addr))
      continue;
    for (int S : Func.Blocks[B].Succs) {
      if (S < 0 || static_cast<size_t>(S) >= N)
        continue;
      if (!In[S]) {
        In[S] = Out;
        Work.push_back(S);
        continue;
      }
      const bool Widen = ++Joins[S] > limits::kStackOffsetWideningJoins;
      StackOffsetState Met;
      for (const auto &[K, Old] : *In[S]) {
        auto It = Out.find(K);
        if (It == Out.end() || It->second.Base != Old.Base)
          continue;
        StackOffsetRange R{Old.Base, std::min(Old.Lo, It->second.Lo),
                           std::max(Old.Hi, It->second.Hi)};
        if (Widen && R.Lo != Old.Lo)
          R.Lo = kNoLowerBound;
        if (Widen && R.Hi != Old.Hi)
          R.Hi = kNoUpperBound;
        Met.emplace(K, R);
      }
      if (Met != *In[S]) {
        In[S] = std::move(Met);
        Work.push_back(S);
      }
    }
  }
}

/// The register-target jumps of \p Func that run once their own block has
/// released a frame: every path into the block reaches it with the stack
/// pointer below the one the function was entered with, and the block raises
/// it back to that one before the jump.
std::set<va_t> frameReleasingIndirectJumps(const BinaryImage &Img,
                                           const LowFunc &Func) {
  std::set<va_t> Jumps;
  const size_t N = Func.Blocks.size();
  if (N == 0)
    return Jumps;
  StackFrameModel M(getTargetRegInfo(Img.Arch), Img.abiFormat());
  M.CallsKeepStackPointer = Img.Arch == Arch::X64;
  std::vector<std::optional<StackOffsetState>> FromEntry(N);
  FromEntry[0] = StackOffsetState{{M.StackPointer, {0, 0, 0}}};
  for (size_t B = 1; B < N; ++B)
    if (Func.Blocks[B].Preds.empty() ||
        !Func.Blocks[B].ExceptionalPreds.empty())
      FromEntry[B] = StackOffsetState{};
  solveStackOffsets(M, Func, FromEntry);
  for (size_t B = 0; B < N; ++B) {
    if (!FromEntry[B])
      continue;
    StackOffsetState State = *FromEntry[B];
    const auto Entering = State.find(M.StackPointer);
    if (Entering == State.end() || Entering->second.Base != 0 ||
        Entering->second.Hi >= 0)
      continue;
    for (const LowOp &Op : Func.Blocks[B].Ops) {
      if (Op.Opcode == NdOp::INDIR_BR) {
        const auto Before = State.find(M.StackPointer);
        if (Before != State.end() &&
            Before->second == StackOffsetRange{0, 0, 0})
          Jumps.insert(Op.Addr);
      }
      transferStackOffsets(M, Op, State);
    }
  }
  return Jumps;
}

} // namespace

//===----------------------------------------------------------------------===//
// CFGBuilder
//===----------------------------------------------------------------------===//

bool CFGBuilder::codeFollowsTrap(const Decoder &Dec, va_t Next) {
  const BinaryImage &Img = *CurrentImg;
  // Keep this decoder separate: a peek must not consume Thumb IT state.
  Decoder PeekDecoder;
  // A branch may already have reached a cold block beyond the primary range.
  // Bound a peek that starts inside that range without clipping such a block.
  va_t RangeEnd = InvalidVA;
  if (CurrentFuncRange && Next >= CurrentFuncRange->first &&
      Next < CurrentFuncRange->second) {
    const auto [Begin, End] = *CurrentFuncRange;
    // Coarse unwind spans support interior queries, but only a real function
    // end or an independent entry can clip this continuation.
    if (codeRangeEndsAt(Begin, End) ||
        isTailCallTarget(End, /*IncludeBoundaryGuesses=*/false))
      RangeEnd = End;
  }
  size_t UnownedScanned = 0;
  for (;;) {
    if (!chargeCandidateDecodeAttempt(Img))
      return false;
    if (isTailCallTarget(Next, /*IncludeBoundaryGuesses=*/false))
      return false;
    const bool OwnedFragment = isCurrentOwnedFragment(Next);
    if (Next >= RangeEnd && !OwnedFragment)
      return false;
    const bool Owned = isCurrentOwnedCode(Next);
    if (!Owned && UnownedScanned > limits::kMaxAlignmentPaddingBytes)
      return false;
    const auto Fail = [&](bool Truncated) {
      // Size evidence makes this a real continuation. A failed peek cannot
      // turn malformed or missing owned code into a complete function.
      if (!Owned)
        return false;
      (Truncated ? TruncatedPathAddresses : DecodeFailureAddresses)
          .insert(Next);
      // Retain the attempted fallthrough in final reachability accounting.
      // Do not retry with the unbounded decoder and read past its owner.
      ExploredAddrs.insert(Next);
      return true;
    };
    const Segment *Seg = Img.getSegmentFor(Next);
    if (!Seg || Next < Seg->VA || !Img.hasExecutableCodeOwnerAt(Next))
      return Fail(/*Truncated=*/true);
    const size_t Offset = static_cast<size_t>(Next - Seg->VA);
    if (Offset >= Seg->Data.size())
      return Fail(/*Truncated=*/true);
    size_t Available = Seg->Data.size() - Offset;
    if (!Owned && Next < RangeEnd)
      Available = std::min<uint64_t>(Available, RangeEnd - Next);
    if (!PeekDecoder.selectMode(Img, Next, Dec.currentMode()))
      return Fail(/*Truncated=*/false);
    PeekDecoder.setDetail(false);
    DecodedInsn Peek;
    const int Size = PeekDecoder.decodeOneLight(Seg->Data.data() + Offset,
                                                Available, Next, Peek);
    if (Size <= 0 || static_cast<uint64_t>(Size) > InvalidVA - Next)
      return Fail(/*Truncated=*/false);
    if (!Img.hasExecutableCodeOwnerRange(Next, static_cast<uint64_t>(Size)))
      return Fail(/*Truncated=*/true);
    const va_t End = Next + Size;
    if (Img.Arch == Arch::ARM &&
        Img.instructionModeAt(End - 1, PeekDecoder.currentMode()) !=
            PeekDecoder.currentMode())
      return Fail(/*Truncated=*/false);
    if (!instructionFitsCurrentOwnership(Next, Size))
      return Fail(/*Truncated=*/false);
    const bool Nop = PeekDecoder.isAlignmentNop(Peek);
    // Sized ownership admits each breakpoint, even a long consecutive run.
    // NOPs have no observable continuation of their own: even an owned run
    // must lead to code before a function boundary. Its finite ownership,
    // rather than the heuristic padding budget, bounds this linear scan.
    if (Owned && !Nop)
      return true;
    const bool Padding = PeekDecoder.isResumableTrap(Peek) || Nop;
    if (!Padding)
      return true;
    // With no size evidence, look through a bounded run for real code before
    // another function. Do not let trap/NOP alignment manufacture a tail call.
    if (codeRangeEndsAt(Next, End))
      return false;
    if (!Owned)
      UnownedScanned += Size;
    Next = End;
  }
}

std::set<va_t>
CFGBuilder::proveClosedOwnSubroutines(const BinaryImage &Img) const {
  if (Img.Arch != Arch::X64 || OwnInteriorCallSites.empty())
    return {};
  const auto *Owner = Img.ExceptionMetadata.findFunction(CurrentFuncEntry);
  if (!Owner || Owner->Kind != RuntimeFunctionKind::Primary ||
      Owner->CodeRange.Begin != CurrentFuncEntry ||
      !Owner->describesUnwindOnly() ||
      !exceptionEncodingBoundsOneFunction(Owner->Encoding) ||
      Owner->CodeRange.End <= CurrentFuncEntry ||
      Owner->CodeRange.End - CurrentFuncEntry >
          limits::ir::kOwnInteriorReturnProofBytes ||
      OwnInteriorCallSites.size() >=
          limits::ir::kOwnInteriorReturnProofFunctions)
    return {};
  std::set<va_t> Entries{CurrentFuncEntry};
  for (const auto &[Target, Sites] : OwnInteriorCallSites) {
    if (!Owner->CodeRange.contains(Target))
      return {};
    Entries.insert(Target);
  }
  Decoder Dec;
  if (!Dec.init(Img))
    return {};
  StackFrameModel Model(getTargetRegInfo(Img.Arch), Img.abiFormat());
  Model.CallsKeepStackPointer = true;
  for (va_t Entry : Entries) {
    CFGBuilder Builder;
    Builder.SkipCalleeRegisterProofs = true;
    Builder.setKnownFuncEntries(&Entries);
    Builder.setExecutableCodeOwnerIndex(ExecutableCodeOwners);
    const LowFunc Body = Builder.buildIntegerFrameProof(Img, Dec, Entry, "");
    if (Body.Blocks.empty() || !Body.hasCompleteLiftCoverage() ||
        !Body.UnsafeIndirectBranchAddresses.empty())
      return {};
    std::vector<std::optional<StackOffsetState>> In(Body.Blocks.size());
    In.front() = StackOffsetState{{Model.StackPointer, {0, 0, 0}}};
    solveStackOffsets(Model, Body, In);
    bool HasReturn = false;
    for (size_t B = 0; B < Body.Blocks.size(); ++B) {
      const auto &Block = Body.Blocks[B];
      if (!In[B] || !Owner->CodeRange.contains(Block.StartAddr) ||
          Block.EndAddr > Owner->CodeRange.End ||
          !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
        return {};
      StackOffsetState State = *In[B];
      bool Returns = false;
      for (const LowOp &Op : Block.Ops) {
        if (Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INDIR_BR ||
            Op.Opcode == NdOp::INTRINSIC)
          return {};
        if (Op.Output.isReg() && Op.Output.Offset != Model.TRI->StackPointer &&
            Model.TRI->isCallPreserved(Op.Output.Offset, Op.Output.Size,
                                       Model.Format))
          return {}; // No invented ordinary ABI for a custom register helper.
        const auto SP = State.find(Model.StackPointer);
        if (Op.Opcode == NdOp::CALL) {
          // Simultaneous induction over the closed group: every callee can
          // write only below its entry SP, and returns through its unchanged
          // entry PC. No assumption about an external callee enters the proof.
          if (!Op.NumInputs || !Op.Inputs[0].isConst() ||
              !Entries.count(Op.Inputs[0].Offset) || SP == State.end() ||
              SP->second.Base != 0 || SP->second.Hi > 0)
            return {};
        } else if (Op.Opcode == NdOp::RETURN) {
          if (SP == State.end() || SP->second != StackOffsetRange{0, 0, 0})
            return {};
          const Segment *Segment = Img.getSegmentFor(Op.Addr);
          if (!Segment || Op.Addr < Segment->VA ||
              Op.Addr - Segment->VA >= Segment->Data.size())
            return {};
          const uint8_t Opcode = Segment->Data[Op.Addr - Segment->VA];
          // A synthetic tail return belongs to a direct CALL in this group;
          // all physical returns must be the plain full-width near RET.
          const bool Tail = llvm::any_of(Block.Ops, [&](const LowOp &Call) {
            return Call.Addr == Op.Addr && Call.Opcode == NdOp::CALL;
          });
          if (Opcode != 0xC3 && !Tail)
            return {};
          Returns = HasReturn = true;
        } else if (Op.Opcode == NdOp::STORE || Op.Opcode == NdOp::ATOMIC_XCHG ||
                   Op.Opcode == NdOp::ATOMIC_ADD ||
                   Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
          const auto Memory = lowMemoryOperands(Op);
          if (!Memory.Complete || !Memory.Address ||
              Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
            return {};
          const auto Address =
              State.find({Memory.Address->Space, Memory.Address->Offset,
                          Memory.Address->Size});
          if (Address == State.end() || Address->second.Base != 0 ||
              Address->second.Hi > -int64_t(Memory.AccessSize))
            return {};
        }
        transferStackOffsets(Model, Op, State);
      }
      if (Block.Succs.empty() && !Returns)
        return {};
    }
    if (!HasReturn)
      return {};
  }
  Entries.erase(CurrentFuncEntry);
  return Entries;
}

CFGBuilder::OwnInteriorCallVerdict
CFGBuilder::classifyOwnInteriorCalls(const BinaryImage &Img,
                                     const LowFunc &Func) const {
  OwnInteriorCallVerdict Verdict;
  const size_t N = Func.Blocks.size();
  if (N == 0)
    return Verdict;
  StackFrameModel M(getTargetRegInfo(Img.Arch), Img.abiFormat());
  M.CallsKeepStackPointer = Img.Arch == Arch::X64;
  const StackOffsetKey &StackPointerKey = M.StackPointer;
  // Offsets from the entry stack pointer.  Any other root, including a block
  // an exceptional edge enters, starts with nothing known.
  std::vector<std::optional<StackOffsetState>> FromEntry(N);
  FromEntry[0] = StackOffsetState{{StackPointerKey, {0, 0, 0}}};
  for (size_t B = 1; B < N; ++B)
    if (Func.Blocks[B].Preds.empty() ||
        !Func.Blocks[B].ExceptionalPreds.empty())
      FromEntry[B] = StackOffsetState{};
  solveStackOffsets(M, Func, FromEntry);

  std::map<va_t, size_t> BlockAt;
  for (size_t B = 0; B < N; ++B)
    BlockAt[Func.Blocks[B].StartAddr] = B;
  auto StateBefore = [&](va_t Addr) -> std::optional<StackOffsetState> {
    for (size_t B = 0; B < N; ++B) {
      const LowBlock &Block = Func.Blocks[B];
      if (Addr < Block.StartAddr || Addr >= Block.EndAddr)
        continue;
      if (!FromEntry[B])
        return std::nullopt;
      StackOffsetState S = *FromEntry[B];
      for (const LowOp &Op : Block.Ops) {
        if (Op.Addr == Addr)
          break;
        transferStackOffsets(M, Op, S);
      }
      return S;
    }
    return std::nullopt;
  };
  auto Reach = [&](size_t From, const std::set<va_t> &Returns) {
    std::vector<bool> Reached(N, false);
    std::vector<int> Work{static_cast<int>(From)};
    while (!Work.empty()) {
      const int B = Work.back();
      Work.pop_back();
      if (Reached[B])
        continue;
      Reached[B] = true;
      if (!Func.Blocks[B].Ops.empty() &&
          Returns.count(Func.Blocks[B].Ops.back().Addr))
        continue;
      for (int S : Func.Blocks[B].Succs)
        if (S >= 0 && static_cast<size_t>(S) < N)
          Work.push_back(S);
    }
    return Reached;
  };
  auto NoteUnproven = [&](va_t Addr) {
    if (!Verdict.Unproven)
      Verdict.Unproven = Addr;
  };
  // A direct tail call lifts as `call target; ret`. Where the target is
  // proved never to return, as this build's own calls are, that RET never
  // executes: the path ends there without popping anything.
  auto EndsInNoReturnTail = [&](const LowOp &Op) {
    if (Op.Opcode != NdOp::RETURN)
      return false;
    const auto It = Insns.find(Op.Addr);
    return It != Insns.end() && It->second.IsCall && It->second.IsRet &&
           !It->second.IsIndirect && It->second.Immediate &&
           calleeNeverReturns(Img, *It->second.Immediate, NoReturnTargets,
                              NoReturnCallees, NoReturnCalleeDepth);
  };

  // A retpoline replaces an internal call's pushed PC, then pops it as an
  // indirect tail transfer. Prove the architectural slot and the enclosing
  // ABI stack independently: a matching SP alone says nothing about either
  // PC. Keep the STORE and the eventual LOAD, including their fault effects.
  if (Img.Arch == Arch::X64 && Func.hasCompleteLiftCoverage())
    for (size_t B = 0; B < N; ++B) {
      const auto &Block = Func.Blocks[B];
      if (!FromEntry[B] || Block.Ops.empty())
        continue;
      const va_t Address = Block.Ops.back().Addr;
      if (Block.Ops.back().Opcode != NdOp::RETURN)
        continue;
      const auto *Segment = Img.getSegmentFor(Address);
      if (!Segment || Address < Segment->VA ||
          Address - Segment->VA >= Segment->Data.size() ||
          Segment->Data[Address - Segment->VA] != 0xc3 ||
          Block.EndAddr != Address + 1)
        continue;
      size_t ReturnIndex = Block.Ops.size() - 1;
      while (ReturnIndex && Block.Ops[ReturnIndex - 1].Addr == Address)
        --ReturnIndex;
      if (!ReturnIndex)
        continue;
      const LowOp &Store = Block.Ops[ReturnIndex - 1];
      if (Store.Opcode != NdOp::STORE || Store.NumInputs != 2 ||
          Store.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Store.Inputs[1].Size != M.Width || !Store.Inputs[1].isReg())
        continue;
      StackOffsetState Before = *FromEntry[B];
      for (size_t I = 0; I + 1 < ReturnIndex; ++I)
        transferStackOffsets(M, Block.Ops[I], Before);
      const auto Slot =
          Before.find({Store.Inputs[0].Space, Store.Inputs[0].Offset,
                       Store.Inputs[0].Size});
      const auto SP = Before.find(StackPointerKey);
      const StackOffsetRange PushedSlot{0, -int64_t(M.Width),
                                        -int64_t(M.Width)};
      if (Slot == Before.end() || SP == Before.end() ||
          Slot->second != PushedSlot || SP->second != PushedSlot)
        continue;
      bool Valid = true;
      std::vector<bool> Seen(N, false);
      std::vector<size_t> Pending{B};
      while (Valid && !Pending.empty()) {
        const size_t P = Pending.back();
        Pending.pop_back();
        if (Seen[P])
          continue;
        Seen[P] = true;
        const LowBlock &Parent = Func.Blocks[P];
        if (!FromEntry[P] || !Parent.ExceptionalPreds.empty() ||
            (P != 0 &&
             (Parent.Preds.empty() ||
              Func.ModuleAnalysisRoots.count(Parent.StartAddr) ||
              Func.OrdinaryModuleAnalysisRoots.count(Parent.StartAddr)))) {
          Valid = false;
          break;
        }
        StackOffsetState State = *FromEntry[P];
        for (const LowOp &Op : Parent.Ops) {
          if (P == B && Op.Addr == Address)
            break;
          if (mayWriteOwnReturnSlot(M, Op, State)) {
            Valid = false;
            break;
          }
          transferStackOffsets(M, Op, State);
        }
        for (int Pred : Parent.Preds)
          if (Pred < 0 || size_t(Pred) >= N)
            Valid = false;
          else
            Pending.push_back(size_t(Pred));
      }
      if (Valid)
        Verdict.TailReturns.insert(Address);
    }

  std::vector<bool> ReachedByAny(N, false);
  std::set<va_t> NonlocalReturns;
  for (const auto &[Target, Sites] : OwnInteriorCallSites) {
    auto TargetBlock = BlockAt.find(Target);
    if (TargetBlock == BlockAt.end())
      continue;
    // Offsets from the slot the call pushed its return address into, which
    // is the stack pointer the target starts with.  A value whose offset
    // from the stack pointer is bounded at every call site keeps the bound
    // that holds for all of them.
    std::optional<StackOffsetState> Seed;
    for (va_t Site : Sites) {
      StackOffsetState Relative{{StackPointerKey, {0, 0, 0}}};
      if (const auto AtSite = StateBefore(Site)) {
        auto SP = AtSite->find(StackPointerKey);
        if (SP != AtSite->end())
          for (const auto &[K, V] : *AtSite) {
            if (K == StackPointerKey || V.Base != SP->second.Base)
              continue;
            // V - (SP - Width), with SP anywhere in its range.
            const int64_t Lo =
                V.Lo == kNoLowerBound || SP->second.Hi == kNoUpperBound
                    ? kNoLowerBound
                    : V.Lo - SP->second.Hi + M.Width;
            const int64_t Hi =
                V.Hi == kNoUpperBound || SP->second.Lo == kNoLowerBound
                    ? kNoUpperBound
                    : V.Hi - SP->second.Lo + M.Width;
            if (Lo != kNoLowerBound || Hi != kNoUpperBound)
              Relative[K] = {0, Lo, Hi};
          }
      }
      if (!Seed) {
        Seed = std::move(Relative);
        continue;
      }
      StackOffsetState Met;
      for (const auto &[K, V] : *Seed)
        if (auto It = Relative.find(K); It != Relative.end())
          Met.emplace(K, StackOffsetRange{0, std::min(V.Lo, It->second.Lo),
                                          std::max(V.Hi, It->second.Hi)});
      Seed = std::move(Met);
    }
    if (!Seed)
      continue;

    std::set<va_t> ReplayedReturns = Verdict.TailReturns;
    for (const auto &[Address, Return] : OwnInteriorReturns)
      if (Return.Target == Target)
        ReplayedReturns.insert(Address);
    const std::vector<bool> Reached =
        Reach(TargetBlock->second, ReplayedReturns);
    for (size_t B = 0; B < N; ++B)
      ReachedByAny[B] = ReachedByAny[B] || Reached[B];
    std::vector<std::optional<StackOffsetState>> FromSlot(N);
    FromSlot[TargetBlock->second] = *Seed;
    solveStackOffsets(M, Func, FromSlot, ReplayedReturns);

    // Where the slot-relative offset is lost, the entry stack pointer still
    // places the return above every slot the calls pushed when all of them
    // lie below it: the entry runs once, so its offsets compare anywhere.
    std::optional<int64_t> HighestSlot;
    for (va_t Site : Sites) {
      const auto AtSite = StateBefore(Site);
      auto SP = AtSite ? AtSite->find(StackPointerKey)
                       : StackOffsetState::const_iterator();
      if (!AtSite || SP == AtSite->end() || SP->second.Base != 0 ||
          SP->second.Hi == kNoUpperBound) {
        HighestSlot.reset();
        break;
      }
      HighestSlot = std::max(HighestSlot.value_or(kNoLowerBound),
                             SP->second.Hi - M.Width);
    }
    auto AboveEverySlot = [&](va_t Return) {
      if (!HighestSlot)
        return false;
      const auto AtReturn = StateBefore(Return);
      if (!AtReturn)
        return false;
      auto SP = AtReturn->find(StackPointerKey);
      return SP != AtReturn->end() && SP->second.Base == 0 &&
             SP->second.Lo != kNoLowerBound && SP->second.Lo > *HighestSlot;
    };

    // At a return, an offset of exactly zero pops this call's own return
    // address: the target is a subroutine.  An offset above zero means the
    // address was discarded first.  Anything else is not proven.
    bool ConsumesSlot = false, Discards = false;
    std::vector<bool> MayWriteSlot(N, false);
    std::vector<size_t> ConsumingReturns;
    std::map<va_t, size_t> ConsumingSites;
    std::set<va_t> DiscardingSites;
    std::optional<va_t> Unproven;
    for (size_t B = 0; B < N && !Unproven; ++B) {
      if (!Reached[B])
        continue;
      StackOffsetState S = FromSlot[B] ? *FromSlot[B] : StackOffsetState{};
      StackOffsetState Parent =
          FromEntry[B] ? *FromEntry[B] : StackOffsetState{};
      for (const LowOp &Op : Func.Blocks[B].Ops) {
        if (Op.Seq == 0 && Verdict.TailReturns.count(Op.Addr)) {
          Discards = true;
          break;
        }
        if (EndsInNoReturnTail(Op))
          break;
        const bool Replayed = Op.Seq == 0 && ReplayedReturns.count(Op.Addr);
        if (Op.Opcode == NdOp::RETURN || Replayed) {
          auto SP = S.find(StackPointerKey);
          const bool Slot = SP != S.end() && SP->second.Base == 0;
          if (Slot && SP->second.Lo == 0 && SP->second.Hi == 0) {
            ConsumesSlot = true;
            ConsumingReturns.push_back(B);
            ConsumingSites.emplace(Op.Addr, B);
          } else if ((Slot && SP->second.Lo > 0) || AboveEverySlot(Op.Addr)) {
            Discards = true;
            DiscardingSites.insert(Op.Addr);
          } else {
            Unproven = Op.Addr;
            break;
          }
          if (Replayed)
            break;
          continue;
        }
        // Returning through an intact internal PC does not prove the
        // enclosing invocation's PC survived. Both coordinate systems must
        // exclude every write reaching an ordinary internal return.
        MayWriteSlot[B] = MayWriteSlot[B] || mayWriteOwnReturnSlot(M, Op, S) ||
                          mayWriteOwnReturnSlot(M, Op, Parent);
        transferStackOffsets(M, Op, S);
        transferStackOffsets(M, Op, Parent);
      }
    }
    // A clobber matters only when it can flow to an ordinary return.
    // Other exits retain their existing nonlocal/unproven classification.
    std::vector<bool> Checked(N, false);
    while (!Unproven && !ConsumingReturns.empty()) {
      const size_t B = ConsumingReturns.back();
      ConsumingReturns.pop_back();
      if (Checked[B] || !Reached[B])
        continue;
      Checked[B] = true;
      if (MayWriteSlot[B]) {
        Unproven = Target;
        break;
      }
      for (int P : Func.Blocks[B].Preds)
        if (P >= 0 && size_t(P) < N)
          ConsumingReturns.push_back(size_t(P));
    }
    if (!Unproven && ConsumesSlot && Discards) {
      // Resolve an invocation, not an arbitrary RET whose SP happens to
      // match. There must be one call site, no independent entry reaching a
      // consuming return, and no path back to the call before that return.
      // Language EH activations require their own parent/callback bridge.
      bool Valid = Sites.size() == 1 && Func.hasCompleteLiftCoverage();
      const auto Call = Insns.find(*Sites.begin());
      Valid &= Call != Insns.end();
      const va_t Continuation =
          Call == Insns.end() ? InvalidVA : Call->first + Call->second.Size;
      const auto *Own = Img.ExceptionMetadata.findFunction(*Sites.begin());
      Valid &= Own && Own->CodeRange.contains(Continuation) &&
               Own->describesUnwindOnly();
      std::optional<size_t> Caller;
      for (size_t B = 0; B < N; ++B) {
        if (Func.Blocks[B].StartAddr <= *Sites.begin() &&
            *Sites.begin() < Func.Blocks[B].EndAddr)
          Caller = B;
        if (Reached[B] && llvm::any_of(Func.Blocks[B].Ops, [](const LowOp &Op) {
              return Op.Opcode == NdOp::INDIR_BR;
            }))
          Valid = false;
      }
      Valid &= Caller && !Reached[*Caller];
      std::vector<size_t> Pending;
      for (const auto &[Address, Block] : ConsumingSites) {
        const auto *Segment = Img.getSegmentFor(Address);
        Valid &= Segment && Address >= Segment->VA &&
                 Address - Segment->VA < Segment->Data.size() &&
                 Segment->Data[Address - Segment->VA] == 0xc3 &&
                 Func.Blocks[Block].EndAddr == Address + 1;
        Pending.push_back(Block);
      }
      std::vector<bool> Seen(N, false);
      while (Valid && !Pending.empty()) {
        const size_t B = Pending.back();
        Pending.pop_back();
        if (Seen[B])
          continue;
        Seen[B] = true;
        const auto &Block = Func.Blocks[B];
        if (Block.Preds.empty() || !Block.ExceptionalPreds.empty() ||
            Func.ModuleAnalysisRoots.count(Block.StartAddr) ||
            Func.OrdinaryModuleAnalysisRoots.count(Block.StartAddr)) {
          Valid = false;
          break;
        }
        for (int P : Block.Preds) {
          if (B == TargetBlock->second && size_t(P) == *Caller)
            continue;
          if (P < 0 || size_t(P) >= N || !Reached[P]) {
            Valid = false;
            break;
          }
          Pending.push_back(size_t(P));
        }
      }
      if (Valid) {
        NonlocalReturns.insert(DiscardingSites.begin(), DiscardingSites.end());
        const OwnInteriorReturn Return{Target, *Sites.begin(), Continuation};
        for (const auto &[Address, Block] : ConsumingSites) {
          const auto [It, New] = Verdict.Returns.emplace(Address, Return);
          if (!New && It->second != Return)
            Unproven = Address;
        }
      } else
        Unproven = Target;
    }
    if (Unproven)
      NoteUnproven(*Unproven);
    else if (ConsumesSlot && !Discards)
      Verdict.Subroutines.insert(Target);
  }

  // Abandoning an internal PC does not authorize an arbitrary outer RET.
  // The parent's physical PC must still occupy the entry slot and survive
  // every reaching write. This is independent of the internal-slot proof:
  // a write disjoint from that slot can corrupt the parent instead.
  if (!NonlocalReturns.empty() && !Verdict.Unproven) {
    std::vector<bool> MayWriteParentSlot(N, false), Seen(N, false);
    std::vector<size_t> Pending;
    for (size_t B = 0; B < N; ++B) {
      StackOffsetState S = FromEntry[B] ? *FromEntry[B] : StackOffsetState{};
      for (const LowOp &Op : Func.Blocks[B].Ops) {
        if (Op.Opcode == NdOp::RETURN && NonlocalReturns.count(Op.Addr) &&
            !Verdict.Returns.count(Op.Addr)) {
          const auto SP = S.find(StackPointerKey);
          if (SP == S.end() || SP->second != StackOffsetRange{0, 0, 0})
            NoteUnproven(Op.Addr);
          Pending.push_back(B);
        }
        MayWriteParentSlot[B] =
            MayWriteParentSlot[B] || mayWriteOwnReturnSlot(M, Op, S);
        transferStackOffsets(M, Op, S);
      }
    }
    while (!Pending.empty() && !Verdict.Unproven) {
      const size_t B = Pending.back();
      Pending.pop_back();
      if (Seen[B])
        continue;
      Seen[B] = true;
      const auto &Block = Func.Blocks[B];
      if (MayWriteParentSlot[B] || !Block.ExceptionalPreds.empty() ||
          (Block.StartAddr != Func.Entry &&
           (Block.Preds.empty() ||
            Func.ModuleAnalysisRoots.count(Block.StartAddr) ||
            Func.OrdinaryModuleAnalysisRoots.count(Block.StartAddr)))) {
        NoteUnproven(Block.StartAddr);
        break;
      }
      for (int P : Block.Preds)
        if (P < 0 || size_t(P) >= N)
          NoteUnproven(Block.StartAddr);
        else
          Pending.push_back(size_t(P));
    }
  }

  // Whatever the targets do with their own slots, a return they lead to
  // below the entry stack pointer pops something this function pushed.
  for (size_t B = 0; B < N && !Verdict.Unproven; ++B) {
    if (!ReachedByAny[B] || !FromEntry[B])
      continue;
    StackOffsetState S = *FromEntry[B];
    for (const LowOp &Op : Func.Blocks[B].Ops) {
      if (Op.Opcode == NdOp::RETURN) {
        if (Verdict.Returns.count(Op.Addr) ||
            Verdict.TailReturns.count(Op.Addr) || EndsInNoReturnTail(Op))
          continue;
        auto SP = S.find(StackPointerKey);
        if (SP != S.end() && SP->second.Base == 0 && SP->second.Hi < 0) {
          NoteUnproven(Op.Addr);
          break;
        }
        continue;
      }
      transferStackOffsets(M, Op, S);
    }
  }
  return Verdict;
}

LowFunc CFGBuilder::build(const BinaryImage &Img, Decoder &Dec, va_t EntryAddr,
                          const std::string &FuncName) {
  // A call into the function's own body starts as a push and a jump.  Each
  // target found to be a subroutine, one whose return pops that call's own
  // return address, is lifted again as an ordinary call. A single invocation
  // with separate normal and nonlocal exits can resume its proved RET at the
  // exact continuation; decoding that continuation must revalidate the proof.
  // Anything else makes the return that breaks the proof unsupported.
  KeptOwnInteriorCallTargets.clear();
  OwnInteriorReturns.clear();
  OwnInteriorTailReturns.clear();
  RegistrationCallees.reset();
  X87CallEffects.reset();
  for (;;) {
    LowFunc Func = buildOnce(Img, Dec, EntryAddr, FuncName);
    if (OwnInteriorCallSites.empty())
      return Func;
    OwnInteriorCallVerdict Verdict = classifyOwnInteriorCalls(Img, Func);
    for (va_t Address : OwnInteriorTailReturns)
      if (!Verdict.TailReturns.count(Address))
        Verdict.Unproven = Address;
    for (const auto &[Address, Return] : OwnInteriorReturns)
      if (auto It = Verdict.Returns.find(Address);
          It == Verdict.Returns.end() || It->second != Return)
        Verdict.Unproven = Address;
    if (!Verdict.Subroutines.empty()) {
      KeptOwnInteriorCallTargets.insert(Verdict.Subroutines.begin(),
                                        Verdict.Subroutines.end());
      continue;
    }
    if (!Verdict.Unproven && (Verdict.Returns != OwnInteriorReturns ||
                              Verdict.TailReturns != OwnInteriorTailReturns)) {
      OwnInteriorReturns = std::move(Verdict.Returns);
      OwnInteriorTailReturns = std::move(Verdict.TailReturns);
      continue;
    }
    if (Verdict.Unproven) {
      if (const auto Closed = proveClosedOwnSubroutines(Img); !Closed.empty()) {
        KeptOwnInteriorCallTargets.insert(Closed.begin(), Closed.end());
        continue;
      }
      Func.UnprovenReturnAddresses.push_back(*Verdict.Unproven);
      if (!llvm::is_contained(Func.UnsupportedInstructionAddresses,
                              *Verdict.Unproven))
        Func.UnsupportedInstructionAddresses.push_back(*Verdict.Unproven);
    }
    return Func;
  }
}

LowFunc CFGBuilder::buildOnce(const BinaryImage &Img, Decoder &Dec,
                              va_t EntryAddr, const std::string &FuncName) {
  RelativeRelocationRootSourceCachePrepared = false;
  RelativeRelocationRootSourceCacheImage = nullptr;
  RelativeRelocationRootSourceCacheArch = Arch::Unknown;
  RelativeRelocationRootSourceCacheMode = InstructionMode::Default;
  RelativeRelocationRootSourceCacheSlotCount = 0;
  RelativeRelocationRootSourceCache.clear();
  RelativeRelocationRootSourceCacheLookupCountForTesting = 0;
  CachedResolverGraph.reset();
  CachedUnmeteredResolverGraph.reset();
  Insns.clear();
  BlockStarts.clear();
  ExploredAddrs.clear();
  CallTargets.clear();
  OwnInteriorCallSites.clear();
  DiscoveredCodeRefs.clear();
  DiscoveredCodeRefSources.clear();
  ResolvedTableInfo.clear();
  GuardedGroupProofContext.reset();
  GuardedGroupIdentity.reset();
  GuardedGroupState.reset();
  GuardedGroupRejected = false;
  JumpTableGroupLifecycleForTesting = {};
  PriorStrongJumpTableProposals.clear();
  NextStrongJumpTableProposals.clear();
  PriorProvisionalRelativeEdges.clear();
  NextProvisionalRelativeEdges.clear();
  StrongJumpTableProposalOutcomes.clear();
  CandidateProposalStageMutationAddrs.clear();
  QuarantinedJumpTableProposals.clear();
  EverStrongJumpTableProposalBranches.clear();
  CandidateProposalStageActive = false;
  CandidateProposalStageEvidenceRemaining = 0;
  CandidateProposalStageEvidenceIncomplete = false;
  IncompleteBranchMarkerEvidenceRemaining =
      std::min<size_t>(limits::kMaxJumpTableProposalStageEvidenceWork,
                       IncompleteBranchMarkerEvidenceBudgetForTesting.value_or(
                           limits::kMaxJumpTableProposalStageEvidenceWork));
  IncompleteBranchMarkerEvidenceIncomplete = false;
  I386GOTOFFTombstoneLookupCountForTesting = 0;
  ProposalStageCommitTailEvidenceExhaustedForTesting = false;
  CommitTailRollbackRetainedPendingI386AmbiguityForTesting = false;
  ExhaustedStableI386AmbiguityCommitTailForTesting = false;
  ExhaustedProposalStageCommitTailForTesting = false;
  ProposalStageForcedCommitTailRollbackPreservedStateForTesting = false;
  ProposalStageRollbackMutatedQuarantineForTesting = false;
  UntrackedJumpTableCandidateExhaustedThisStageForTesting = false;
  UntrackedJumpTableCandidateRollbackObservedForTesting = false;
  ForcedUntrackedJumpTableCandidateAddrForTesting = InvalidVA;
  UntrackedJumpTableCandidateProvisionalStateObservedForTesting = false;
  UntrackedJumpTableCandidateStateClearedOnRollbackForTesting = false;
  RecordedCompleteRuntimeStorageCertificateForTesting = false;
  NestedMutationTrackingEvidenceExhaustedForTesting = false;
  NestedMutationTrackingEvidenceExhaustedAddrForTesting = InvalidVA;
  ConstBaseLocalShapeClaimedForTesting = false;
  ConstBasePostShapeAnalysisIncompleteForTesting = false;
  ConstBaseFirstLocalShapeClaimedAddrForTesting = InvalidVA;
  ConstBaseSecondLocalShapeClaimedAddrForTesting = InvalidVA;
  ConstBaseLocalShapeClaimedAddrOverflowForTesting = false;
  ProposalCleanupEvidenceForTesting = {};
  JumpTableProofContextComplete = false;
  RequestedCompleteJumpTableProof = false;
  PersistentCFGRoots.clear();
  CalleePreservedGPRs.clear();
  OrdinaryCFGRoots.clear();
  DurableCFGRoots.clear();
  CurrentExceptionalEntries.clear();
  RelocationCFGRootSources.clear();
  ActiveJumpTableProofRoots.reset();
  ActiveJumpTableCandidateAddr = InvalidVA;
  ActiveJumpTableCandidateProofRank = 0;
  ActiveJumpTableCandidateDependencyRank = 0;
  ActiveJumpTableConsumerAudit = false;
  PotentialJumpTableBranches.clear();
  EverPublishedJumpTableBranches.clear();
  LostValidatedJumpTableBranches.clear();
  StackTableEvidenceIncompleteBranches.clear();
  IndexDomainEvidenceIncompleteBranches.clear();
  ShapeClaimedBranches.clear();
  ValidatedPhysicalJumpTableBranches.clear();
  CandidateFixedPointExplorationTargets.clear();
  MaxCandidateFixedPointExplorationTargetCountForTesting = 0;
  AmbiguousI386GOTPCBranches.clear();
  PendingAmbiguousI386GOTPCBranches.clear();
  PendingAmbiguousI386GOTPCKeys.clear();
  StageAmbiguousI386GOTPCBranches.clear();
  StageReplayedI386GOTPCKeys.clear();
  CurrentI386GOTOFFAmbiguityKeys.clear();
  PublishedReachableInsns.clear();
  PublishedBlockStarts.clear();
  DecodedInstructionCount = 0;
  LiftedInstructionCount = 0;
  DecodedInstructionAddresses.clear();
  LiftedInstructionAddresses.clear();
  DecodeFailureAddresses.clear();
  UnsupportedInstructionAddresses.clear();
  RemainingX86BoundedLeafWork = limits::kMaxX86BoundedLeafEvidenceWork;
  TruncatedPathAddresses.clear();
  RelocatedInstructionAddressOccurrences.clear();
  I386GetPcOccurrences.clear();
  RelocatedInstructionScalarOperandOccurrences.clear();
  RelocatedInstructionScalarModelOccurrences.clear();
  I386GOTOFFProposalRootCache.clear();
  I386GOTOFFModelReachCache.clear();
  I386GOTOFFProposalEvidenceRemaining = 0;
  I386GOTOFFProposalShapeClaimed = false;
  I386GOTOFFProposalEvidenceIncomplete = false;
  I386GOTModelEvidenceIncomplete = false;
  I386GOTOFFAmbiguousModelReach = false;
  I386GOTOFFGraphQueryIssuedForTesting = false;
  I386GOTOFFGraphQueryBudgetExhaustedForTesting = false;
  StackTableEvidenceRemaining =
      std::min<size_t>(limits::kMaxJumpTableStackEvidenceWork,
                       StackTableEvidenceBudgetForTesting.value_or(
                           limits::kMaxJumpTableStackEvidenceWork));
  PreviouslyPublishedJumpTableBranches =
      std::move(PendingPreviouslyPublishedJumpTableBranches);
  PendingPreviouslyPublishedJumpTableBranches.clear();

  CurrentFuncEntry = EntryAddr;
  CurrentFuncRange.reset();
  AuthoritativeCurrentFuncRange.reset();
  CurrentImg = &Img;
  // The x87 TOP counter persists across functions in the shared lifter; start
  // each function with an empty stack so the entry block's lift TOP is 0.
  Dec.resetX86FpuState();
  BlockStarts.insert(EntryAddr);
  PersistentCFGRoots.insert(EntryAddr);
  OrdinaryCFGRoots.insert(EntryAddr);
  DurableCFGRoots.insert(EntryAddr);

  const ExceptionFunction *Exception = nullptr;
  for (const ExceptionFunction &Candidate : Img.ExceptionMetadata.Functions) {
    if (Candidate.CodeRange.Begin == EntryAddr &&
        Candidate.Kind == RuntimeFunctionKind::Primary) {
      Exception = &Candidate;
      break;
    }
  }
  if (!Exception) {
    Exception = Img.ExceptionMetadata.findFunction(EntryAddr);
    // A registration record is its installer's alone; its range also covers
    // what lies after the installer, up to the next function the loader knew.
    if (Exception && Exception->Registration)
      Exception = registration_state::installedRegistration(Img, EntryAddr);
  }
  std::vector<va_t> ExceptionalRoots;
  std::vector<va_t> ContinuationRoots;
  if (Exception) {
    // Every one of these tables spells "this field names no address" as zero,
    // so a zero has to be dropped before the range test rather than left to it.
    // The function's code includes its chained cold fragments.
    auto AddBoundary = [&](va_t Address) {
      if (Address != 0 && Exception->ownsCode(Address) && Address != EntryAddr)
        BlockStarts.insert(Address);
    };
    auto AddExceptionalRoot = [&](va_t Address) {
      if (Address == 0 || !Exception->ownsCode(Address))
        return;
      AddBoundary(Address);
      PersistentCFGRoots.insert(Address);
      DurableCFGRoots.insert(Address);
      if (Address != EntryAddr)
        ExceptionalRoots.push_back(Address);
    };
    auto AddContinuationRoot = [&](va_t Address) {
      if (Address == 0 || !Exception->ownsCode(Address))
        return;
      if (Address != EntryAddr)
        ContinuationRoots.push_back(Address);
    };
    if (Exception->SEH)
      for (const SEHScopeRecord &Scope : Exception->SEH->Scopes) {
        if (const auto Range =
                getSemanticSEHGuardedRange(Scope, Img.Arch, *Exception)) {
          AddBoundary(Range->Begin);
          AddBoundary(Range->End);
        }
        AddExceptionalRoot(Scope.FilterOrFinallyVA);
        AddExceptionalRoot(Scope.HandlerVA);
        AddBoundary(Scope.ContinuationVA);
      }
    if (Exception->Cxx) {
      // IP map entries that keep the current state (including the implicit
      // entry state -1) must not cut a block.  MSVC often plants the first
      // IP on a call (`GetRank`); splitting there leaves `mov rcx, this` in
      // the predecessor and collectCallArgs recovers the incoming sret.
      int32_t PrevState = -1;
      for (const CxxIPState &State : Exception->Cxx->IPMap) {
        if (State.State != PrevState)
          AddBoundary(State.IP);
        PrevState = State.State;
      }
      for (const CxxUnwindAction &Action : Exception->Cxx->UnwindMap)
        AddExceptionalRoot(Action.ActionVA);
      for (const CxxTryBlock &Try : Exception->Cxx->TryBlocks)
        for (const CxxCatchHandler &Catch : Try.Handlers) {
          AddExceptionalRoot(Catch.HandlerVA);
          for (va_t Continuation : Catch.ContinuationVAs)
            AddContinuationRoot(Continuation);
        }
    }
    if (Exception->Itanium)
      for (const ItaniumCallSite &Site : Exception->Itanium->CallSites) {
        // An SJLJ table's "ranges" are call-site indices, not addresses, so
        // nothing in it may be read as one.
        if (!Exception->Itanium->IsCallSiteAddressForm)
          break;
        AddBoundary(Site.GuardedRange.Begin);
        if (Site.GuardedRange.End != Exception->CodeRange.End)
          AddBoundary(Site.GuardedRange.End);
        AddExceptionalRoot(Site.LandingPadVA);
      }
    if (Exception->Rust)
      for (const RustLandingPad &Pad : Exception->Rust->LandingPads)
        AddExceptionalRoot(Pad.PadVA);
    if (Exception->Registration) {
      const RegistrationChainInfo &Chain = *Exception->Registration;
      const auto InstallSize = coff_loader::getX86RegistrationChainStoreSize(
          Img, Chain.ChainInstallVA);
      if (Chain.RegistrationOffset && Chain.ChainInstallVA != 0 &&
          InstallSize && Chain.ChainInstallVA <= InvalidVA - *InstallSize)
        AddBoundary(Chain.ChainInstallVA + *InstallSize);
      if (const auto Prologue =
              coff_loader::getCheckedX86CxxPrologueCall(Img, *Exception))
        AddBoundary(Prologue->Caller.End);
      for (const RegistrationScopeRecord &Scope :
           Exception->Registration->Scopes) {
        AddExceptionalRoot(Scope.FilterVA);
        AddExceptionalRoot(Scope.HandlerVA);
      }
      // A store of the current try level ends the region the previous level
      // guarded and begins the next one.  Cutting the block after it keeps one
      // level current throughout every block, which is what lets a block be
      // given its scope's edges without splitting hairs over where in the block
      // the change took effect.
      for (const RegistrationTryLevelStore &Store :
           Exception->Registration->TryLevelStores)
        AddBoundary(Store.EndVA);
    }
    if (Exception->Delphi) {
      AddExceptionalRoot(Exception->Delphi->FinallyBodyVA);
      AddExceptionalRoot(Exception->Delphi->ExceptBodyVA);
      for (const DelphiOnExceptionEntry &Arm : Exception->Delphi->OnExceptions)
        AddExceptionalRoot(Arm.HandlerVA);
    }
    if (Exception->DelphiScopes)
      for (const DelphiScopeRecord &Scope : Exception->DelphiScopes->Scopes) {
        AddBoundary(Scope.GuardedRange.Begin);
        if (Scope.GuardedRange.End != Exception->CodeRange.End)
          AddBoundary(Scope.GuardedRange.End);
        AddExceptionalRoot(Scope.TargetVA);
        for (const DelphiOnExceptionEntry &Arm : Scope.OnExceptions)
          AddExceptionalRoot(Arm.HandlerVA);
      }
    if (Exception->Go && Exception->Go->DeferReturnOffset) {
      // The runtime resumes a panicking frame at this offset to run what the
      // frame deferred, so it is entered without any branch reaching it.
      AddExceptionalRoot(Exception->CodeRange.Begin +
                         *Exception->Go->DeferReturnOffset);
    }
  }
  CurrentExceptionalEntries.insert(ExceptionalRoots.begin(),
                                   ExceptionalRoots.end());
  establishCurrentFuncRange(Img, Exception);
  explore(Img, Dec, EntryAddr);
  // A relocation can take the address of a basic block that no ordinary edge
  // reaches (GNU computed-goto labels are the canonical case).  Decode those
  // roots only after the normal entry walk, so an invalid target cannot split a
  // real instruction that the entry traversal already established.
  exploreAddressTakenRoots(Img, Dec);
  // FH4 handler maps name ordinary resume points explicitly.  They are
  // disconnected control roots in the declaring runtime-function body, but
  // they are not exceptional entries and therefore stay separate from the
  // landing-pad inventory below.
  std::sort(ContinuationRoots.begin(), ContinuationRoots.end());
  ContinuationRoots.erase(
      std::unique(ContinuationRoots.begin(), ContinuationRoots.end()),
      ContinuationRoots.end());
  for (va_t Root : ContinuationRoots) {
    if (!AuthoritativeCurrentFuncRange ||
        Root <= AuthoritativeCurrentFuncRange->first ||
        Root >= AuthoritativeCurrentFuncRange->second ||
        !isOwnedInteriorTarget(Img, Root))
      continue;
    BlockStarts.insert(Root);
    PersistentCFGRoots.insert(Root);
    OrdinaryCFGRoots.insert(Root);
    DurableCFGRoots.insert(Root);
    if (!ExploredAddrs.count(Root))
      explore(Img, Dec, Root);
  }
  // A handler or landing pad inside the owning range is a legal CFG root even
  // though no ordinary branch reaches it — only the unwinder or the dispatcher
  // enters one, so recursive descent alone would leave its body undecoded.
  // Decode them explicitly; exceptional edges remain separate and therefore do
  // not perturb dominator or ordinary structuring semantics.
  std::sort(ExceptionalRoots.begin(), ExceptionalRoots.end());
  ExceptionalRoots.erase(
      std::unique(ExceptionalRoots.begin(), ExceptionalRoots.end()),
      ExceptionalRoots.end());
  for (va_t Root : ExceptionalRoots)
    if (!ExploredAddrs.count(Root))
      explore(Img, Dec, Root);
  // An exceptional or continuation root can itself materialize an exact
  // address-taken continuation (for example, a PC-relative epilogue address
  // passed to a local-unwind helper).  The first address-root walk ran before
  // those disconnected roots were decoded, so close the local discovery fixed
  // point once more.  exploreAddressTakenRoots loops over any further roots it
  // discovers and still applies the authoritative owner/instruction-boundary
  // checks to every candidate.
  exploreAddressTakenRoots(Img, Dec);
  completeExactAArch64PageBases(Img);
  splitBlocks();

  LowFunc Func;
  Func.Entry = EntryAddr;
  if (CollectCallRegisterBody)
    CollectCallRegisterBody->Exception = Exception;
  if (Exception)
    Func.ExceptionMetadata = *Exception;
  Func.Name = FuncName.empty()
                  ? (kAutoFuncPrefix + llvm::utohexstr(EntryAddr)).str()
                  : FuncName;
  // The callee-cleanup pop (x86 `ret imm`) seen while lifting this function's
  // instructions just above; a caller adds it to its post-call stack pointer.
  Func.CalleePopBytes = Dec.getX86RetPopBytes();

  rebuildBlocks(Func);

  size_t ScalarBranchWork = limits::kMaxScalarBranchEvidenceWork;
  ScalarImageFacts ImageScalars;
  recoverScalarConstantBranches(Img, Dec, Func, ScalarBranchWork, ImageScalars);
  multiStageResolve(Img, Dec, Func);
  // Jump-table exploration may add an incoming edge between a constant's
  // definition and its use. Revalidate against that completed graph, and let
  // the ordinary resolver reconsider any transfers whose proof was withdrawn.
  while (recoverScalarConstantBranches(Img, Dec, Func, ScalarBranchWork,
                                       ImageScalars))
    multiStageResolve(Img, Dec, Func);

  // PAGEOFF output provenance is consumed only after lifting, so authenticate
  // it on the final resolver CFG rather than during linear instruction
  // discovery.  In particular, an ADD that is also an address-taken/exception
  // root has an independent live-in and must not borrow a preceding ADRP.
  ActiveJumpTableProofRoots.reset();
  JumpTableProofContextComplete = true;
  completeExactAArch64PageBases(Img);
  completeExactARMRelativeLiteralAddresses(Img);
  completeExactI386GOTBaseModels(Img);
  JumpTableProofContextComplete = false;

  // Scalar proofs have reached their fixed point. An exact independent
  // target gets the original transfer's call site, as a decoded direct tail
  // branch does, instead of a synthetic block at the callee's entry. Earlier
  // CFG changes can withdraw a proof only while it remains a branch.
  bool ScalarTailCalls = false;
  for (auto &[Addr, Rec] : Insns)
    if (Rec.ScalarBranchInput && Rec.IsBranch && !Rec.IsIndirect &&
        isTailCallTarget(Rec.BranchTarget) &&
        !isBackwardSharedEpilogue(Img, CurrentFuncEntry, Addr, Rec.BranchTarget,
                                  KnownFuncEntries)) {
      rewriteAsTailCall(Rec);
      Rec.ScalarBranchInput.reset();
      ScalarTailCalls = true;
    }
  if (ScalarTailCalls)
    rebuildBlocks(Func);
  convertIndirectTailCalls(Func);
  closeRegistrationCxxContinuations(Img, Dec, Func);

  std::set<va_t> ReachableInsnAddrs;
  for (const LowBlock &Block : Func.Blocks)
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
      ReachableInsnAddrs.insert(Boundary.Address);

  // A relocation-free code reference discovered only while decoding a
  // provisional table case is not function-global evidence.  Once fixed-point
  // recovery removes that case, drop both the reference and its conditional
  // root instead of leaking an unreachable LEA into later functions.
  for (auto It = DiscoveredCodeRefs.begin(); It != DiscoveredCodeRefs.end();) {
    auto Sources = DiscoveredCodeRefSources.find(*It);
    const bool HasReachableSource =
        Sources != DiscoveredCodeRefSources.end() &&
        std::any_of(
            Sources->second.begin(), Sources->second.end(),
            [&](va_t Source) { return ReachableInsnAddrs.count(Source) != 0; });
    if (!HasReachableSource) {
      auto Erase = It++;
      DiscoveredCodeRefs.erase(Erase);
    } else {
      ++It;
    }
  }
  auto exactProofOp = [&](va_t Addr, int Seq) -> const LowOp * {
    const auto Insn = Insns.find(Addr);
    if (Insn == Insns.end() || Insn->second.IsInstructionGuard)
      return nullptr;
    const LowOp *Found = nullptr;
    for (const LowOp &Op : Insn->second.Ops) {
      if (Op.Addr != Addr || Op.Seq != Seq)
        continue;
      if (Found)
        return nullptr;
      Found = &Op;
    }
    return Found;
  };
  auto proofRegistersOverlap = [](const NdVar &Left, const NdVar &Right) {
    if (!Left.isReg() || !Right.isReg() || Left.Size == 0 || Right.Size == 0)
      return false;
    const uint64_t LeftEnd = Left.Offset + Left.Size;
    const uint64_t RightEnd = Right.Offset + Right.Size;
    return Left.Offset < RightEnd && Right.Offset < LeftEnd;
  };
  auto proofValueClobbers = [&](const NdVar &Output, const NdVar &Value) {
    return Output.Size != 0 &&
           (Output == Value || proofRegistersOverlap(Output, Value));
  };
  auto relocationFreeProofStillMatches = [&](const RelocatedInstructionAddressOccurrence
                                                 &Occurrence) {
    if (Occurrence.Authority ==
        RelocatedInstructionAddressProofKind::X86PCRelativeCodeAddress) {
      const uint16_t PointerSize = Img.getPointerSize();
      const auto Rec = Insns.find(Occurrence.InstructionAddr);
      const LowOp *Output =
          exactProofOp(Occurrence.InstructionAddr, Occurrence.OpSeq);
      const auto Sources = DiscoveredCodeRefSources.find(Occurrence.TargetVA);
      if ((Img.Arch != Arch::X86 && Img.Arch != Arch::X64) ||
          PointerSize == 0 || Occurrence.FieldVA != InvalidVA ||
          Occurrence.Width != PointerSize || !Occurrence.DefinesOutput ||
          Occurrence.OutputMayDepend ||
          Occurrence.Provenance != ConstantAddressProvenance::CodeAddress ||
          !Occurrence.PCRelativeFromInstructionEnd ||
          Occurrence.TargetVA == InvalidVA ||
          Occurrence.TargetOwnerVA == InvalidVA ||
          !Img.hasExecutableCodeOwnerAt(Occurrence.TargetVA,
                                        ExecutableCodeOwners) ||
          !Img.relocatedTargetBelongsToOwner(Occurrence.TargetVA,
                                             Occurrence.TargetOwnerVA,
                                             ExecutableCodeOwners) ||
          Rec == Insns.end() || Rec->second.IsInstructionGuard ||
          Rec->second.IsBranch || Rec->second.IsCall || Rec->second.IsRet ||
          Rec->second.IsOpaqueTerminator || Rec->second.IsResumableTerminator ||
          !Output || Output->Opcode != NdOp::COPY ||
          Output->Opcode != Occurrence.OutputOpcode ||
          Output->Output != Occurrence.OutputWitness ||
          !Output->Output.isReg() || Output->Output.Size != PointerSize ||
          Output->NumInputs != 1 || !Output->Inputs[0].isTemp() ||
          Output->Inputs[0].Size != PointerSize ||
          Occurrence.SeedInstructionAddr != Occurrence.InstructionAddr ||
          Occurrence.SeedOpSeq != Occurrence.OpSeq ||
          Occurrence.SeedOpcode != Occurrence.OutputOpcode ||
          Occurrence.SeedInputWitness != Output->Inputs[0] ||
          Occurrence.SeedOutputWitness != Occurrence.OutputWitness ||
          Sources == DiscoveredCodeRefSources.end() ||
          Sources->second.count(Occurrence.InstructionAddr) == 0)
        return false;

      unsigned MatchingOutputs = 0;
      for (const LowOp &Op : Rec->second.Ops)
        MatchingOutputs += Op.Addr == Occurrence.InstructionAddr &&
                           Op.Opcode == NdOp::COPY && Op.Output.isReg() &&
                           Op.Output.Size == PointerSize && Op.NumInputs == 1 &&
                           Op.Inputs[0].isTemp() &&
                           Op.Inputs[0].Size == PointerSize;
      return MatchingOutputs == 1;
    }
    if (Occurrence.Authority != RelocatedInstructionAddressProofKind::
                                    AArch64RelocationFreeDataDereference)
      return true;
    const uint16_t PointerSize = Img.getPointerSize();
    if (Img.Arch != Arch::AArch64 || Img.IsRelocatable || PointerSize == 0 ||
        PointerSize > sizeof(va_t) || Occurrence.FieldVA != InvalidVA ||
        Occurrence.Width != PointerSize || !Occurrence.DefinesOutput ||
        Occurrence.OutputMayDepend ||
        Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
        Occurrence.TargetVA == InvalidVA ||
        Occurrence.TargetOwnerVA == InvalidVA ||
        Occurrence.ArithmeticProof.empty() ||
        Occurrence.ArithmeticProof.size() >
            ::neverd::limits::ir::
                kCFGBuilderBuildOnceRelocationFreeProofStillMatchesOccurrenceArithmeticProofSizeLimit)
      return false;
    const auto RootIt = Insns.find(Occurrence.SeedInstructionAddr);
    const LowOp *Root =
        exactProofOp(Occurrence.SeedInstructionAddr, Occurrence.SeedOpSeq);
    if (RootIt == Insns.end() || !Root || RootIt->second.Ops.size() != 1 ||
        RootIt->second.Size == 0 ||
        RootIt->second.Size > InvalidVA - RootIt->second.Addr ||
        RootIt->second.IsBranch || RootIt->second.IsCall ||
        RootIt->second.IsRet || RootIt->second.IsOpaqueTerminator ||
        RootIt->second.IsResumableTerminator ||
        Root->Opcode != Occurrence.SeedOpcode || Root->Opcode != NdOp::COPY ||
        Root->NumInputs != 1 ||
        Root->Inputs[0] != Occurrence.SeedInputWitness ||
        Root->Output != Occurrence.SeedOutputWitness || !Root->Output.isReg() ||
        Root->Output.Size != PointerSize || !Root->Inputs[0].isConst() ||
        Root->Inputs[0].Size != PointerSize ||
        Root->Inputs[0].Provenance !=
            ConstantAddressProvenance::AddressFragment)
      return false;

    const unsigned PointerBits = static_cast<unsigned>(PointerSize) * 8;
    const uint64_t PointerMask = PointerBits == 64
                                     ? std::numeric_limits<uint64_t>::max()
                                     : (uint64_t{1} << PointerBits) - 1;
    auto canonicalScalar =
        [&](const LowOp &Op,
            const RelocatedInstructionAddressArithmeticStep &Step)
        -> std::optional<uint64_t> {
      if (!Step.ScalarInputWitness.isConst() ||
          Step.ScalarInputWitness.Provenance !=
              ConstantAddressProvenance::Scalar)
        return std::nullopt;
      if (Step.ScalarInputWitness.Size == PointerSize)
        return Step.ScalarInputWitness.Offset & PointerMask;
      if (PointerSize != 8 || Step.ScalarInputWitness.Size != 4 ||
          Step.BaseInputIndex != 0)
        return std::nullopt;
      const uint8_t *Bytes = Img.readVA(Op.Addr, sizeof(uint32_t));
      if (!Bytes)
        return std::nullopt;
      const uint32_t Word = readLE<uint32_t>(Bytes);
      if ((Word & 0x1f000000u) != 0x11000000u || (Word & 0x80000000u) == 0 ||
          (Word & 0x20000000u) != 0 ||
          (((Word & 0x40000000u) != 0) != (Op.Opcode == NdOp::INT_SUB)))
        return std::nullopt;
      const uint64_t Encoded = uint64_t((Word >> 10) & 0xfffu)
                               << (((Word >> 22) & 1u) ? 12 : 0);
      return Encoded == Step.ScalarInputWitness.Offset
                 ? std::optional<uint64_t>(Encoded)
                 : std::nullopt;
    };
    NdVar Current = Root->Output;
    va_t Address = Root->Inputs[0].Offset;
    va_t ExpectedAddress = RootIt->second.Addr + RootIt->second.Size;
    for (const RelocatedInstructionAddressArithmeticStep &Step :
         Occurrence.ArithmeticProof) {
      const auto Rec = Insns.find(Step.InstructionAddr);
      const LowOp *Op = exactProofOp(Step.InstructionAddr, Step.OpSeq);
      if (Rec == Insns.end() || Rec->second.Addr != ExpectedAddress || !Op ||
          Rec->second.Size == 0 ||
          Rec->second.Size > InvalidVA - Rec->second.Addr ||
          Rec->second.IsBranch || Rec->second.IsCall || Rec->second.IsRet ||
          Rec->second.IsOpaqueTerminator || Rec->second.IsResumableTerminator ||
          (Step.Opcode != NdOp::INT_ADD && Step.Opcode != NdOp::INT_SUB) ||
          Op->Opcode != Step.Opcode || Op->NumInputs != 2 ||
          Step.BaseInputIndex > 1 ||
          (Op->Opcode == NdOp::INT_SUB && Step.BaseInputIndex != 0) ||
          Op->Inputs[Step.BaseInputIndex] != Step.BaseInputWitness ||
          Op->Inputs[1 - Step.BaseInputIndex] != Step.ScalarInputWitness ||
          Op->Output != Step.OutputWitness ||
          (!Op->Output.isReg() && !Op->Output.isTemp()) ||
          Op->Output.Size != PointerSize ||
          Step.BaseInputWitness.Size != PointerSize ||
          Img.InstructionAddressMaterializations.count(Op->Addr))
        return false;

      // Some AArch64 memory encodings lower their unsigned offset into the
      // same instruction record as an instruction-local COPY/ADD/LOAD
      // chain. Reconstruct only exact, full-width COPY aliases from the
      // previous address value to this arithmetic input. P-code may retain
      // both the architectural register and a bookkeeping temporary in one
      // record; a COPY to the temporary does not invalidate the still-live
      // register.
      std::vector<NdVar> BaseAliases{Current};
      bool SawArithmetic = false;
      for (const LowOp &Other : Rec->second.Ops) {
        if (&Other == Op) {
          if (std::find(BaseAliases.begin(), BaseAliases.end(),
                        Step.BaseInputWitness) == BaseAliases.end())
            return false;
          SawArithmetic = true;
          continue;
        }
        const LowMemoryOperandView Memory = lowMemoryOperands(Other);
        if (!SawArithmetic) {
          if (Memory.Complete)
            return false;
          const bool CopiesAlias =
              Other.Opcode == NdOp::COPY && Other.NumInputs == 1 &&
              (Other.Output.isReg() || Other.Output.isTemp()) &&
              Other.Output.Size == PointerSize &&
              std::find(BaseAliases.begin(), BaseAliases.end(),
                        Other.Inputs[0]) != BaseAliases.end();
          if (CopiesAlias) {
            if (std::find(BaseAliases.begin(), BaseAliases.end(),
                          Other.Output) == BaseAliases.end())
              BaseAliases.push_back(Other.Output);
            continue;
          }
          if (std::any_of(BaseAliases.begin(), BaseAliases.end(),
                          [&](const NdVar &Alias) {
                            return proofValueClobbers(Other.Output, Alias);
                          }) ||
              proofValueClobbers(Other.Output, Op->Output))
            return false;
          continue;
        }

        const bool SameRecordFinalDereference =
            &Step == &Occurrence.ArithmeticProof.back() &&
            Occurrence.DereferenceInstructionAddr == Step.InstructionAddr;
        if (!SameRecordFinalDereference &&
            (Memory.Complete || proofValueClobbers(Other.Output, Current) ||
             proofValueClobbers(Other.Output, Op->Output)))
          return false;
      }
      if (!SawArithmetic)
        return false;
      const std::optional<uint64_t> Delta = canonicalScalar(*Op, Step);
      if (!Delta || Address > PointerMask ||
          (Op->Opcode == NdOp::INT_ADD && *Delta > PointerMask - Address) ||
          (Op->Opcode == NdOp::INT_SUB && *Delta > Address))
        return false;
      Address =
          Op->Opcode == NdOp::INT_ADD ? Address + *Delta : Address - *Delta;
      Current = Op->Output;
      ExpectedAddress =
          (&Step == &Occurrence.ArithmeticProof.back() &&
           Occurrence.DereferenceInstructionAddr == Step.InstructionAddr)
              ? Rec->second.Addr
              : Rec->second.Addr + Rec->second.Size;
    }
    const RelocatedInstructionAddressArithmeticStep &Final =
        Occurrence.ArithmeticProof.back();
    if (Occurrence.InstructionAddr != Final.InstructionAddr ||
        Occurrence.OpSeq != Final.OpSeq ||
        Occurrence.OutputOpcode != Final.Opcode ||
        Occurrence.OutputWitness != Final.OutputWitness ||
        Occurrence.TargetVA != Address)
      return false;

    const auto DereferenceRec =
        Insns.find(Occurrence.DereferenceInstructionAddr);
    const LowOp *Dereference = exactProofOp(
        Occurrence.DereferenceInstructionAddr, Occurrence.DereferenceOpSeq);
    if (Dereference && Dereference->Opcode == NdOp::CALL)
      return importedMemoryCopyDereferenceMatches(Img, Insns, *Dereference,
                                                  Occurrence, Current, Address,
                                                  ExpectedAddress);
    if (DereferenceRec == Insns.end() || !Dereference ||
        DereferenceRec->second.Addr != ExpectedAddress ||
        DereferenceRec->second.IsBranch || DereferenceRec->second.IsCall ||
        DereferenceRec->second.IsRet ||
        DereferenceRec->second.IsOpaqueTerminator ||
        DereferenceRec->second.IsResumableTerminator ||
        Dereference->Opcode != Occurrence.DereferenceOpcode ||
        (Dereference->Opcode != NdOp::LOAD &&
         Dereference->Opcode != NdOp::STORE &&
         !lowAArch64AddressMemoryShapeIsValid(*Dereference) &&
         !lowAArch64WritebackMemoryShapeIsValid(*Dereference)))
      return false;
    NdVar DereferenceAddress = Current;
    bool SawDereference = false;
    LowImageBaseMemoryWitness Memory;
    bool SawFinalArithmetic =
        DereferenceRec->second.Addr != Final.InstructionAddr;
    for (const LowOp &Op : DereferenceRec->second.Ops) {
      if (!SawFinalArithmetic) {
        if (Op.Addr == Final.InstructionAddr && Op.Seq == Final.OpSeq) {
          SawFinalArithmetic = true;
          continue;
        }
        continue;
      }
      if (&Op == Dereference) {
        Memory = lowImageBaseMemoryWitness(Img, Op, Address,
                                           Occurrence.TargetOwnerVA);
        if (!Memory.Complete || !Memory.Base ||
            *Memory.Base != DereferenceAddress || SawDereference ||
            proofValueClobbers(Op.Output, Current))
          return false;
        SawDereference = true;
        continue;
      }
      if (SawDereference && lowImageBaseMemoryWritebackCommit(
                                Op, Dereference, DereferenceRec->second.Ops))
        continue;
      if (lowMemoryOperands(Op).Complete ||
          lowAArch64WritebackMemoryShapeIsValid(Op) ||
          lowAArch64AddressMemoryShapeIsValid(Op) ||
          proofValueClobbers(Op.Output, Current))
        return false;
      if (!SawDereference) {
        if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 ||
            Op.Inputs[0] != DereferenceAddress ||
            (!Op.Output.isReg() && !Op.Output.isTemp()) ||
            Op.Output.Size != PointerSize ||
            (Op.Output.isReg() && proofRegistersOverlap(Op.Output, Current)))
          return false;
        DereferenceAddress = Op.Output;
      }
    }
    if (!SawFinalArithmetic || !SawDereference || !Memory.Complete ||
        !Memory.Base ||
        DereferenceAddress != Occurrence.DereferenceAddressWitness ||
        *Memory.Base != DereferenceAddress ||
        Memory.AccessSize != Occurrence.DereferenceAccessSize ||
        Memory.AccessSize == 0 || Memory.AccessSize - 1 > InvalidVA - Address)
      return false;
    const va_t Last = Address + Memory.AccessSize - 1;
    if (!Img.hasObjectDataProvenance(Address) ||
        !Img.hasObjectDataProvenance(Last) ||
        Img.hasExecutableCodeOwnerAt(Address, ExecutableCodeOwners) ||
        Img.hasExecutableCodeOwnerAt(Last, ExecutableCodeOwners) ||
        isRuntimeWritableAddress(Img, Address) ||
        isRuntimeWritableAddress(Img, Last) ||
        !Img.relocatedTargetBelongsToOwner(Address, Occurrence.TargetOwnerVA,
                                           ExecutableCodeOwners))
      return false;
    const Section *StartSection = Img.getSectionFor(Address);
    const Section *LastSection = Img.getSectionFor(Last);
    if (StartSection || LastSection)
      return StartSection && StartSection == LastSection &&
             StartSection->VA == Occurrence.TargetOwnerVA;
    const Segment *StartSegment = Img.getSegmentFor(Address);
    const Segment *LastSegment = Img.getSegmentFor(Last);
    return StartSegment && StartSegment == LastSegment &&
           StartSegment->VA == Occurrence.TargetOwnerVA;
  };

  Func.RelocatedInstructionAddressOccurrences.clear();
  for (const RelocatedInstructionAddressOccurrence &Occurrence :
       RelocatedInstructionAddressOccurrences) {
    bool Published = false;
    for (const LowBlock &Block : Func.Blocks) {
      for (const LowInstructionBoundary &Boundary :
           Block.InstructionBoundaries) {
        if (Boundary.Address != Occurrence.InstructionAddr ||
            (!Occurrence.DefinesOutput &&
             (Boundary.Size > InvalidVA - Boundary.Address ||
              Occurrence.FieldVA < Boundary.Address ||
              Occurrence.FieldVA >= Boundary.Address + Boundary.Size)))
          continue;
        const uint64_t End = Boundary.FirstOp + Boundary.OpCount;
        if (End > Block.Ops.size())
          continue;
        for (uint64_t I = Boundary.FirstOp; I < End; ++I) {
          const LowOp &Op = Block.Ops[I];
          if (Op.Addr != Occurrence.InstructionAddr ||
              Op.Seq != Occurrence.OpSeq)
            continue;
          auto Matches = [&](const NdVar &Value) {
            return Value.isConst() &&
                   isExactAddressProvenance(Value.Provenance) &&
                   Value.Provenance == Occurrence.Provenance &&
                   Value.Offset == Occurrence.TargetVA &&
                   (Occurrence.TargetOwnerVA == InvalidVA ||
                    Value.AddressOwnerVA == Occurrence.TargetOwnerVA);
          };
          if (Occurrence.DefinesOutput) {
            Published = Op.Opcode == Occurrence.OutputOpcode &&
                        Op.Output == Occurrence.OutputWitness &&
                        (Op.Output.isReg() || Op.Output.isTemp()) &&
                        Op.Output.Size != 0;
          } else {
            Published = Matches(Op.Output);
            for (uint8_t InputIndex = 0;
                 !Published && InputIndex < Op.NumInputs; ++InputIndex)
              Published = Matches(Op.Inputs[InputIndex]);
          }
          if (Published)
            break;
        }
        if (Published)
          break;
      }
      if (Published)
        break;
    }
    if (Published && relocationFreeProofStillMatches(Occurrence)) {
      Func.RelocatedInstructionAddressOccurrences.push_back(Occurrence);
      if (!Occurrence.OutputMayDepend &&
          Occurrence.Provenance == ConstantAddressProvenance::CodeAddress)
        DiscoveredCodeRefs.insert(
            normalizeCodeAddress(Occurrence.TargetVA, Img.Arch, Img.Mode));
    }
  }

  Func.I386GetPcOccurrences.clear();
  for (const I386GetPcOccurrence &Occurrence : I386GetPcOccurrences) {
    bool Published = false;
    for (const LowBlock &Block : Func.Blocks) {
      for (const LowInstructionBoundary &Boundary :
           Block.InstructionBoundaries) {
        if (Boundary.Address != Occurrence.InstructionAddr)
          continue;
        const uint64_t End = Boundary.FirstOp + Boundary.OpCount;
        if (End > Block.Ops.size())
          continue;
        for (uint64_t I = Boundary.FirstOp; I < End; ++I) {
          const LowOp &Op = Block.Ops[I];
          if (Op.Addr == Occurrence.InstructionAddr &&
              Op.Seq == Occurrence.OpSeq &&
              Op.Opcode == Occurrence.OutputOpcode &&
              Op.Output == Occurrence.OutputWitness && Op.NumInputs == 1 &&
              Op.Inputs[0] == Occurrence.InputWitness) {
            Published = true;
            break;
          }
        }
        if (Published)
          break;
      }
      if (Published)
        break;
    }
    if (Published)
      Func.I386GetPcOccurrences.push_back(Occurrence);
  }

  Func.RelocatedInstructionScalarModelOccurrences.clear();
  for (const RelocatedInstructionScalarModelOccurrence &Occurrence :
       RelocatedInstructionScalarModelOccurrences) {
    bool Published = false;
    for (const LowBlock &Block : Func.Blocks) {
      for (const LowInstructionBoundary &Boundary :
           Block.InstructionBoundaries) {
        if (Boundary.Address != Occurrence.InstructionAddr ||
            Boundary.Size > InvalidVA - Boundary.Address ||
            Occurrence.FieldVA < Boundary.Address ||
            Occurrence.FieldVA >= Boundary.Address + Boundary.Size)
          continue;
        const uint64_t End = Boundary.FirstOp + Boundary.OpCount;
        if (End > Block.Ops.size())
          continue;
        for (uint64_t I = Boundary.FirstOp; I < End; ++I) {
          const LowOp &Op = Block.Ops[I];
          if (Op.Addr == Occurrence.InstructionAddr &&
              Op.Seq == Occurrence.OpSeq &&
              Op.Opcode == Occurrence.OutputOpcode &&
              Op.Output == Occurrence.OutputWitness &&
              (Op.Output.isReg() || Op.Output.isTemp()) &&
              Op.Output.Size == Occurrence.Width) {
            Published = true;
            break;
          }
        }
        if (Published)
          break;
      }
      if (Published)
        break;
    }
    if (Published)
      Func.RelocatedInstructionScalarModelOccurrences.push_back(Occurrence);
  }

  // A relocation-free RIP-relative LEA is initially only an address-of
  // candidate.  Once jump-table recovery proves that the same address owns
  // inline table storage (which Mach-O commonly places in __text), the data
  // owner wins: publishing it as a global CodeRefTarget would make every
  // numerically equal occurrence look like a function/label identity.
  // Apply the same arbitration to exact code-address relocation occurrences
  // before publishing the function-local inventory.
  for (auto It = DiscoveredCodeRefs.begin(); It != DiscoveredCodeRefs.end();) {
    bool IsJumpTableStorage = false;
    for (const JumpTable &JT : Func.JumpTables)
      if (JT.ownsStorageAddress(*It)) {
        IsJumpTableStorage = true;
        break;
      }
    if (IsJumpTableStorage) {
      auto Erase = It++;
      DiscoveredCodeRefs.erase(Erase);
    } else {
      ++It;
    }
  }
  Func.RelocatedInstructionAddressOccurrences.erase(
      std::remove_if(
          Func.RelocatedInstructionAddressOccurrences.begin(),
          Func.RelocatedInstructionAddressOccurrences.end(),
          [&](const RelocatedInstructionAddressOccurrence &Occurrence) {
            return Occurrence.Authority ==
                       RelocatedInstructionAddressProofKind::
                           X86PCRelativeCodeAddress &&
                   DiscoveredCodeRefs.count(Occurrence.TargetVA) == 0;
          }),
      Func.RelocatedInstructionAddressOccurrences.end());
  Func.CodeRefTargets.assign(DiscoveredCodeRefs.begin(),
                             DiscoveredCodeRefs.end());
  // Recursive descent intentionally retains decoded provisional jump-table
  // cases across fixed-point rounds.  Coverage is a property of the final
  // public function, however, so derive its inventory from the final roots and
  // edges instead of publishing the exploration cache wholesale.  Terminal
  // decode/lift failures have no LowInstructionBoundary of their own; retain
  // them when a final edge (or an unsuppressed durable/address-taken root)
  // attempted that address.
  std::set<va_t> FinalAttemptedAddresses = PublishedReachableInsns;
  auto AddAttempted = [&](va_t Address) {
    if (Address != InvalidVA && ExploredAddrs.count(Address))
      FinalAttemptedAddresses.insert(Address);
  };
  auto FinalTableSuppressesRelocationRoot = [&](va_t Root) {
    if (DurableCFGRoots.count(Root))
      return false;
    const auto Sources = RelocationCFGRootSources.find(Root);
    if (Sources == RelocationCFGRootSources.end() || Sources->second.empty())
      return false;
    return std::all_of(
        Sources->second.begin(), Sources->second.end(), [&](va_t Slot) {
          for (const auto &[BranchAddr, Info] : ResolvedTableInfo) {
            const auto Branch = Insns.find(BranchAddr);
            if (Branch == Insns.end() ||
                Branch->second.JumpTableTargets.empty() ||
                !PublishedReachableInsns.count(BranchAddr))
              continue;
            if (std::find(Info.SuppressibleRelocationSlots.begin(),
                          Info.SuppressibleRelocationSlots.end(),
                          Slot) != Info.SuppressibleRelocationSlots.end())
              return true;
          }
          return false;
        });
  };
  for (va_t Root : PersistentCFGRoots)
    if (!FinalTableSuppressesRelocationRoot(Root))
      AddAttempted(Root);
  for (va_t Root : DiscoveredCodeRefs)
    AddAttempted(Root);

  for (va_t Address : PublishedReachableInsns) {
    const auto It = Insns.find(Address);
    if (It == Insns.end())
      continue;
    const InsnRecord &Rec = It->second;
    if (Rec.IsRet) {
      if (Rec.IsCond && Rec.IsBranch)
        AddAttempted(Rec.BranchTarget);
      continue;
    }
    if (Rec.IsBranch && !Rec.IsCall) {
      if (Rec.IsIndirect)
        for (va_t Target : Rec.JumpTableTargets)
          AddAttempted(Target);
      else
        AddAttempted(Rec.BranchTarget);
      if (Rec.IsCond && Rec.Size <= InvalidVA - Rec.Addr)
        AddAttempted(Rec.Addr + Rec.Size);
      continue;
    }
    if (Rec.IsNoReturnCall && !Rec.IsCond)
      continue;
    if (Rec.Size <= InvalidVA - Rec.Addr)
      AddAttempted(Rec.Addr + Rec.Size);
  }

  // Pending leaves belong to the final graph: an intermediate unresolved
  // selector must not poison a later complete proof, and added edges can
  // revoke an earlier specialization. Raw decoded instruction records stay
  // pending across every rebuild.
  for (const auto &Block : Func.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst() &&
          isX86BoundedLeafPending(
              static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
        UnsupportedInstructionAddresses.insert(Op.Addr);
        LiftedInstructionAddresses.erase(Op.Addr);
      }

  Func.DecodedInstructionCount = static_cast<uint64_t>(std::count_if(
      DecodedInstructionAddresses.begin(), DecodedInstructionAddresses.end(),
      [&](va_t Address) { return FinalAttemptedAddresses.count(Address); }));
  Func.LiftedInstructionCount = static_cast<uint64_t>(std::count_if(
      LiftedInstructionAddresses.begin(), LiftedInstructionAddresses.end(),
      [&](va_t Address) { return FinalAttemptedAddresses.count(Address); }));
  auto PublishFailures = [&](const std::set<va_t> &Failures,
                             std::vector<va_t> &Published) {
    for (va_t Address : Failures)
      if (FinalAttemptedAddresses.count(Address))
        Published.push_back(Address);
  };
  PublishFailures(DecodeFailureAddresses, Func.DecodeFailureAddresses);
  PublishFailures(UnsupportedInstructionAddresses,
                  Func.UnsupportedInstructionAddresses);
  PublishFailures(TruncatedPathAddresses, Func.TruncatedPathAddresses);
  for (const auto &[Addr, Rec] : Insns)
    if (Rec.RejectedScalarBranch && ReachableInsnAddrs.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  if (UnsafeJumpTableBranches)
    for (va_t Addr : *UnsafeJumpTableBranches)
      if (Insns.count(Addr))
        Func.UnsafeIndirectBranchAddresses.insert(Addr);
  // A complete group owner remains table-shaped even when one anchor or
  // relocation slot is damaged. The joint proof must publish every member or
  // none; retain the unresolved branch identity for downstream trap lowering.
  if (finiteGOTOFFGroupClaimed() || finiteAbsoluteGroupClaimed())
    for (size_t I = 0; I < GuardedGroupIdentity->MemberCount; ++I) {
      const va_t Addr = GuardedGroupIdentity->Members[I];
      if (auto It = Insns.find(Addr);
          It != Insns.end() && It->second.JumpTableTargets.empty())
        Func.UnsafeIndirectBranchAddresses.insert(Addr);
    }
  if (PreservePotentialJumpTableBranches)
    for (va_t Addr : PotentialJumpTableBranches) {
      auto It = Insns.find(Addr);
      if (It != Insns.end() && It->second.JumpTableTargets.empty())
        Func.UnsafeIndirectBranchAddresses.insert(Addr);
    }
  Func.EverPublishedJumpTableBranchAddresses.insert(
      EverPublishedJumpTableBranches.begin(),
      EverPublishedJumpTableBranches.end());
  for (va_t Addr : LostValidatedJumpTableBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : StackTableEvidenceIncompleteBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : IndexDomainEvidenceIncompleteBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  if (IncompleteBranchMarkerEvidenceIncomplete)
    for (const auto &[Addr, Rec] : Insns)
      if (Rec.IsBranch && Rec.IsIndirect && !Rec.IsCall && !Rec.IsRet &&
          !Rec.IsCond && Rec.JumpTableTargets.empty())
        Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : ValidatedPhysicalJumpTableBranches)
    if (auto It = Insns.find(Addr);
        It != Insns.end() && It->second.JumpTableTargets.empty())
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : AmbiguousI386GOTPCBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : PendingAmbiguousI386GOTPCBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);

  LLVM_DEBUG(llvm::dbgs() << "CFG built: " << Func.Blocks.size()
                          << " blocks for " << Func.Name << " @ 0x"
                          << llvm::utohexstr(Func.Entry) << "\n");
  canonicalizePrologueStackAllocation(Img, Func);
  return Func;
}

static std::optional<size_t>
candidateFiniteProofOwnerLookupWork(const BinaryImage &Img,
                                    size_t KnownFunctionEntries) {
  size_t Work = 128;
  if (!detail::addLinearComparisonWork(Work, Img.Segments.size(), 16) ||
      !detail::addLinearComparisonWork(Work, Img.Sections.size(), 16) ||
      !detail::addLinearComparisonWork(Work, Img.Symbols.size(), 16) ||
      !detail::addLinearComparisonWork(Work, Img.KnownCodeRanges.size(), 8) ||
      !detail::addLinearComparisonWork(Work, Img.ImportStubRanges.size(), 8) ||
      !detail::addLinearComparisonWork(Work, Img.Imports.size(), 8) ||
      !detail::addLinearComparisonWork(Work, Img.Exports.size(), 8) ||
      !detail::addLinearComparisonWork(Work, KnownFunctionEntries, 4))
    return std::nullopt;
  return Work;
}

bool CFGBuilder::chargeCandidateDecodeAttempt(const BinaryImage &Img) {
  if (!CandidateFiniteProofDecodeOnly)
    return true;
  const std::optional<size_t> OwnerWork =
      candidateFiniteProofOwnerLookupWork(Img, knownFunctionEntryCount());
  size_t ScratchDecodeWork = 8192;
  if (!OwnerWork ||
      !detail::addLinearComparisonWork(ScratchDecodeWork, *OwnerWork, 64) ||
      !CandidateFiniteProofDecodeBudget ||
      CandidateFiniteProofDecodeAttempts >=
          ::neverd::limits::ir::kCFGBuilderMaxScratchDecodeAttempts ||
      *CandidateFiniteProofDecodeBudget < ScratchDecodeWork) {
    CandidateFiniteProofDecodeIncomplete = true;
    if (CandidateFiniteProofDecodeBudget)
      *CandidateFiniteProofDecodeBudget = 0;
    return false;
  }
  *CandidateFiniteProofDecodeBudget -= ScratchDecodeWork;
  ++CandidateFiniteProofDecodeAttempts;
  return true;
}

bool CFGBuilder::prepareCandidateFiniteProofScratch(
    CFGBuilder &Scratch, const std::vector<va_t> &PhysicalTargets,
    size_t *EvidenceBudget, bool *AnalysisIncomplete) const {
  if (AnalysisIncomplete)
    *AnalysisIncomplete = false;
  auto Incomplete = [&] {
    if (EvidenceBudget)
      *EvidenceBudget = 0;
    if (AnalysisIncomplete)
      *AnalysisIncomplete = true;
    return false;
  };
  if (!EvidenceBudget || !CurrentImg || !CurrentFuncRange ||
      !Scratch.Insns.empty() || Scratch.CurrentImg || PhysicalTargets.empty() ||
      PhysicalTargets.size() > limits::kMaxJumpTableEntries)
    return false;

  const BinaryImage &Img = *CurrentImg;
  const std::optional<size_t> OwnerWork =
      candidateFiniteProofOwnerLookupWork(Img, knownFunctionEntryCount());
  if (!OwnerWork)
    return Incomplete();
  const size_t OwnerLookupWork = *OwnerWork;
  size_t CopyWork = 512;
  auto Add = [&](size_t Count, size_t Cost) {
    return detail::addLinearComparisonWork(CopyWork, Count, Cost);
  };
  // Charge construction, all owned copies, and retirement before filling the
  // fresh builder.  Proposal caches, provisional/strong edges, group proof
  // contexts, and resolved-table metadata are intentionally not copied.
  if (!Add(Insns.size(), 192) || !Add(BlockStarts.size(), 24) ||
      !Add(PublishedBlockStarts.size(), 16) ||
      !Add(PublishedReachableInsns.size(), 24) ||
      !Add(PersistentCFGRoots.size(), 24) ||
      !Add(OrdinaryCFGRoots.size(), 24) || !Add(DurableCFGRoots.size(), 24) ||
      !Add(RelocationCFGRootSources.size(), 64) ||
      !Add(DiscoveredCodeRefSources.size(), 64) ||
      !Add(ExploredAddrs.getMemorySize() / sizeof(va_t), 12) ||
      !Add(RelocatedInstructionAddressOccurrences.size(), 384) ||
      !Add(RelocatedInstructionScalarOperandOccurrences.size(), 192) ||
      !Add(RelocatedInstructionScalarModelOccurrences.size(), 384) ||
      !Add(I386GetPcOccurrences.size(), 96) ||
      !Add(CurrentExceptionalEntries.size(), 24) ||
      !Add(ActiveJumpTableProofRoots ? ActiveJumpTableProofRoots->size() : 0,
           24) ||
      !Add(PhysicalTargets.size(), 32))
    return Incomplete();
  // Every physical target may run owner-at, owner-range, mapped-byte and
  // interior-target checks. Those helpers scan image inventories without an
  // index when no borrowed owner index exists, so charge their worst-case
  // repeated work before even the first scratch mutation.
  for (unsigned Check = 0; Check != 6; ++Check)
    if (!Add(PhysicalTargets.size(), OwnerLookupWork))
      return Incomplete();
  for (const auto &[Addr, Rec] : Insns) {
    (void)Addr;
    if (!Add(Rec.Ops.size(), 384) || !Add(Rec.JumpTableTargets.size(), 24))
      return Incomplete();
  }
  for (const auto &[Target, Sources] : RelocationCFGRootSources) {
    (void)Target;
    if (!Add(Sources.size(), 24))
      return Incomplete();
  }
  for (const auto &[Target, Sources] : DiscoveredCodeRefSources) {
    (void)Target;
    if (!Add(Sources.size(), 24))
      return Incomplete();
  }
  for (const auto &Occurrence : RelocatedInstructionAddressOccurrences)
    if (!Add(Occurrence.ArithmeticProof.size(), 384))
      return Incomplete();
  if (CopyWork > *EvidenceBudget)
    return Incomplete();
  *EvidenceBudget -= CopyWork;

  const uint32_t Alignment = getInsnAlignment();
  for (va_t Target : PhysicalTargets) {
    if (Target == InvalidVA || (Alignment > 1 && Target % Alignment != 0) ||
        !Img.hasExecutableCodeOwnerAt(Target, ExecutableCodeOwners) ||
        !Img.hasExecutableCodeOwnerRange(Target, Alignment) ||
        !Img.readVA(Target, 1))
      return false;
    // A physical pointer into another function, an unowned byte range, or
    // the middle of an already decoded instruction is never a proof root.
    if (Target != CurrentFuncEntry && !isCurrentOwnedFragment(Target) &&
        !isCurrentExceptionalEntry(Target) &&
        !isOwnedInteriorTarget(Img, Target))
      return false;
    auto After = Insns.upper_bound(Target);
    if (After != Insns.begin()) {
      const auto &Prev = *std::prev(After);
      if (Prev.first < Target &&
          Target - Prev.first < static_cast<va_t>(Prev.second.Size))
        return false;
    }
  }

  Scratch.Insns = Insns;
  Scratch.BlockStarts = BlockStarts;
  Scratch.PublishedBlockStarts = PublishedBlockStarts;
  Scratch.PublishedReachableInsns = PublishedReachableInsns;
  Scratch.PersistentCFGRoots = PersistentCFGRoots;
  Scratch.OrdinaryCFGRoots = OrdinaryCFGRoots;
  Scratch.DurableCFGRoots = DurableCFGRoots;
  Scratch.RelocationCFGRootSources = RelocationCFGRootSources;
  Scratch.DiscoveredCodeRefSources = DiscoveredCodeRefSources;
  Scratch.ExploredAddrs = ExploredAddrs;
  Scratch.RelocatedInstructionAddressOccurrences =
      RelocatedInstructionAddressOccurrences;
  Scratch.RelocatedInstructionScalarOperandOccurrences =
      RelocatedInstructionScalarOperandOccurrences;
  Scratch.RelocatedInstructionScalarModelOccurrences =
      RelocatedInstructionScalarModelOccurrences;
  Scratch.I386GetPcOccurrences = I386GetPcOccurrences;
  Scratch.CurrentImg = CurrentImg;
  Scratch.ExecutableCodeOwners = ExecutableCodeOwners;
  Scratch.CurrentFuncEntry = CurrentFuncEntry;
  Scratch.CurrentFuncRange = CurrentFuncRange;
  Scratch.AuthoritativeCurrentFuncRange = AuthoritativeCurrentFuncRange;
  Scratch.CurrentFuncIsFramelessLeaf = CurrentFuncIsFramelessLeaf;
  Scratch.KnownFuncEntries = KnownFuncEntries;
  Scratch.CurrentExceptionalEntries = CurrentExceptionalEntries;
  Scratch.NoReturnTargets = NoReturnTargets;
  Scratch.I386GOTModelEvidenceIncomplete = I386GOTModelEvidenceIncomplete;
  Scratch.I386GOTOFFProposalEvidenceBudgetForTesting =
      I386GOTOFFProposalEvidenceBudgetForTesting;
  Scratch.I386GOTModelEvidenceBudgetForTesting =
      I386GOTModelEvidenceBudgetForTesting;
  Scratch.FiniteSetSymbolEvidenceBudgetForTesting =
      FiniteSetSymbolEvidenceBudgetForTesting;
  Scratch.StackTableEvidenceRemaining = StackTableEvidenceRemaining;
  Scratch.CrossFunctionContinuationRoots = CrossFunctionContinuationRoots;
  Scratch.ProtectedJumpTableRelocationSlots = ProtectedJumpTableRelocationSlots;
  Scratch.UnsafeJumpTableBranches = UnsafeJumpTableBranches;
  Scratch.ActiveJumpTableProofRoots = ActiveJumpTableProofRoots;
  Scratch.ActiveJumpTableCandidateAddr = ActiveJumpTableCandidateAddr;
  Scratch.ActiveJumpTableCandidateProofRank = ActiveJumpTableCandidateProofRank;
  Scratch.ActiveJumpTableCandidateDependencyRank =
      ActiveJumpTableCandidateDependencyRank;
  Scratch.JumpTableProofContextComplete = JumpTableProofContextComplete;
  Scratch.CandidateFiniteProofDecodeOnly = true;
  Scratch.CandidateFiniteProofDecodeBudget = EvidenceBudget;

  Decoder Dec;
  Dec.setStrict(true);
  if (!Dec.init(Img)) {
    Scratch.CandidateFiniteProofDecodeBudget = nullptr;
    return Incomplete();
  }
  for (va_t Target : PhysicalTargets) {
    Scratch.BlockStarts.insert(Target);
    if (!Scratch.ExploredAddrs.count(Target))
      Scratch.explore(Img, Dec, Target);
    if (Scratch.CandidateFiniteProofDecodeIncomplete) {
      Scratch.CandidateFiniteProofDecodeBudget = nullptr;
      return Incomplete();
    }
    if (!Scratch.Insns.count(Target)) {
      Scratch.CandidateFiniteProofDecodeBudget = nullptr;
      return Incomplete();
    }
  }
  Scratch.CandidateFiniteProofDecodeBudget = nullptr;
  if (!Scratch.DecodeFailureAddresses.empty() ||
      !Scratch.UnsupportedInstructionAddresses.empty() ||
      !Scratch.TruncatedPathAddresses.empty())
    return Incomplete();

  size_t ValidationWork = 0;
  if (OwnerLookupWork > std::numeric_limits<size_t>::max() - 32 ||
      !detail::addLinearComparisonWork(ValidationWork, Scratch.Insns.size(),
                                       OwnerLookupWork + 32) ||
      ValidationWork > *EvidenceBudget)
    return Incomplete();
  *EvidenceBudget -= ValidationWork;
  va_t PreviousEnd = 0;
  bool HavePrevious = false;
  for (const auto &[Addr, Rec] : Scratch.Insns) {
    if (Rec.Size == 0 ||
        static_cast<va_t>(Rec.Size) > std::numeric_limits<va_t>::max() - Addr ||
        (HavePrevious && Addr < PreviousEnd))
      return false;
    PreviousEnd = Addr + static_cast<va_t>(Rec.Size);
    HavePrevious = true;
    if (Insns.count(Addr))
      continue;
    if (!Img.hasExecutableCodeOwnerRange(Addr, Rec.Size) ||
        (Addr != CurrentFuncEntry && !isCurrentOwnedFragment(Addr) &&
         !isCurrentExceptionalEntry(Addr) &&
         (Addr <= CurrentFuncRange->first || Addr >= CurrentFuncRange->second ||
          isKnownFunctionEntry(Addr))))
      return false;
  }
  return true;
}

void CFGBuilder::explore(const BinaryImage &Img, Decoder &Dec, va_t Addr) {
  // Resolver-stage proposal caches are valid only for one immutable decoded
  // graph.  Exploring even one new target can add an alternate predecessor,
  // persistent relocation root, or source occurrence that invalidates a
  // previously successful exact reaching-value proof.  Centralize the
  // invalidation here so target publication, relocated-interior recovery, and
  // shared-table reconciliation cannot accidentally reuse a stale result.
  I386GOTOFFProposalRootCache.clear();
  I386GOTOFFModelReachCache.clear();
  // Only an independently authenticated direct B may authorize decoding
  // across a known entry. Decoded blocks keep all their physical predecessors.
  // Keeping its source on this local worklist prevents fallthrough, BL, and
  // unrelated exploration roots from inheriting another edge's permission.
  struct PendingEdge {
    va_t Address;
    std::optional<va_t> SourceBranch;
    std::optional<InstructionMode> Mode;
  };
  std::queue<PendingEdge> Worklist;
  Worklist.push({Addr, std::nullopt, Img.instructionModeAt(Addr)});
  auto branchMode =
      [](const InsnRecord &Source) -> std::optional<InstructionMode> {
    switch (Source.TargetMode) {
    case LowInstructionTargetMode::ARM:
      return InstructionMode::ARM;
    case LowInstructionTargetMode::Thumb:
      return InstructionMode::Thumb;
    case LowInstructionTargetMode::Preserve:
      return Source.Mode;
    case LowInstructionTargetMode::FromTargetBit0:
      return std::nullopt;
    }
    return std::nullopt;
  };
  // Known entries reached by a conditional branch or by falling through.
  std::vector<std::pair<va_t, std::optional<InstructionMode>>> TailCallEntries;

  while (!Worklist.empty()) {
    auto [Cur, SourceBranch, PathMode] = Worklist.front();
    Worklist.pop();

    while (true) {
      if (ExploredAddrs.count(Cur)) {
        if (Img.Arch == Arch::ARM) {
          const auto Existing = Insns.find(Cur);
          if (Existing != Insns.end() &&
              Img.instructionModeAt(Cur, PathMode) != Existing->second.Mode)
            DecodeFailureAddresses.insert(Cur);
        }
        break;
      }
      // Fallthrough or a queued edge into another function's entry belongs to
      // that function.  Following it here fuses callees into a multi-hundred-
      // thousand-op CFG and leaves the real PDB symbol as an empty HighC stub.
      if (Cur != CurrentFuncEntry && isKnownFunctionEntry(Cur) &&
          !isCurrentExceptionalEntry(Cur) && !isCurrentOwnedFragment(Cur) &&
          (!SourceBranch ||
           !isBackwardSharedEpilogue(Img, CurrentFuncEntry, *SourceBranch, Cur,
                                     KnownFuncEntries))) {
        // Reaching it is a tail call, whether by a conditional branch or by
        // falling through (see the stubs after this walk).  A disposable
        // proof snapshot keeps its decode-only accounting.
        if (!CandidateFiniteProofDecodeOnly)
          TailCallEntries.emplace_back(Cur, PathMode);
        break;
      }
      SourceBranch.reset();
      if (!chargeCandidateDecodeAttempt(Img))
        break;
      // An actual graph extension invalidates every generation-local replay
      // and positive ambiguity shadow.  Pending exact query identities remain
      // fail-closed carry, but only a fresh query on this immutable graph may
      // retire them or commit a semantic certificate.
      StageReplayedI386GOTPCKeys.clear();
      StageAmbiguousI386GOTPCBranches.clear();
      ExploredAddrs.insert(Cur);

      const auto *Seg = Img.getSegmentFor(Cur);
      if (!Seg || !Img.hasExecutableCodeOwnerAt(Cur, ExecutableCodeOwners)) {
        TruncatedPathAddresses.insert(Cur);
        break;
      }

      // A segment's VA range (Seg->Size) can exceed its materialized bytes
      // (e.g. .bss, or bytes the loader refused to map from a crafted header),
      // so guard before subtracting or Remain underflows into a huge length.
      size_t Off = static_cast<size_t>(Cur - Seg->VA);
      if (Off >= Seg->Data.size()) {
        TruncatedPathAddresses.insert(Cur);
        break;
      }
      size_t Remain = Seg->Data.size() - Off;

      if (!Dec.selectMode(Img, Cur, PathMode)) {
        DecodeFailureAddresses.insert(Cur);
        break;
      }
      PathMode = Dec.currentMode();
      DecodedInsn DI;
      int Sz = Dec.decodeOneForLift(Seg->Data.data() + Off, Remain, Cur, DI);
      if (Sz <= 0) {
        DecodeFailureAddresses.insert(Cur);
        break;
      }
      if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz))) {
        TruncatedPathAddresses.insert(Cur);
        break;
      }
      if (!instructionFitsCurrentOwnership(Cur, static_cast<uint64_t>(Sz))) {
        DecodeFailureAddresses.insert(Cur);
        break;
      }
      if (Img.Arch == Arch::ARM &&
          Img.instructionModeAt(Cur + Sz - 1, Dec.currentMode()) !=
              Dec.currentMode()) {
        DecodeFailureAddresses.insert(Cur);
        break;
      }
      ++DecodedInstructionCount;
      DecodedInstructionAddresses.insert(Cur);

      const va_t InsnSize = static_cast<va_t>(Sz);
      if (InsnSize > std::numeric_limits<va_t>::max() - Cur) {
        DecodeFailureAddresses.insert(Cur);
        break;
      }
      const va_t Next = Cur + InsnSize;

      InsnRecord Rec;
      Rec.Addr = Cur;
      Rec.Size = static_cast<uint16_t>(Sz);
      Rec.Mode = Dec.currentMode();
      Rec.Immediate = Dec.returnImmediate(DI);
      Rec.TargetMode = Dec.controlTargetMode(DI, Rec.Mode);
      Rec.FpuTopIn = Dec.getX86FpuTop();
      const InstructionRelocations Relocations =
          instructionRelocations(Img, DI);
      const std::vector<RelocatedAddressOperand> &RelocatedOperands =
          Relocations.Addresses;
      try {
        Dec.liftToLow(DI, Rec.Ops, Relocations.Addresses, Relocations.Scalars);
      } catch (const UnliftedInstruction &Failure) {
        UnsupportedInstructionAddresses.insert(Failure.getAddr());
        break;
      }
      if (std::optional<I386GetPcOccurrence> GetPc =
              Dec.getX86GetPcOccurrence())
        I386GetPcOccurrences.push_back(*GetPc);
      if (std::optional<RelocatedInstructionScalarOperandOccurrence> Scalar =
              Dec.getX86ScalarOperandOccurrence())
        RelocatedInstructionScalarOperandOccurrences.push_back(*Scalar);
      for (const RelocatedAddressOperand &Reloc : RelocatedOperands) {
        for (const LowOp &Op : Rec.Ops) {
          auto Matches = [&](const NdVar &Value) {
            return Value.isConst() &&
                   isExactAddressProvenance(Value.Provenance) &&
                   Value.Provenance == Reloc.Provenance &&
                   Value.Offset == Reloc.TargetVA &&
                   (Reloc.TargetOwnerVA == InvalidVA ||
                    Value.AddressOwnerVA == Reloc.TargetOwnerVA);
          };
          const bool ConsumedOutput = Matches(Op.Output);
          unsigned MatchingInputs = 0;
          int MatchingInput = -1;
          for (uint8_t I = 0; I < Op.NumInputs; ++I)
            if (Matches(Op.Inputs[I])) {
              ++MatchingInputs;
              MatchingInput = I;
            }
          if (!ConsumedOutput && MatchingInputs == 0)
            continue;
          RelocatedInstructionAddressOccurrence Occurrence{
              Reloc.FieldVA,
              Rec.Addr,
              Op.Seq,
              Reloc.TargetVA,
              Reloc.TargetOwnerVA,
              Reloc.Width,
              Reloc.Provenance,
              Reloc.PCRelativeFromInstructionEnd};
          if (!ConsumedOutput && MatchingInputs == 1)
            Occurrence.InputIndex = MatchingInput;
          RelocatedInstructionAddressOccurrences.push_back(
              std::move(Occurrence));
          break;
        }
      }
      ++LiftedInstructionCount;
      LiftedInstructionAddresses.insert(Cur);
      Rec.FpuTopOut = Dec.getX86FpuTop();
      Rec.FpuReset = Dec.x86FpuDidReset();

      // A relocation-free PC-relative `lea` taking the address of executable
      // code is a same-section function pointer the assembler resolved (so the
      // loader saw no relocation).  Record the target so the emitter symbolizes
      // the folded constant to `ptrtoint @func` rather than the stale VA.
      if (va_t Ref = Dec.pcRelCodeRefTarget(DI); Ref != InvalidVA) {
        if (Img.hasExecutableCodeOwnerAt(Ref, ExecutableCodeOwners)) {
          DiscoveredCodeRefs.insert(Ref);
          DiscoveredCodeRefSources[Ref].insert(Cur);

          // Preserve the decoded address-of as an occurrence certificate, not
          // merely as a module-wide target number.  A pure RIP/EIP-relative
          // LEA has exactly one architectural pointer-width COPY from its
          // effective-address temporary.  Binding that exact output prevents a
          // later scalar with the same numeric value (or a LEA into a different
          // register) from acquiring code-address provenance.
          const LowOp *Output = nullptr;
          bool AmbiguousOutput = false;
          const uint16_t PointerSize = Img.getPointerSize();
          for (const LowOp &Op : Rec.Ops) {
            if (Op.Addr != Cur || Op.Opcode != NdOp::COPY ||
                !Op.Output.isReg() || Op.Output.Size != PointerSize ||
                Op.NumInputs != 1 || !Op.Inputs[0].isTemp() ||
                Op.Inputs[0].Size != PointerSize)
              continue;
            if (Output) {
              AmbiguousOutput = true;
              break;
            }
            Output = &Op;
          }

          va_t TargetOwnerVA = InvalidVA;
          if (const Section *Sec = Img.getSectionFor(Ref);
              Sec && Sec->isExecutable())
            TargetOwnerVA = Sec->VA;
          else if (const Segment *TargetSeg = Img.getSegmentFor(Ref);
                   TargetSeg && TargetSeg->isExecutable())
            TargetOwnerVA = TargetSeg->VA;

          if (!AmbiguousOutput && Output && RelocatedOperands.empty() &&
              PointerSize != 0 && TargetOwnerVA != InvalidVA &&
              Img.relocatedTargetBelongsToOwner(Ref, TargetOwnerVA,
                                                ExecutableCodeOwners)) {
            RelocatedInstructionAddressOccurrence Occurrence;
            Occurrence.InstructionAddr = Cur;
            Occurrence.OpSeq = Output->Seq;
            Occurrence.TargetVA = Ref;
            Occurrence.TargetOwnerVA = TargetOwnerVA;
            Occurrence.Width = PointerSize;
            Occurrence.Provenance = ConstantAddressProvenance::CodeAddress;
            Occurrence.PCRelativeFromInstructionEnd = true;
            Occurrence.DefinesOutput = true;
            Occurrence.OutputOpcode = Output->Opcode;
            Occurrence.OutputWitness = Output->Output;
            Occurrence.Authority =
                RelocatedInstructionAddressProofKind::X86PCRelativeCodeAddress;
            Occurrence.SeedInstructionAddr = Cur;
            Occurrence.SeedOpSeq = Output->Seq;
            Occurrence.SeedOpcode = Output->Opcode;
            Occurrence.SeedInputWitness = Output->Inputs[0];
            Occurrence.SeedOutputWitness = Output->Output;
            RelocatedInstructionAddressOccurrences.push_back(
                std::move(Occurrence));
          }
        }
      }

      restoreAdjacentNoReturnCall(Rec, Dec.directCallTarget(DI));
      if (Img.Arch == Arch::X86 && Rec.Ops.size() == 2)
        if (const auto *EH = registration_state::installedRegistration(
                Img, CurrentFuncEntry))
          registration_state::restoreAdjacentSEHFinallyCall(Img, *EH, Rec.Addr,
                                                            Rec.Size, Rec.Ops);
      rewriteI386PLTJump(Img, Rec);
      classifyInsn(Rec);
      if (Rec.IsCall && !Rec.IsIndirect && Rec.Immediate)
        rewriteGetPcThunkCall(Img, Rec, Next);
      if (Rec.IsCall && !Rec.IsIndirect && Rec.Immediate)
        rewriteOwnInteriorCall(Img, Rec, Next);
      rewriteOwnInteriorReturn(Img, Rec);
      rewriteOwnInteriorTailReturn(Img, Rec);

      // Keep recursive CFG exploration consistent with the decoder's
      // architecture-specific terminator classification.  Trap instructions
      // such as x86 INT3/UD2 lift to an intrinsic rather than a RETURN op, but
      // control does not fall through into the inline data or padding that
      // commonly follows them.  Real branches/returns were already classified
      // from their LowIR ops and retain their more precise handling below.
      //
      // Debugger traps are resumable: `__debugbreak()` can put one in the
      // middle of an ordinary function with the epilogue behind it.
      // Whether it ends the block is therefore decided by what follows, which
      // separates that case from the run of `int3` a linker pads with and from
      // the embedded data a trap is often planted in front of. A known range
      // end or another function's entry takes precedence over decodable bytes:
      // resuming there must not manufacture a tail call to the next function.
      if (!Rec.IsBranch && !Rec.IsRet && Dec.isFunctionTerminator(DI)) {
        Rec.IsOpaqueTerminator = true;
        Rec.IsResumableTerminator = Dec.isResumableTrap(DI);
        Rec.IsRet = !Rec.IsResumableTerminator ||
                    codeRangeEndsAt(Rec.Addr, Next) ||
                    !codeFollowsTrap(Dec, Next);
      }

      // operator[]= returns the stored element, so bind to it directly rather
      // than doing a second tree lookup for the same key on the decode hot
      // path.
      auto &Saved = (Insns[Cur] = std::move(Rec));

      // AArch64 can spell a direct local transfer through an explicit RET
      // register (`adr x16, label; ret x16`).  Fold that dominating constant
      // before treating the unresolved branch as a function-pointer tail call,
      // so recursive descent reaches the local target and keeps it in this
      // function's CFG.
      resolveConstantIndirectBranch(Img, DI.Id, Saved);
      // Relocation-backed interior branches are deliberately resolved later,
      // after every address-taken root has been decoded.  Until then the CFG's
      // predecessor set is incomplete and cannot prove a unique relay path.

      if (Saved.IsRet && !(Saved.IsCond && Saved.IsBranch))
        break;
      if (Saved.IsRet && Saved.IsCond && Saved.IsBranch) {
        if (Saved.BranchTarget != InvalidVA) {
          BlockStarts.insert(Saved.BranchTarget);
          if (!ExploredAddrs.count(Saved.BranchTarget))
            Worklist.push(
                {Saved.BranchTarget, std::nullopt, branchMode(Saved)});
        }
        break;
      }

      // Tail call: an unconditional direct branch to *another* known function's
      // entry is `call target; ret`, not intra-function control flow. Following
      // the target inlines the callee into this CFG (harmless for an acyclic
      // tail chain, but it fuses mutually-recursive functions into one bogus
      // cyclic CFG).  Rewrite it to an explicit CALL + RETURN and stop
      // exploring here.
      if (Saved.IsBranch && !Saved.IsCall && !Saved.IsCond &&
          !Saved.IsIndirect && isTailCallTarget(Saved.BranchTarget)) {
        if (isBackwardSharedEpilogue(Img, CurrentFuncEntry, Saved.Addr,
                                     Saved.BranchTarget, KnownFuncEntries)) {
          BlockStarts.insert(Saved.BranchTarget);
          if (!ExploredAddrs.count(Saved.BranchTarget))
            Worklist.push({Saved.BranchTarget, Saved.Addr, branchMode(Saved)});
          break;
        }
        rewriteAsTailCall(Saved);
        break;
      }

      if (Saved.IsBranch && !Saved.IsCall) {
        if (Saved.IsIndirect && !Saved.IsCond) {
          if (!CandidateFiniteProofDecodeOnly) {
            auto Targets = resolveJumpTable(Img, Saved);
            if (!Targets.empty()) {
              Saved.JumpTableTargets = std::move(Targets);
              for (va_t T : Saved.JumpTableTargets) {
                BlockStarts.insert(T);
                if (!ExploredAddrs.count(T))
                  Worklist.push({T, std::nullopt, branchMode(Saved)});
              }
            }
          }
        }
        if (Saved.BranchTarget != InvalidVA) {
          BlockStarts.insert(Saved.BranchTarget);
          if (!ExploredAddrs.count(Saved.BranchTarget))
            Worklist.push(
                {Saved.BranchTarget, std::nullopt, branchMode(Saved)});
        }
        if (Saved.IsCond) {
          va_t Fall = Next;
          BlockStarts.insert(Fall);
          if (!ExploredAddrs.count(Fall))
            Worklist.push({Fall, std::nullopt, Saved.Mode});
        }
        break;
      }

      // An unconditional call to a no-return libc function (longjmp / abort /
      // exit / ...), direct or through the import slot the loader binds, is a
      // control-flow terminator.  At -O2 the compiler emits nothing after it,
      // so continuing would absorb the next function into this CFG.  A
      // predicated ARM call is different: its false path must continue at the
      // following instruction even though its taken path never returns.
      if (Saved.IsCall && isNoReturnCall(Saved)) {
        Saved.IsNoReturnCall = true;
        if (!Saved.IsCond)
          break;
      }

      // This runtime call retires a registration-state transition just like
      // a source try-level store. Give its ordinary continuation a new block.
      if (Saved.IsCall && Img.Arch == Arch::X86 &&
          registration_state::installedRegistration(Img, CurrentFuncEntry))
        for (const auto &Op : Saved.Ops)
          if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
              Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
              Op.Inputs[0].Size == 4 &&
              getCheckedX86LocalUnwindContract(Img, Op.Inputs[0].Offset,
                                               Op.Opcode == NdOp::INDIR_CALL))
            BlockStarts.insert(Next);

      Cur = Next;
    }
  }
  // Model each such edge at the entry as `call target; ret`, like an
  // unconditional jump there; otherwise the edge has no block.  An
  // authenticated B later in this walk may have decoded the entry as a shared
  // epilogue instead, and then every edge keeps that real block.
  for (const auto &[Entry, IncomingMode] : TailCallEntries) {
    if (Insns.count(Entry))
      continue;
    const auto Mode = Img.instructionModeAt(Entry, IncomingMode);
    if (!Mode) {
      DecodeFailureAddresses.insert(Entry);
      continue;
    }
    InsnRecord Stub;
    Stub.Addr = Entry;
    Stub.Size = 1;
    Stub.Mode = *Mode;
    Stub.BranchTarget = Entry;
    rewriteAsTailCall(Stub);
    // This block models an edge into another function, not instructions
    // decoded at that function's entry. Preserve synthetic provenance through
    // Low-to-Med lowering; sequence zero would falsely bind the call/return to
    // an original operation in the callee.
    for (auto &Op : Stub.Ops)
      Op.Seq = -1;
    Insns[Entry] = std::move(Stub);
    ExploredAddrs.insert(Entry);
  }
}

std::set<va_t> CFGBuilder::currentRelocatedInstructionTableAnchors(
    const BinaryImage &Img) const {
  std::set<va_t> Anchors;
  for (const RelocatedInstructionAddressOccurrence &Occurrence :
       RelocatedInstructionAddressOccurrences) {
    if (!PublishedReachableInsns.count(Occurrence.InstructionAddr) ||
        Occurrence.OutputMayDepend ||
        Occurrence.Provenance != ConstantAddressProvenance::DataAddress)
      continue;
    if (Img.RelCodeRelocSlots.count(Occurrence.TargetVA) ||
        Img.CodePtrRelocSlots.count(Occurrence.TargetVA))
      Anchors.insert(Occurrence.TargetVA);
  }
  return Anchors;
}

void CFGBuilder::completeExactI386GOTBaseModels(const BinaryImage &Img) {
  // Destruction here was prepaid when the preceding generation published
  // each occurrence.  I386GetPcOccurrence initializes RawPCAuthenticated to
  // false. Reset the published permission before every proof refresh so a
  // changed root set or an exhausted budget cannot retain an earlier proof.
  for (auto &Occurrence : I386GetPcOccurrences)
    Occurrence.RawPCAuthenticated = false;
  RelocatedInstructionScalarModelOccurrences.clear();
  I386GOTModelEvidenceIncomplete = false;
  if (Img.Arch != Arch::X86 || Img.getPointerSize() != 4 ||
      !JumpTableProofContextComplete)
    return;

  size_t EvidenceWork =
      std::min<size_t>(limits::kMaxI386GOTModelEvidenceWork,
                       I386GOTModelEvidenceBudgetForTesting.value_or(
                           limits::kMaxI386GOTModelEvidenceWork));
  auto Consume = [&](size_t Amount = 1) {
    if (Amount > EvidenceWork) {
      EvidenceWork = 0;
      I386GOTModelEvidenceIncomplete = true;
      return false;
    }
    EvidenceWork -= Amount;
    return true;
  };
  auto OrderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = (N + 1) / 2)
      ++Work;
    return Work;
  };
  auto ConsumeProducts =
      [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
        const size_t Max = std::numeric_limits<size_t>::max();
        size_t Total = 0;
        for (const auto &[Count, Cost] : Products) {
          if (Count != 0 && Cost > Max / Count) {
            Consume(Max);
            return false;
          }
          const size_t Product = Count * Cost;
          if (Product > Max - Total) {
            Consume(Max);
            return false;
          }
          Total += Product;
        }
        return Consume(Total);
      };
  if (!Consume(RelocatedInstructionScalarOperandOccurrences.size()) ||
      !Consume(I386GetPcOccurrences.size()))
    return;

  struct PendingModel {
    va_t FieldVA = InvalidVA;
    const LowOp *Op = nullptr;
    const I386GetPcOccurrence *Seed = nullptr;
    size_t QueryIndex = 0;
  };
  // A model whose base input the add's own instruction proves needs no
  // graph query.
  constexpr size_t LocallyProvenModel = std::numeric_limits<size_t>::max();
  std::vector<JumpTableValueQuery> Queries;
  std::vector<PendingModel> Pending;

  std::map<uint32_t, std::vector<const I386GetPcOccurrence *>> SeedsByPC;
  std::vector<I386GetPcOccurrence *> AuthenticatedGetPcSeeds;
  using SeedIdentity =
      std::tuple<va_t, int, uint8_t, uint64_t, uint16_t, uint8_t, uint64_t>;
  constexpr size_t SeedIdentityWork = 7;
  std::set<SeedIdentity> SeenSeedPoints;
  const size_t GetPcCount = I386GetPcOccurrences.size();
  const size_t OperandCount =
      RelocatedInstructionScalarOperandOccurrences.size();
  // Reserve every outer vector before scanning the graph.  Per-element copy,
  // nested alternative allocation and eventual destruction are charged here;
  // ordered map/set nodes remain charged immediately before insertion.
  if (!ConsumeProducts({{GetPcCount, 4}, {OperandCount, 12}}))
    return;
  AuthenticatedGetPcSeeds.reserve(GetPcCount);
  Queries.reserve(OperandCount);
  Pending.reserve(OperandCount);
  RelocatedInstructionScalarModelOccurrences.reserve(OperandCount);
  for (I386GetPcOccurrence &GetPc : I386GetPcOccurrences) {
    if (!Consume())
      return;
    if (!ConsumeProducts(
            {{1, OrderedLookupWork(PublishedReachableInsns.size())},
             {1, OrderedLookupWork(PersistentCFGRoots.size())},
             {2, OrderedLookupWork(Insns.size())}}))
      return;
    // A get-PC thunk call is lifted as `COPY reg, <return address>` at the
    // call itself.  That literal is the exact PC however control reaches the
    // call, so the seed needs none of the POP's stack proof below, only the
    // unchanged rewritten instruction.
    if (GetPc.CallInstructionAddr == GetPc.InstructionAddr &&
        GetPc.CallInstructionAddr != InvalidVA) {
      const auto RecIt = Insns.find(GetPc.InstructionAddr);
      if (GetPc.OutputOpcode != NdOp::COPY || !GetPc.OutputWitness.isReg() ||
          GetPc.OutputWitness.Size != 4 || !GetPc.InputWitness.isConst() ||
          GetPc.InputWitness.Size != 4 ||
          static_cast<uint32_t>(GetPc.InputWitness.Offset) != GetPc.PCValue ||
          !PublishedReachableInsns.count(GetPc.InstructionAddr) ||
          RecIt == Insns.end() || RecIt->second.IsInstructionGuard ||
          RecIt->second.IsCall || RecIt->second.Size == 0 ||
          RecIt->second.Size > InvalidVA - GetPc.InstructionAddr ||
          GetPc.InstructionAddr + RecIt->second.Size != GetPc.PCValue ||
          RecIt->second.Ops.size() != 1)
        continue;
      const LowOp &Copy = RecIt->second.Ops.front();
      if (Copy.Opcode != NdOp::COPY || Copy.Addr != GetPc.InstructionAddr ||
          Copy.Seq != GetPc.OpSeq || Copy.Output != GetPc.OutputWitness ||
          Copy.NumInputs != 1 || Copy.Inputs[0] != GetPc.InputWitness)
        continue;
      const SeedIdentity Identity =
          std::make_tuple(GetPc.InstructionAddr, GetPc.OpSeq,
                          static_cast<uint8_t>(GetPc.OutputWitness.Space),
                          GetPc.OutputWitness.Offset, GetPc.OutputWitness.Size,
                          static_cast<uint8_t>(GetPc.OutputWitness.Provenance),
                          GetPc.OutputWitness.AddressOwnerVA);
      if (!ConsumeProducts(
              {{SeedIdentityWork, OrderedLookupWork(SeenSeedPoints.size())},
               {SeedIdentityWork, 2},
               {1, 2},
               {1, OrderedLookupWork(SeedsByPC.size())},
               {1, 7}}))
        return;
      if (!SeenSeedPoints.insert(Identity).second)
        return;
      SeedsByPC[GetPc.PCValue].push_back(&GetPc);
      continue;
    }
    if (GetPc.OutputOpcode != NdOp::COPY || GetPc.OutputWitness.Size != 4 ||
        (!GetPc.OutputWitness.isReg() && !GetPc.OutputWitness.isTemp()) ||
        !GetPc.InputWitness.isTemp() || GetPc.InputWitness.Size != 4 ||
        GetPc.PCValue != GetPc.InstructionAddr ||
        GetPc.CallInstructionAddr == InvalidVA ||
        GetPc.CallInstructionAddr >= GetPc.InstructionAddr ||
        !PublishedReachableInsns.count(GetPc.InstructionAddr) ||
        PersistentCFGRoots.count(GetPc.InstructionAddr))
      continue;

    // The pop is a valid get-PC seed only when the adjacent call is its sole
    // machine-code predecessor.  An address-taken/exception root, another
    // fall-through decode, or a direct/indirect branch into the pop can enter
    // with an arbitrary stack value and must not inherit the call's pushed PC.
    const auto CallIt = Insns.find(GetPc.CallInstructionAddr);
    if (CallIt == Insns.end() || CallIt->second.Size == 0 ||
        CallIt->second.Size > InvalidVA - GetPc.CallInstructionAddr ||
        GetPc.CallInstructionAddr + CallIt->second.Size !=
            GetPc.InstructionAddr)
      continue;
    const LowOp *PushAdjust = nullptr;
    const LowOp *PushStore = nullptr;
    if (!Consume(CallIt->second.Ops.size()))
      return;
    for (const LowOp &Op : CallIt->second.Ops) {
      if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2 &&
          Op.Output.isReg() && Op.Output.Size == 4 &&
          Op.Inputs[0] == Op.Output && Op.Inputs[1].isConst() &&
          Op.Inputs[1].Size == 4 && Op.Inputs[1].Offset == 4) {
        if (PushAdjust)
          return;
        PushAdjust = &Op;
      }
      if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
          Op.Inputs[0].isReg() && Op.Inputs[0].Size == 4 &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Size == 4 &&
          static_cast<uint32_t>(Op.Inputs[1].Offset) == GetPc.PCValue) {
        if (PushStore)
          return;
        PushStore = &Op;
      }
    }
    if (!PushAdjust || !PushStore ||
        PushStore->Inputs[0] != PushAdjust->Output ||
        PushAdjust->Seq >= PushStore->Seq)
      continue;
    bool HasAlternateEntry = false;
    for (const auto &[Addr, Rec] : Insns) {
      if (!Consume() || !Consume(Rec.JumpTableTargets.size()))
        return;
      if (Addr != GetPc.CallInstructionAddr && Rec.Size != 0 &&
          Rec.Size <= InvalidVA - Addr &&
          Addr + Rec.Size == GetPc.InstructionAddr)
        HasAlternateEntry = true;
      if ((Rec.BranchTarget == GetPc.InstructionAddr) ||
          (Rec.IsCall && Rec.Immediate &&
           *Rec.Immediate == GetPc.InstructionAddr) ||
          llvm::is_contained(Rec.JumpTableTargets, GetPc.InstructionAddr))
        HasAlternateEntry = true;
    }
    if (HasAlternateEntry)
      continue;
    const SeedIdentity Identity =
        std::make_tuple(GetPc.InstructionAddr, GetPc.OpSeq,
                        static_cast<uint8_t>(GetPc.OutputWitness.Space),
                        GetPc.OutputWitness.Offset, GetPc.OutputWitness.Size,
                        static_cast<uint8_t>(GetPc.OutputWitness.Provenance),
                        GetPc.OutputWitness.AddressOwnerVA);
    if (!ConsumeProducts(
            {{SeedIdentityWork, OrderedLookupWork(SeenSeedPoints.size())},
             {SeedIdentityWork, 2},
             // Set-node allocation and its eventual destruction.
             {1, 2}}))
      return;
    if (!SeenSeedPoints.insert(Identity).second)
      return;
    const auto RecIt = Insns.find(GetPc.InstructionAddr);
    if (RecIt == Insns.end() || RecIt->second.IsInstructionGuard)
      continue;
    const LowOp *Producer = nullptr;
    for (const LowOp &Op : RecIt->second.Ops) {
      if (!Consume())
        return;
      if (Op.Addr == GetPc.InstructionAddr && Op.Seq == GetPc.OpSeq &&
          Op.Opcode == GetPc.OutputOpcode && Op.Output == GetPc.OutputWitness) {
        if (Producer)
          return;
        Producer = &Op;
      }
    }
    if (!Producer || Producer->NumInputs != 1 ||
        Producer->Inputs[0] != GetPc.InputWitness)
      continue;
    const LowOp *PopLoad = nullptr;
    const LowOp *LegacyPopAdjust = nullptr;
    const LowOp *StagedPopAdjust = nullptr;
    const LowOp *StagedStackCommit = nullptr;
    for (const LowOp &Op : RecIt->second.Ops) {
      if (!Consume())
        return;
      if (Op.Addr == GetPc.InstructionAddr && Op.Opcode == NdOp::LOAD &&
          Op.NumInputs == 1 && Op.Output == Producer->Inputs[0] &&
          Op.Inputs[0].isReg() && Op.Output.isTemp() && Op.Output.Size == 4 &&
          Op.Inputs[0].Size == 4) {
        if (PopLoad)
          return;
        PopLoad = &Op;
      }
      const bool IsStackAdjust =
          Op.Addr == GetPc.InstructionAddr && Op.Opcode == NdOp::INT_ADD &&
          Op.NumInputs == 2 && Op.Output.Size == 4 && Op.Inputs[0].isReg() &&
          Op.Inputs[0].Size == 4 && Op.Inputs[1].isConst() &&
          Op.Inputs[1].Size == 4 && Op.Inputs[1].Offset == 4;
      if (IsStackAdjust) {
        if (Op.Output.isReg() && Op.Inputs[0] == Op.Output) {
          if (LegacyPopAdjust)
            return;
          LegacyPopAdjust = &Op;
        } else if (Op.Output.isTemp()) {
          if (StagedPopAdjust)
            return;
          StagedPopAdjust = &Op;
        }
      }
      if (&Op != Producer && Op.Addr == GetPc.InstructionAddr &&
          Op.Opcode == NdOp::COPY && Op.NumInputs == 1 && Op.Output.isReg() &&
          Op.Output.Size == 4 && Op.Inputs[0].isTemp() &&
          Op.Inputs[0].Size == 4) {
        if (StagedStackCommit)
          return;
        StagedStackCommit = &Op;
      }
    }
    if (!PopLoad || PopLoad->Inputs[0] != PushStore->Inputs[0])
      continue;
    const bool LegacyShape =
        LegacyPopAdjust && LegacyPopAdjust->Output == PopLoad->Inputs[0] &&
        PopLoad->Seq < Producer->Seq && Producer->Seq < LegacyPopAdjust->Seq;
    const bool StagedShape =
        StagedPopAdjust && StagedStackCommit &&
        StagedPopAdjust->Inputs[0] == PopLoad->Inputs[0] &&
        StagedStackCommit->Output == PopLoad->Inputs[0] &&
        StagedStackCommit->Inputs[0] == StagedPopAdjust->Output &&
        PopLoad->Seq < StagedPopAdjust->Seq &&
        StagedPopAdjust->Seq < StagedStackCommit->Seq &&
        StagedStackCommit->Seq < Producer->Seq;
    if (LegacyShape == StagedShape)
      continue;
    if (!ConsumeProducts(
            {{1, OrderedLookupWork(SeedsByPC.size())},
             // Possible map node, nested vector element/allocation/destruction,
             // and the authenticated-seed vector element/destruction.
             {1, 7}}))
      return;
    SeedsByPC[GetPc.PCValue].push_back(&GetPc);
    AuthenticatedGetPcSeeds.push_back(&GetPc);
  }

  // Mach-O/COFF PIC arithmetic has no ELF R_386_GOTPC scalar field to bind,
  // but the same call-next/POP seed is still an exact architectural PC once
  // the CFG proof above succeeds.  Publish that fact without rewriting the
  // ordinary LOAD/COPY: Low-to-Med will bind the exact surviving occurrence,
  // and the LLVM data resolver may fold only arithmetic rooted at that bound
  // value.  ELF keeps its stricter combined call/POP + exact GOTPC contract
  // below for relocatable objects. Linked ELF has already applied its GOTPC
  // displacement, so the same exact PC root is sufficient there.
  if (!Img.isELF() || !Img.IsRelocatable) {
    if (!ConsumeProducts({{AuthenticatedGetPcSeeds.size(), 2}}))
      return;
    for (I386GetPcOccurrence *Seed : AuthenticatedGetPcSeeds)
      Seed->RawPCAuthenticated = true;
    return;
  }
  if (Img.I386GOTPCFields.empty())
    return;

  std::set<std::tuple<va_t, va_t, int>> SeenOperandPoints;
  for (const RelocatedInstructionScalarOperandOccurrence &Operand :
       RelocatedInstructionScalarOperandOccurrences) {
    constexpr size_t OperandPointKeyWork = 3;
    if (!Consume(3) ||
        !ConsumeProducts(
            {{1, OrderedLookupWork(PublishedReachableInsns.size())},
             {OperandPointKeyWork, OrderedLookupWork(SeenOperandPoints.size())},
             {OperandPointKeyWork, 2},
             // Set-node allocation and its eventual destruction.
             {1, 2},
             {2, OrderedLookupWork(Img.I386GOTPCFields.size())},
             {1, OrderedLookupWork(Insns.size())},
             {1, OrderedLookupWork(SeedsByPC.size())}}))
      return;
    if (Operand.Kind != RelocatedInstructionScalarOperandOccurrence::
                            OperandKind::I386ELFGOTPC ||
        Operand.Width != 4 || Operand.InputIndex >= 2 ||
        Operand.Opcode != NdOp::INT_ADD || Operand.OutputWitness.Size != 4 ||
        (!Operand.OutputWitness.isReg() && !Operand.OutputWitness.isTemp()) ||
        !PublishedReachableInsns.count(Operand.InstructionAddr) ||
        !SeenOperandPoints
             .insert({Operand.FieldVA, Operand.InstructionAddr, Operand.OpSeq})
             .second)
      return;

    const auto FieldIt = Img.I386GOTPCFields.find(Operand.FieldVA);
    const auto InsnIt = Insns.find(Operand.InstructionAddr);
    if (FieldIt == Img.I386GOTPCFields.end() || InsnIt == Insns.end() ||
        InsnIt->second.IsInstructionGuard || InsnIt->second.Size == 0 ||
        InsnIt->second.Size > InvalidVA - Operand.InstructionAddr ||
        Operand.FieldVA < Operand.InstructionAddr ||
        Operand.FieldVA >= Operand.InstructionAddr + InsnIt->second.Size ||
        FieldIt->second.EncodedValue !=
            static_cast<uint32_t>(Operand.EncodedValue) ||
        static_cast<uint32_t>(FieldIt->second.EncodedValue +
                              FieldIt->second.ExpectedPCValue) != 0)
      continue;

    unsigned InstructionFields = 0;
    auto Field = Img.I386GOTPCFields.lower_bound(Operand.InstructionAddr);
    for (; Field != Img.I386GOTPCFields.end() &&
           Field->first < Operand.InstructionAddr + InsnIt->second.Size;
         ++Field) {
      if (!Consume())
        return;
      ++InstructionFields;
    }
    if (InstructionFields != 1)
      continue;

    const LowOp *Candidate = nullptr;
    for (const LowOp &Op : InsnIt->second.Ops) {
      if (!Consume())
        return;
      if (Op.Addr == Operand.InstructionAddr && Op.Seq == Operand.OpSeq &&
          Op.Opcode == Operand.Opcode && Op.Output == Operand.OutputWitness) {
        if (Candidate)
          return;
        Candidate = &Op;
      }
    }
    if (!Candidate || Candidate->NumInputs != 2 ||
        Operand.InputIndex >= Candidate->NumInputs)
      continue;
    const NdVar &Immediate = Candidate->Inputs[Operand.InputIndex];
    const NdVar &Base = Candidate->Inputs[1 - Operand.InputIndex];
    if (!Immediate.isConst() || Immediate.Size != 4 ||
        Immediate.Provenance != ConstantAddressProvenance::Scalar ||
        static_cast<uint32_t>(Immediate.Offset) !=
            FieldIt->second.EncodedValue ||
        (!Base.isReg() && !Base.isTemp()) || Base.Size != 4)
      continue;

    const auto Seeds = SeedsByPC.find(FieldIt->second.ExpectedPCValue);
    if (Seeds == SeedsByPC.end() || Seeds->second.size() != 1)
      continue;
    const I386GetPcOccurrence &Seed = *Seeds->second.front();
    if (!Consume(2))
      return;
    // GCC adds the GOTPC immediate right after its thunk call, which is lifted
    // as a copy of the very address the field expects.  When nothing else
    // enters the add and none of its ops before the sum writes the base
    // register, that register holds the copy, without a graph query.
    if (Seed.CallInstructionAddr == Seed.InstructionAddr &&
        Seed.PCValue == Operand.InstructionAddr && Base.isReg() &&
        Base == Seed.OutputWitness) {
      bool BaseWritten = false;
      for (const LowOp &Op : InsnIt->second.Ops) {
        if (!Consume())
          return;
        if (&Op == Candidate)
          break;
        BaseWritten |= Op.Output.isReg() && Op.Output.Size != 0 &&
                       Op.Output.Offset < Base.Offset + Base.Size &&
                       Base.Offset < Op.Output.Offset + Op.Output.Size;
      }
      bool HasAlternateEntry =
          PersistentCFGRoots.count(Operand.InstructionAddr) != 0;
      for (const auto &[Addr, Rec] : Insns) {
        if (HasAlternateEntry)
          break;
        if (!Consume() || !Consume(Rec.JumpTableTargets.size()))
          return;
        HasAlternateEntry =
            (Addr != Seed.InstructionAddr && Rec.Size != 0 &&
             Rec.Size <= InvalidVA - Addr &&
             Addr + Rec.Size == Operand.InstructionAddr) ||
            Rec.BranchTarget == Operand.InstructionAddr ||
            (Rec.IsCall && Rec.Immediate &&
             *Rec.Immediate == Operand.InstructionAddr) ||
            llvm::is_contained(Rec.JumpTableTargets, Operand.InstructionAddr);
      }
      if (!BaseWritten && !HasAlternateEntry) {
        Pending.push_back(
            {Operand.FieldVA, Candidate, &Seed, LocallyProvenModel});
        continue;
      }
    }
    JumpTableValueQuery Query;
    Query.Candidate = Base;
    Query.UseAddr = Candidate->Addr;
    Query.UseSeq = Candidate->Seq;
    Query.Alternatives.push_back({Seed.OutputWitness, Seed.InstructionAddr,
                                  Seed.OpSeq, /*DefinedAtPoint=*/true});
    Query.RequireExactAddressOwner = true;
    Pending.push_back({Operand.FieldVA, Candidate, &Seed, Queries.size()});
    Queries.push_back(std::move(Query));
  }

  if (Pending.empty())
    return;
  // Reserve the transactional commit tail before the graph resolver sees the
  // remaining balance.  A complete query must still scan every pending model
  // and release its result buffer; those operations cannot occur after graph
  // propagation has consumed the final evidence unit.
  if (!ConsumeProducts({{Pending.size(), 1}, {1, 1}}))
    return;
  std::vector<bool> Matches;
  if (!Queries.empty()) {
    bool AnalysisComplete = false;
    Matches = tableValuesMatchAtUses(
        Queries, &AnalysisComplete, /*QueryAnalysisComplete=*/nullptr,
        /*CandidateBranchOverride=*/InvalidVA,
        /*CandidateTargetsOverride=*/nullptr, &EvidenceWork);
    if (!AnalysisComplete || Matches.size() != Queries.size()) {
      I386GOTModelEvidenceIncomplete = true;
      return;
    }
  }

  for (const PendingModel &Model : Pending) {
    if (!Model.Op || !Model.Seed ||
        (Model.QueryIndex != LocallyProvenModel &&
         (Model.QueryIndex >= Matches.size() || !Matches[Model.QueryIndex])))
      continue;
    RelocatedInstructionScalarModelOccurrence Occurrence;
    Occurrence.FieldVA = Model.FieldVA;
    Occurrence.InstructionAddr = Model.Op->Addr;
    Occurrence.OpSeq = Model.Op->Seq;
    Occurrence.Width = 4;
    Occurrence.Model = RelocatedInstructionScalarModelOccurrence::ModelKind::
        I386ELFGOTBaseZero;
    Occurrence.OutputOpcode = Model.Op->Opcode;
    Occurrence.OutputWitness = Model.Op->Output;
    Occurrence.SeedInstructionAddr = Model.Seed->InstructionAddr;
    Occurrence.SeedOpSeq = Model.Seed->OpSeq;
    Occurrence.SeedOpcode = Model.Seed->OutputOpcode;
    Occurrence.SeedOutputWitness = Model.Seed->OutputWitness;
    RelocatedInstructionScalarModelOccurrences.push_back(std::move(Occurrence));
  }
}

void CFGBuilder::completeExactAArch64PageBases(const BinaryImage &Img) {
  if (Img.Arch != Arch::AArch64)
    return;

  auto sameRegister = [](const NdVar &Left, const NdVar &Right) {
    return Left.isReg() && Right.isReg() && Left.Offset == Right.Offset &&
           Left.Size == Right.Size;
  };
  auto overlapsRegister = [](const NdVar &Left, const NdVar &Right) {
    if (!Left.isReg() || !Right.isReg() || Left.Size == 0 || Right.Size == 0)
      return false;
    const uint64_t LeftEnd = Left.Offset + Left.Size;
    const uint64_t RightEnd = Right.Offset + Right.Size;
    return Left.Offset < RightEnd && Right.Offset < LeftEnd;
  };

  // ADRP always clears the architectural low 12 bits, independent of the
  // operating system's VM page size.
  constexpr uint64_t AArch64PageMask = 0xfff;

  for (auto RootIt = Insns.begin(); RootIt != Insns.end(); ++RootIt) {
    InsnRecord &RootRec = RootIt->second;
    if (RootRec.Ops.size() != 1)
      continue;
    LowOp &Root = RootRec.Ops.front();
    if (Root.Opcode != NdOp::COPY || Root.NumInputs != 1 ||
        !Root.Output.isReg() || Root.Output.Size != Img.getPointerSize() ||
        !Root.Inputs[0].isConst() ||
        Root.Inputs[0].Provenance != ConstantAddressProvenance::AddressFragment)
      continue;

    const va_t PageBase = Root.Inputs[0].Offset;
    const auto PageMaterialization =
        Img.InstructionPageAddressFragments.find(RootRec.Addr);
    const bool HasAuthenticatedPage =
        PageMaterialization != Img.InstructionPageAddressFragments.end() &&
        PageMaterialization->second.TargetOwnerVA != InvalidVA &&
        Img.relocatedTargetBelongsToOwner(
            PageMaterialization->second.TargetVA,
            PageMaterialization->second.TargetOwnerVA, ExecutableCodeOwners) &&
        (PageMaterialization->second.TargetVA & ~AArch64PageMask) == PageBase;
    const bool CanAuthenticatePageByDereference =
        Img.hasObjectDataProvenance(PageBase) &&
        !Img.hasExecutableCodeOwnerAt(PageBase, ExecutableCodeOwners);
    if ((PageBase & AArch64PageMask) != 0 ||
        (!HasAuthenticatedPage && !CanAuthenticatePageByDereference))
      continue;
    if (HasAuthenticatedPage)
      Root.Inputs[0].AddressOwnerVA = PageMaterialization->second.TargetOwnerVA;

    va_t OwnerVA = InvalidVA;
    if (CanAuthenticatePageByDereference) {
      const Segment *OwnerSegment = Img.getSegmentFor(PageBase);
      if (!OwnerSegment)
        continue;
      const Section *OwnerSection = Img.getSectionFor(PageBase);
      OwnerVA = OwnerSection ? OwnerSection->VA : OwnerSegment->VA;
    }
    const NdVar PageRegister = Root.Output;

    bool ProvedExactDereference = false;
    va_t ExpectedAddress = RootRec.Addr + RootRec.Size;
    unsigned Lookahead = 0;
    for (auto UseIt = std::next(RootIt);
         UseIt != Insns.end() &&
         Lookahead++ <
             ::neverd::limits::ir::kCFGBuilderMaxLookaheadInstructions;
         ++UseIt) {
      InsnRecord &UseRec = UseIt->second;
      if (UseRec.Addr != ExpectedAddress)
        break;

      // Temporary ids are instruction-local.  Track only expressions that are
      // an exact byte offset from the still-live ADRP destination; this is
      // enough to distinguish [xN] from [xN,#off] without treating unrelated
      // constants in the instruction as addresses.
      std::vector<std::pair<NdVar, uint64_t>> RelativeValues;
      auto relativeOffset = [&](const NdVar &Value) -> std::optional<uint64_t> {
        if (sameRegister(Value, PageRegister))
          return 0;
        for (auto It = RelativeValues.rbegin(); It != RelativeValues.rend();
             ++It)
          if (It->first.Space == Value.Space &&
              It->first.Offset == Value.Offset && It->first.Size == Value.Size)
            return It->second;
        return std::nullopt;
      };
      auto scalarValue = [](const NdVar &Value) -> std::optional<uint64_t> {
        if (!Value.isConst() ||
            Value.Provenance != ConstantAddressProvenance::Scalar)
          return std::nullopt;
        return Value.Offset;
      };

      bool RedefinedPageRegister = false;
      for (const LowOp &Op : UseRec.Ops) {
        if (CanAuthenticatePageByDereference &&
            (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
            Op.NumInputs >= 1) {
          if (auto Offset = relativeOffset(Op.Inputs[0]);
              Offset && *Offset == 0) {
            ProvedExactDereference = true;
            break;
          }
        }

        if (CanAuthenticatePageByDereference &&
            (lowAArch64AddressMemoryShapeIsValid(Op) ||
             lowAArch64WritebackMemoryShapeIsValid(Op))) {
          const auto Offset =
              relativeOffset(Op.Inputs[kAddressMemoryBaseOperand + 1]);
          if (Offset && *Offset == 0 &&
              lowImageBaseMemoryWitness(Img, Op, PageBase, OwnerVA).Complete) {
            ProvedExactDereference = true;
            break;
          }
        }

        std::optional<uint64_t> Derived;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
          Derived = relativeOffset(Op.Inputs[0]);
        } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2) {
          if (auto Base = relativeOffset(Op.Inputs[0])) {
            if (auto Delta = scalarValue(Op.Inputs[1]))
              Derived = *Base + *Delta;
          } else if (auto Base = relativeOffset(Op.Inputs[1])) {
            if (auto Delta = scalarValue(Op.Inputs[0]))
              Derived = *Base + *Delta;
          }
        } else if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2) {
          if (auto Base = relativeOffset(Op.Inputs[0]))
            if (auto Delta = scalarValue(Op.Inputs[1]))
              Derived = *Base - *Delta;
        }

        if (Op.Output.Size != 0 && Derived)
          RelativeValues.emplace_back(Op.Output, *Derived);
        if (overlapsRegister(Op.Output, PageRegister)) {
          RedefinedPageRegister = true;
          break;
        }
      }

      if (ProvedExactDereference || RedefinedPageRegister)
        break;
      if (UseRec.IsBranch || UseRec.IsCall || UseRec.IsRet ||
          UseRec.IsOpaqueTerminator)
        break;
      ExpectedAddress = UseRec.Addr + UseRec.Size;
    }

    if (!ProvedExactDereference)
      continue;
    Root.Inputs[0].Provenance = ConstantAddressProvenance::DataAddress;
    Root.Inputs[0].AddressOwnerVA = OwnerVA;
  }

  if (!JumpTableProofContextComplete)
    return;

  // Output certificates are rebuilt from the current final proof graph.  A
  // certificate from an earlier provisional CFG must not survive after a new
  // root/predecessor makes the PAGEOFF base ambiguous.
  RelocatedInstructionAddressOccurrences.erase(
      std::remove_if(RelocatedInstructionAddressOccurrences.begin(),
                     RelocatedInstructionAddressOccurrences.end(),
                     [](const RelocatedInstructionAddressOccurrence &Item) {
                       return Item.DefinesOutput;
                     }),
      RelocatedInstructionAddressOccurrences.end());

  struct AuthenticatedPageDefinition {
    va_t TargetVA = InvalidVA;
    va_t TargetOwnerVA = InvalidVA;
    JumpTableValueOccurrence Occurrence;
  };
  std::vector<AuthenticatedPageDefinition> PageDefinitions;
  for (const auto &[Addr, Rec] : Insns) {
    const auto Materialized = Img.InstructionPageAddressFragments.find(Addr);
    if (Materialized == Img.InstructionPageAddressFragments.end() ||
        Materialized->second.TargetOwnerVA == InvalidVA ||
        !Img.relocatedTargetBelongsToOwner(Materialized->second.TargetVA,
                                           Materialized->second.TargetOwnerVA,
                                           ExecutableCodeOwners) ||
        Rec.Ops.size() != 1)
      continue;
    const LowOp &Op = Rec.Ops.front();
    const va_t PageBase = Materialized->second.TargetVA & ~AArch64PageMask;
    if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 || !Op.Output.isReg() ||
        Op.Output.Size != Img.getPointerSize() || !Op.Inputs[0].isConst() ||
        (Op.Inputs[0].Provenance !=
             ConstantAddressProvenance::AddressFragment &&
         Op.Inputs[0].Provenance != ConstantAddressProvenance::DataAddress) ||
        Op.Inputs[0].Offset != PageBase)
      continue;
    PageDefinitions.push_back(
        {Materialized->second.TargetVA,
         Materialized->second.TargetOwnerVA,
         {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true}});
  }

  struct PendingMaterialization {
    const LowOp *Op = nullptr;
    RelocatedInstructionAddressMaterialization Materialized;
    size_t ExactQuery = std::numeric_limits<size_t>::max();
    size_t MayQuery = 0;
  };
  std::vector<JumpTableValueQuery> Queries;
  std::vector<PendingMaterialization> Pending;
  for (const auto &[Addr, Rec] : Insns) {
    const auto Materialized = Img.InstructionAddressMaterializations.find(Addr);
    if (Materialized == Img.InstructionAddressMaterializations.end() ||
        Materialized->second.TargetOwnerVA == InvalidVA ||
        !Img.relocatedTargetBelongsToOwner(Materialized->second.TargetVA,
                                           Materialized->second.TargetOwnerVA,
                                           ExecutableCodeOwners) ||
        Rec.IsInstructionGuard)
      continue;
    const va_t PageBase = Materialized->second.TargetVA & ~AArch64PageMask;
    const uint64_t PageOffset = Materialized->second.TargetVA & AArch64PageMask;
    for (const LowOp &Op : Rec.Ops) {
      if (Op.Opcode != NdOp::INT_ADD || Op.NumInputs != 2 ||
          !Op.Output.isReg() || Op.Output.Size != Img.getPointerSize())
        continue;
      int BaseSide = -1;
      for (int Side = 0; Side < 2; ++Side) {
        const NdVar &Immediate = Op.Inputs[1 - Side];
        if ((!Op.Inputs[Side].isReg() && !Op.Inputs[Side].isTemp()) ||
            Op.Inputs[Side].Size != Img.getPointerSize() ||
            !Immediate.isConst() ||
            Immediate.Provenance != ConstantAddressProvenance::Scalar ||
            Immediate.Offset != PageOffset)
          continue;
        if (BaseSide != -1) {
          BaseSide = -2;
          break;
        }
        BaseSide = Side;
      }
      if (BaseSide < 0)
        continue;

      std::vector<JumpTableValueOccurrence> ExactAlternatives;
      std::vector<JumpTableValueOccurrence> AllPageAlternatives;
      for (const AuthenticatedPageDefinition &Page : PageDefinitions)
        if (Page.Occurrence.Value.Size == Op.Inputs[BaseSide].Size) {
          AllPageAlternatives.push_back(Page.Occurrence);
          if ((Page.TargetVA & ~AArch64PageMask) == PageBase)
            ExactAlternatives.push_back(Page.Occurrence);
        }
      if (AllPageAlternatives.empty())
        continue;
      size_t ExactQuery = std::numeric_limits<size_t>::max();
      if (!ExactAlternatives.empty()) {
        ExactQuery = Queries.size();
        Queries.push_back({Op.Inputs[BaseSide], Op.Addr, Op.Seq,
                           std::move(ExactAlternatives), false, false});
      }
      const size_t MayQuery = Queries.size();
      JumpTableValueQuery MayDepend{Op.Inputs[BaseSide],
                                    Op.Addr,
                                    Op.Seq,
                                    std::move(AllPageAlternatives),
                                    false,
                                    false};
      MayDepend.Relation = JumpTableValueRelation::MayDepend;
      Queries.push_back(std::move(MayDepend));
      Pending.push_back({&Op, Materialized->second, ExactQuery, MayQuery});
    }
  }

  if (!Queries.empty()) {
    bool AnalysisComplete = false;
    const std::vector<bool> Matches =
        tableValuesMatchAtUses(Queries, &AnalysisComplete);
    const bool Complete = AnalysisComplete && Matches.size() == Queries.size();
    for (const PendingMaterialization &Candidate : Pending) {
      if (!Candidate.Op)
        continue;
      const bool HasExactQuery =
          Candidate.ExactQuery != std::numeric_limits<size_t>::max();
      const bool IsExact = Complete && HasExactQuery &&
                           Candidate.ExactQuery < Matches.size() &&
                           Matches[Candidate.ExactQuery];
      const bool MayDepend = Complete && Candidate.MayQuery < Matches.size() &&
                             Matches[Candidate.MayQuery];
      // A complete query that proves no authenticated page reaches this ADD is
      // not address evidence: an unrelated/sibling ADRP must not poison module
      // arbitration.  An incomplete proof is different — the PAGEOFF output
      // may still carry a reachable page definition, so publish an explicit
      // partial certificate and let module arbitration fail closed.
      if (Complete && !IsExact && !MayDepend)
        continue;
      const LowOp &Op = *Candidate.Op;
      const RelocatedInstructionAddressMaterialization &Materialized =
          Candidate.Materialized;
      RelocatedInstructionAddressOccurrence Occurrence;
      Occurrence.FieldVA = Op.Addr;
      Occurrence.InstructionAddr = Op.Addr;
      Occurrence.OpSeq = Op.Seq;
      Occurrence.TargetVA = Materialized.TargetVA;
      Occurrence.TargetOwnerVA = Materialized.TargetOwnerVA;
      Occurrence.Width = 4;
      Occurrence.Provenance = Img.hasExecutableCodeOwnerAt(
                                  Materialized.TargetVA, ExecutableCodeOwners)
                                  ? ConstantAddressProvenance::CodeAddress
                                  : ConstantAddressProvenance::DataAddress;
      Occurrence.DefinesOutput = true;
      Occurrence.OutputMayDepend = !IsExact;
      Occurrence.OutputOpcode = Op.Opcode;
      Occurrence.OutputWitness = Op.Output;
      if (!llvm::is_contained(RelocatedInstructionAddressOccurrences,
                              Occurrence))
        RelocatedInstructionAddressOccurrences.push_back(std::move(Occurrence));
    }
  }

  // A linked AArch64 image may no longer carry PAGE/PAGEOFF relocation
  // records even though the decoded ADRP/add pair still computes an exact
  // image address.  Do not turn numeric foldability into provenance.  The
  // fallback below publishes only a fully occurrence-bound chain whose final
  // full-width scalar result is immediately dereferenced in one immutable
  // object-data owner.  Relocatable objects and any instruction with loader
  // materialization authority remain exclusively on the path above.
  if (Img.IsRelocatable || Img.getPointerSize() == 0 ||
      Img.getPointerSize() > sizeof(va_t))
    return;

  const uint16_t PointerSize = Img.getPointerSize();
  const unsigned PointerBits = static_cast<unsigned>(PointerSize) * 8;
  const uint64_t PointerMask = PointerBits == 64
                                   ? std::numeric_limits<uint64_t>::max()
                                   : (uint64_t{1} << PointerBits) - 1;
  auto canonicalArithmeticScalar =
      [&](const LowOp &Op, int BaseSide) -> std::optional<uint64_t> {
    if (BaseSide < 0 || BaseSide > 1 || !Op.Inputs[1 - BaseSide].isConst() ||
        Op.Inputs[1 - BaseSide].Provenance != ConstantAddressProvenance::Scalar)
      return std::nullopt;
    const NdVar &Scalar = Op.Inputs[1 - BaseSide];
    if (Scalar.Size == PointerSize)
      return Scalar.Offset & PointerMask;

    // AArch64Lifter intentionally retains a compact 32-bit NdVar for an
    // encoded ADD/SUB immediate whose value fits in uint32_t; INT_ADD/SUB then
    // applies that non-negative displacement in the destination width.  Admit
    // this mixed-width form only when the exact machine word independently
    // proves a 64-bit, non-flag-setting immediate instruction and its imm12 +
    // optional lsl #12 value is exactly the scalar carried by this LowOp.
    if (PointerSize != 8 || Scalar.Size != 4 || BaseSide != 0)
      return std::nullopt;
    const uint8_t *Bytes = Img.readVA(Op.Addr, sizeof(uint32_t));
    if (!Bytes)
      return std::nullopt;
    const uint32_t Word = readLE<uint32_t>(Bytes);
    if ((Word & 0x1f000000u) != 0x11000000u || (Word & 0x80000000u) == 0 ||
        (Word & 0x20000000u) != 0 ||
        (((Word & 0x40000000u) != 0) != (Op.Opcode == NdOp::INT_SUB)))
      return std::nullopt;
    const uint64_t Encoded = uint64_t((Word >> 10) & 0xfffu)
                             << (((Word >> 22) & 1u) ? 12 : 0);
    if (Encoded != Scalar.Offset)
      return std::nullopt;
    return Encoded;
  };
  auto checkedArithmetic = [&](va_t Base, const LowOp &Op,
                               int BaseSide) -> std::optional<va_t> {
    if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
        Op.NumInputs != 2 || BaseSide < 0 || BaseSide > 1 ||
        (Op.Opcode == NdOp::INT_SUB && BaseSide != 0) ||
        Op.Output.Size != PointerSize ||
        Op.Inputs[BaseSide].Size != PointerSize || Base > PointerMask)
      return std::nullopt;
    const std::optional<uint64_t> Delta =
        canonicalArithmeticScalar(Op, BaseSide);
    if (!Delta)
      return std::nullopt;
    if (Op.Opcode == NdOp::INT_ADD) {
      if (*Delta > PointerMask - Base)
        return std::nullopt;
      return Base + *Delta;
    }
    if (*Delta > Base)
      return std::nullopt;
    return Base - *Delta;
  };

  auto uniqueImmutableDataOwner =
      [&](va_t Address, uint16_t AccessSize) -> std::optional<va_t> {
    if (AccessSize == 0 || AccessSize - 1 > InvalidVA - Address)
      return std::nullopt;
    const va_t Last = Address + AccessSize - 1;
    if (!Img.hasObjectDataProvenance(Address) ||
        !Img.hasObjectDataProvenance(Last) ||
        Img.hasExecutableCodeOwnerAt(Address, ExecutableCodeOwners) ||
        Img.hasExecutableCodeOwnerAt(Last, ExecutableCodeOwners) ||
        isRuntimeWritableAddress(Img, Address) ||
        isRuntimeWritableAddress(Img, Last))
      return std::nullopt;

    const Section *OnlySection = nullptr;
    for (const Section &Section : Img.Sections) {
      if (!Section.isReadable() || Section.isExecutable() ||
          !Section.contains(Address) || !Section.contains(Last))
        continue;
      if (OnlySection)
        return std::nullopt;
      OnlySection = &Section;
    }
    if (OnlySection)
      return OnlySection->VA;

    const Segment *OnlySegment = nullptr;
    for (const Segment &Segment : Img.Segments) {
      if (!Segment.isReadable() || Segment.isExecutable() ||
          !Segment.contains(Address) || !Segment.contains(Last) ||
          Address < Segment.VA || Last < Segment.VA ||
          Last - Segment.VA >= Segment.Data.size())
        continue;
      if (OnlySegment)
        return std::nullopt;
      OnlySegment = &Segment;
    }
    return OnlySegment ? std::optional<va_t>(OnlySegment->VA) : std::nullopt;
  };

  struct RelocationFreeCandidate {
    RelocatedInstructionAddressOccurrence Occurrence;
    std::vector<size_t> QueryIndices;
  };
  std::vector<JumpTableValueQuery> RelocationFreeQueries;
  std::vector<RelocationFreeCandidate> RelocationFreeCandidates;
  for (auto RootIt = Insns.begin(); RootIt != Insns.end(); ++RootIt) {
    const InsnRecord &RootRec = RootIt->second;
    if (RootRec.IsInstructionGuard || RootRec.IsBranch || RootRec.IsCall ||
        RootRec.IsRet || RootRec.IsOpaqueTerminator ||
        RootRec.IsResumableTerminator || RootRec.Size == 0 ||
        RootRec.Size > InvalidVA - RootRec.Addr || RootRec.Ops.size() != 1)
      continue;
    const LowOp &Root = RootRec.Ops.front();
    if (Root.Opcode != NdOp::COPY || Root.NumInputs != 1 ||
        !Root.Output.isReg() || Root.Output.Size != PointerSize ||
        !Root.Inputs[0].isConst() || Root.Inputs[0].Size != PointerSize ||
        Root.Inputs[0].Provenance !=
            ConstantAddressProvenance::AddressFragment ||
        Root.Inputs[0].Offset > PointerMask ||
        (Root.Inputs[0].Offset & AArch64PageMask) != 0)
      continue;

    NdVar CurrentValue = Root.Output;
    va_t CurrentAddress = Root.Inputs[0].Offset;
    JumpTableValueOccurrence CurrentDefinition{Root.Output, Root.Addr, Root.Seq,
                                               /*DefinedAtPoint=*/true};
    std::vector<RelocatedInstructionAddressArithmeticStep> Steps;
    std::vector<JumpTableValueQuery> CandidateQueries;
    va_t ExpectedAddress = RootRec.Addr + RootRec.Size;
    auto publishCandidate = [&](va_t Owner, const LowOp &Dereference,
                                const NdVar &MemoryAddress, uint16_t Bytes) {
      RelocationFreeCandidate Candidate;
      for (JumpTableValueQuery &Query : CandidateQueries) {
        Candidate.QueryIndices.push_back(RelocationFreeQueries.size());
        RelocationFreeQueries.push_back(std::move(Query));
      }
      auto &Occurrence = Candidate.Occurrence;
      Occurrence.FieldVA = InvalidVA;
      Occurrence.InstructionAddr = Steps.back().InstructionAddr;
      Occurrence.OpSeq = Steps.back().OpSeq;
      Occurrence.TargetVA = CurrentAddress;
      Occurrence.TargetOwnerVA = Owner;
      Occurrence.Width = static_cast<uint8_t>(PointerSize);
      Occurrence.Provenance = ConstantAddressProvenance::DataAddress;
      Occurrence.DefinesOutput = true;
      Occurrence.OutputOpcode = Steps.back().Opcode;
      Occurrence.OutputWitness = Steps.back().OutputWitness;
      Occurrence.Authority = RelocatedInstructionAddressProofKind::
          AArch64RelocationFreeDataDereference;
      Occurrence.SeedInstructionAddr = Root.Addr;
      Occurrence.SeedOpSeq = Root.Seq;
      Occurrence.SeedOpcode = Root.Opcode;
      Occurrence.SeedInputWitness = Root.Inputs[0];
      Occurrence.SeedOutputWitness = Root.Output;
      Occurrence.ArithmeticProof = Steps;
      Occurrence.DereferenceInstructionAddr = Dereference.Addr;
      Occurrence.DereferenceOpSeq = Dereference.Seq;
      Occurrence.DereferenceOpcode = Dereference.Opcode;
      Occurrence.DereferenceAddressWitness = MemoryAddress;
      Occurrence.DereferenceAccessSize = Bytes;
      RelocationFreeCandidates.push_back(std::move(Candidate));
    };

    for (auto UseIt = std::next(RootIt);
         UseIt != Insns.end() &&
         Steps.size() <
             ::neverd::limits::ir::kCFGBuilderMaxLookaheadInstructions;
         ++UseIt) {
      const InsnRecord &UseRec = UseIt->second;
      if (UseRec.Addr != ExpectedAddress || UseRec.Size == 0 ||
          UseRec.Size > InvalidVA - UseRec.Addr || UseRec.IsInstructionGuard ||
          (UseRec.IsBranch && !UseRec.IsCall) || UseRec.IsRet ||
          UseRec.IsOpaqueTerminator || UseRec.IsResumableTerminator)
        break;
      if (UseRec.IsCall) {
        if (Steps.empty())
          break;
        const LowOp *Call = nullptr;
        for (const LowOp &Op : UseRec.Ops)
          if (Op.Opcode == NdOp::CALL)
            Call = &Op;
        const auto Read =
            Call ? importedMemoryCopyRead(Img, Insns, *Call) : std::nullopt;
        if (!Read || Read->Source != CurrentValue)
          break;
        const auto Owner = uniqueImmutableDataOwner(CurrentAddress, Read->Size);
        if (!Owner || !Img.relocatedTargetBelongsToOwner(CurrentAddress, *Owner,
                                                         ExecutableCodeOwners))
          break;
        CandidateQueries.push_back({Read->Source,
                                    Call->Addr,
                                    Call->Seq,
                                    {CurrentDefinition},
                                    false,
                                    false});
        CandidateQueries.push_back({Read->Length,
                                    Call->Addr,
                                    Call->Seq,
                                    {{Read->Length, Read->LengthInstructionAddr,
                                      Read->LengthOpSeq, true}},
                                    false,
                                    false});
        publishCandidate(*Owner, *Call, Read->Source, Read->Size);
        break;
      }

      // AArch64 unsigned-offset LDR/STR folds the address calculation into the
      // memory instruction.  Its LowIR is instruction-local rather than three
      // records:
      //
      //   COPY    tmp, page-reg
      //   INT_ADD tmp, tmp, #offset
      //   LOAD    value, tmp
      //
      // The relocation-free proof still owns the exact ADD output occurrence;
      // the immediately following memory op is its dereference witness.  Keep
      // this narrow shape separate from the ordinary cross-instruction walk:
      // every transport and the sole memory effect must occur in this exact
      // InsnRecord, and the usual all-path value queries remain authoritative.
      const LowOp *LocalArithmetic = nullptr;
      const LowOp *LocalDereference = nullptr;
      LowImageBaseMemoryWitness LocalDereferenceMemory;
      NdVar LocalDereferenceAddress;
      int LocalBaseSide = -1;
      NdVar LocalAddressValue = CurrentValue;
      bool InvalidLocalShape = false;
      for (const LowOp &Op : UseRec.Ops) {
        const auto Memory = lowImageBaseMemoryWitness(Img, Op, CurrentAddress);
        if (!LocalArithmetic) {
          if (Memory.Complete) {
            InvalidLocalShape = true;
            break;
          }
          int CandidateBase = -1;
          if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
              Op.NumInputs == 2 && (Op.Output.isReg() || Op.Output.isTemp()) &&
              Op.Output.Size == PointerSize) {
            const bool LeftAlias = Op.Inputs[0] == LocalAddressValue;
            const bool RightAlias = Op.Inputs[1] == LocalAddressValue;
            if (LeftAlias && Op.Inputs[1].isConst())
              CandidateBase = 0;
            if (Op.Opcode == NdOp::INT_ADD && RightAlias &&
                Op.Inputs[0].isConst()) {
              if (CandidateBase >= 0)
                CandidateBase = -2;
              else
                CandidateBase = 1;
            }
          }
          if (CandidateBase >= 0 &&
              checkedArithmetic(CurrentAddress, Op, CandidateBase)) {
            LocalArithmetic = &Op;
            LocalBaseSide = CandidateBase;
            LocalAddressValue = Op.Output;
            continue;
          }
          if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
              Op.Inputs[0] == LocalAddressValue &&
              (Op.Output.isReg() || Op.Output.isTemp()) &&
              Op.Output.Size == PointerSize) {
            LocalAddressValue = Op.Output;
            continue;
          }
          if (Op.Output == LocalAddressValue ||
              overlapsRegister(Op.Output, LocalAddressValue)) {
            InvalidLocalShape = true;
            break;
          }
          continue;
        }

        if (Memory.Complete) {
          if (LocalDereference ||
              (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE &&
               !lowAArch64AddressMemoryShapeIsValid(Op) &&
               !lowAArch64WritebackMemoryShapeIsValid(Op)) ||
              !Memory.Base || *Memory.Base != LocalAddressValue ||
              Op.Output == LocalAddressValue ||
              overlapsRegister(Op.Output, LocalAddressValue)) {
            InvalidLocalShape = true;
            break;
          }
          LocalDereference = &Op;
          LocalDereferenceMemory = Memory;
          LocalDereferenceAddress = *Memory.Base;
          continue;
        }
        if (!LocalDereference && Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0] == LocalAddressValue &&
            (Op.Output.isReg() || Op.Output.isTemp()) &&
            Op.Output.Size == PointerSize) {
          LocalAddressValue = Op.Output;
          continue;
        }
        if (lowImageBaseMemoryWritebackCommit(Op, LocalDereference, UseRec.Ops))
          continue;
        if (!LocalDereference || Op.Output == LocalAddressValue ||
            overlapsRegister(Op.Output, LocalAddressValue)) {
          InvalidLocalShape = true;
          break;
        }
      }

      if (!InvalidLocalShape && LocalArithmetic && LocalBaseSide >= 0 &&
          LocalDereference && LocalDereferenceMemory.AccessSize != 0 &&
          !Img.InstructionAddressMaterializations.count(
              LocalArithmetic->Addr)) {
        const std::optional<va_t> NextAddress =
            checkedArithmetic(CurrentAddress, *LocalArithmetic, LocalBaseSide);
        const std::optional<va_t> Owner =
            NextAddress ? uniqueImmutableDataOwner(
                              *NextAddress, LocalDereferenceMemory.AccessSize)
                        : std::nullopt;
        if (!NextAddress || !Owner ||
            !Img.relocatedTargetBelongsToOwner(*NextAddress, *Owner,
                                               ExecutableCodeOwners))
          break;

        CandidateQueries.push_back({LocalArithmetic->Inputs[LocalBaseSide],
                                    LocalArithmetic->Addr,
                                    LocalArithmetic->Seq,
                                    {CurrentDefinition},
                                    /*AllowZeroExtension=*/false,
                                    /*AllowSignExtension=*/false});
        Steps.push_back({LocalArithmetic->Addr, LocalArithmetic->Seq,
                         LocalArithmetic->Opcode,
                         static_cast<uint8_t>(LocalBaseSide),
                         LocalArithmetic->Inputs[LocalBaseSide],
                         LocalArithmetic->Inputs[1 - LocalBaseSide],
                         LocalArithmetic->Output});
        CurrentValue = LocalArithmetic->Output;
        CurrentAddress = *NextAddress;
        CurrentDefinition = {LocalArithmetic->Output, LocalArithmetic->Addr,
                             LocalArithmetic->Seq,
                             /*DefinedAtPoint=*/true};
        CandidateQueries.push_back({LocalDereferenceAddress,
                                    LocalDereference->Addr,
                                    LocalDereference->Seq,
                                    {CurrentDefinition},
                                    /*AllowZeroExtension=*/false,
                                    /*AllowSignExtension=*/false});

        publishCandidate(*Owner, *LocalDereference, LocalDereferenceAddress,
                         LocalDereferenceMemory.AccessSize);
        break;
      }

      const LowOp *Arithmetic = nullptr;
      int BaseSide = -1;
      for (const LowOp &Op : UseRec.Ops) {
        if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
            Op.NumInputs != 2 || !Op.Output.isReg() ||
            Op.Output.Size != PointerSize)
          continue;
        int CandidateBase = -1;
        if (Op.Inputs[0] == CurrentValue && Op.Inputs[1].isConst())
          CandidateBase = 0;
        if (Op.Opcode == NdOp::INT_ADD && Op.Inputs[1] == CurrentValue &&
            Op.Inputs[0].isConst()) {
          if (CandidateBase >= 0) {
            CandidateBase = -2;
          } else {
            CandidateBase = 1;
          }
        }
        if (CandidateBase < 0 ||
            !checkedArithmetic(CurrentAddress, Op, CandidateBase))
          continue;
        if (Arithmetic) {
          Arithmetic = nullptr;
          BaseSide = -2;
          break;
        }
        Arithmetic = &Op;
        BaseSide = CandidateBase;
      }

      if (Arithmetic && BaseSide >= 0) {
        bool InvalidStep = false;
        for (const LowOp &Op : UseRec.Ops) {
          const auto Memory =
              lowImageBaseMemoryWitness(Img, Op, CurrentAddress);
          if (Memory.Complete ||
              (&Op != Arithmetic &&
               (overlapsRegister(Op.Output, CurrentValue) ||
                overlapsRegister(Op.Output, Arithmetic->Output)))) {
            InvalidStep = true;
            break;
          }
        }
        if (InvalidStep ||
            Img.InstructionAddressMaterializations.count(Arithmetic->Addr))
          break;
        const std::optional<va_t> NextAddress =
            checkedArithmetic(CurrentAddress, *Arithmetic, BaseSide);
        if (!NextAddress)
          break;
        CandidateQueries.push_back({Arithmetic->Inputs[BaseSide],
                                    Arithmetic->Addr,
                                    Arithmetic->Seq,
                                    {CurrentDefinition},
                                    /*AllowZeroExtension=*/false,
                                    /*AllowSignExtension=*/false});
        Steps.push_back({Arithmetic->Addr, Arithmetic->Seq, Arithmetic->Opcode,
                         static_cast<uint8_t>(BaseSide),
                         Arithmetic->Inputs[BaseSide],
                         Arithmetic->Inputs[1 - BaseSide], Arithmetic->Output});
        CurrentValue = Arithmetic->Output;
        CurrentAddress = *NextAddress;
        CurrentDefinition = {Arithmetic->Output, Arithmetic->Addr,
                             Arithmetic->Seq, /*DefinedAtPoint=*/true};
        ExpectedAddress = UseRec.Addr + UseRec.Size;
        continue;
      }

      if (Steps.empty())
        break;
      const LowOp *Dereference = nullptr;
      LowImageBaseMemoryWitness DereferenceMemory;
      NdVar DereferenceAddress = CurrentValue;
      bool InvalidDereference = false;
      for (const LowOp &Op : UseRec.Ops) {
        const auto Memory = lowImageBaseMemoryWitness(Img, Op, CurrentAddress);
        if (Memory.Complete) {
          if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE &&
               !lowAArch64AddressMemoryShapeIsValid(Op) &&
               !lowAArch64WritebackMemoryShapeIsValid(Op)) ||
              !Memory.Base || *Memory.Base != DereferenceAddress ||
              Dereference) {
            InvalidDereference = true;
            break;
          }
          Dereference = &Op;
          DereferenceMemory = Memory;
          if (overlapsRegister(Op.Output, CurrentValue)) {
            InvalidDereference = true;
            break;
          }
          continue;
        }
        if (!Dereference) {
          if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 ||
              Op.Inputs[0] != DereferenceAddress ||
              (!Op.Output.isReg() && !Op.Output.isTemp()) ||
              Op.Output.Size != PointerSize ||
              (Op.Output.isReg() &&
               overlapsRegister(Op.Output, CurrentValue))) {
            InvalidDereference = true;
            break;
          }
          DereferenceAddress = Op.Output;
        }
        if (lowImageBaseMemoryWritebackCommit(Op, Dereference, UseRec.Ops))
          continue;
        if (overlapsRegister(Op.Output, CurrentValue)) {
          InvalidDereference = true;
          break;
        }
      }
      if (InvalidDereference || !Dereference ||
          DereferenceMemory.AccessSize == 0)
        break;
      const std::optional<va_t> Owner = uniqueImmutableDataOwner(
          CurrentAddress, DereferenceMemory.AccessSize);
      if (!Owner || !Img.relocatedTargetBelongsToOwner(CurrentAddress, *Owner,
                                                       ExecutableCodeOwners))
        break;

      CandidateQueries.push_back({DereferenceAddress,
                                  Dereference->Addr,
                                  Dereference->Seq,
                                  {CurrentDefinition},
                                  /*AllowZeroExtension=*/false,
                                  /*AllowSignExtension=*/false});
      publishCandidate(*Owner, *Dereference, DereferenceAddress,
                       DereferenceMemory.AccessSize);
      break;
    }
  }

  if (RelocationFreeQueries.empty())
    return;
  bool RelocationFreeAnalysisComplete = false;
  const std::vector<bool> RelocationFreeMatches = tableValuesMatchAtUses(
      RelocationFreeQueries, &RelocationFreeAnalysisComplete);
  if (!RelocationFreeAnalysisComplete ||
      RelocationFreeMatches.size() != RelocationFreeQueries.size())
    return;
  for (RelocationFreeCandidate &Candidate : RelocationFreeCandidates) {
    if (!std::all_of(Candidate.QueryIndices.begin(),
                     Candidate.QueryIndices.end(), [&](size_t Index) {
                       return Index < RelocationFreeMatches.size() &&
                              RelocationFreeMatches[Index];
                     }))
      continue;
    if (!llvm::is_contained(RelocatedInstructionAddressOccurrences,
                            Candidate.Occurrence))
      RelocatedInstructionAddressOccurrences.push_back(
          std::move(Candidate.Occurrence));
  }
}

void CFGBuilder::completeExactARMRelativeLiteralAddresses(
    const BinaryImage &Img) {
  if (Img.Arch != Arch::ARM || !Img.isELF() || !JumpTableProofContextComplete ||
      Img.getPointerSize() != 4)
    return;

  // Output certificates are rebuilt from the current final proof graph.  One
  // from an earlier provisional CFG must not survive a later batch whose
  // analysis is incomplete or that sees a new predecessor of the PC ADD.
  RelocatedInstructionAddressOccurrences.erase(
      std::remove_if(RelocatedInstructionAddressOccurrences.begin(),
                     RelocatedInstructionAddressOccurrences.end(),
                     [](const RelocatedInstructionAddressOccurrence &Item) {
                       return Item.DefinesOutput;
                     }),
      RelocatedInstructionAddressOccurrences.end());

  struct LiteralDefinition {
    va_t SlotVA = InvalidVA;
    va_t TargetVA = InvalidVA;
    va_t TargetOwnerVA = InvalidVA;
    uint32_t Encoded = 0;
    JumpTableValueOccurrence Occurrence;
    /// No relocation writes the literal: its target is known only at the ADD
    /// that adds the PC, and must lie in that ADD's own section.
    bool RelocationFree = false;
    /// A relocation-free literal of a linked image, whose sections keep the
    /// distances the static linker resolved it against: the target may lie in
    /// any section, which then owns it.
    bool Linked = false;
  };
  std::vector<LiteralDefinition> Literals;

  // Resolve only instruction-local literal effective addresses.  This is not
  // a CFG proof: it authenticates which exact LOAD occurrence read the
  // relocation field.  The later batch query proves that LOAD's output reaches
  // the ADD base on every feasible incoming path.
  for (const auto &[Addr, Rec] : Insns) {
    (void)Addr;
    std::vector<std::pair<NdVar, uint32_t>> Known;
    auto knownValue = [&](const NdVar &Value) -> std::optional<uint32_t> {
      if (Value.isConst())
        return static_cast<uint32_t>(Value.Offset);
      for (auto It = Known.rbegin(); It != Known.rend(); ++It)
        if (It->first == Value)
          return It->second;
      return std::nullopt;
    };
    for (const LowOp &Op : Rec.Ops) {
      if (Op.Opcode == NdOp::LOAD) {
        const LowMemoryOperandView Memory = lowMemoryOperands(Op);
        if (Memory.Complete && Memory.Address) {
          const std::optional<uint32_t> Slot = knownValue(*Memory.Address);
          const auto Applied = Slot ? Img.ARMRelativeLiteralFields.find(*Slot)
                                    : Img.ARMRelativeLiteralFields.end();
          const Segment *SlotSegment =
              Slot ? Img.getSegmentFor(*Slot) : nullptr;
          const uint8_t *Bytes =
              Slot ? Img.readVA(*Slot, sizeof(uint32_t)) : nullptr;
          if (Applied != Img.ARMRelativeLiteralFields.end() &&
              Applied->second.TargetVA != InvalidVA &&
              Applied->second.TargetOwnerVA != InvalidVA && SlotSegment &&
              SlotSegment->isReadable() && !SlotSegment->isWritable() &&
              Bytes && Op.Output.Size == 4) {
            const AppliedARMRelativeLiteral &Field = Applied->second;
            if (readLE<uint32_t>(Bytes) == Field.EncodedValue &&
                Img.relocatedTargetBelongsToOwner(
                    Field.TargetVA, Field.TargetOwnerVA, ExecutableCodeOwners))
              Literals.push_back(
                  {*Slot,
                   Field.TargetVA,
                   Field.TargetOwnerVA,
                   Field.EncodedValue,
                   {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true}});
          } else if (Applied == Img.ARMRelativeLiteralFields.end() && Slot &&
                     SlotSegment && SlotSegment->isReadable() &&
                     !SlotSegment->isWritable() && Bytes &&
                     Op.Output.Size == 4 &&
                     Img.isUnrelocatedObjectField(*Slot, sizeof(uint32_t)) &&
                     Img.isUnrelocatedObjectField(Rec.Addr, Rec.Size)) {
            // The assembler resolved this literal itself, so the PC-relative
            // load and the word it reads are both its final values.
            Literals.push_back(
                {*Slot,
                 InvalidVA,
                 InvalidVA,
                 readLE<uint32_t>(Bytes),
                 {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true},
                 /*RelocationFree=*/true});
          } else if (Applied == Img.ARMRelativeLiteralFields.end() && Slot &&
                     SlotSegment && SlotSegment->isReadable() &&
                     !SlotSegment->isWritable() && Bytes &&
                     Op.Output.Size == 4 && !Img.IsRelocatable &&
                     !Img.hasRelocationProvenanceAt(*Slot)) {
            // The static linker resolved this literal, and no relocation
            // moves the word.
            Literals.push_back(
                {*Slot,
                 InvalidVA,
                 InvalidVA,
                 readLE<uint32_t>(Bytes),
                 {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true},
                 /*RelocationFree=*/true,
                 /*Linked=*/true});
          }
        }
      }

      std::optional<uint32_t> Result;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
        Result = knownValue(Op.Inputs[0]);
      } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2) {
        const auto Left = knownValue(Op.Inputs[0]);
        const auto Right = knownValue(Op.Inputs[1]);
        if (Left && Right)
          Result = static_cast<uint32_t>(*Left + *Right);
      } else if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2) {
        const auto Left = knownValue(Op.Inputs[0]);
        const auto Right = knownValue(Op.Inputs[1]);
        if (Left && Right)
          Result = static_cast<uint32_t>(*Left - *Right);
      }
      if (Result && Op.Output.Size == 4)
        Known.emplace_back(Op.Output, *Result);
    }
  }
  if (Literals.empty())
    return;

  struct PendingMaterialization {
    const LowOp *Op = nullptr;
    va_t TargetVA = InvalidVA;
    va_t TargetOwnerVA = InvalidVA;
    size_t ExactQuery = 0;
    std::vector<std::pair<const LiteralDefinition *, size_t>> Sources;
  };
  auto ProveLiterals = [&](bool RelocationFree) {
    std::vector<JumpTableValueQuery> Queries;
    std::vector<PendingMaterialization> Pending;
    for (const auto &[Addr, Rec] : Insns) {
      (void)Addr;
      if (Rec.IsInstructionGuard)
        continue;
      std::vector<std::pair<NdVar, uint32_t>> Known;
      auto knownValue = [&](const NdVar &Value) -> std::optional<uint32_t> {
        if (Value.isConst())
          return static_cast<uint32_t>(Value.Offset);
        for (auto It = Known.rbegin(); It != Known.rend(); ++It)
          if (It->first == Value)
            return It->second;
        return std::nullopt;
      };
      for (const LowOp &Op : Rec.Ops) {
        if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2 &&
            (Op.Output.isReg() || Op.Output.isTemp()) && Op.Output.Size == 4 &&
            isSingleInstructionMode(Rec.Mode)) {
          const uint32_t PC = armreg::pcReadValue(Op.Addr, Rec.Mode);
          int PCSide = -1;
          for (int Side = 0; Side < 2; ++Side) {
            const std::optional<uint32_t> Value = knownValue(Op.Inputs[Side]);
            if (!Value || *Value != PC)
              continue;
            if (PCSide != -1) {
              PCSide = -2;
              break;
            }
            PCSide = Side;
          }
          if (PCSide >= 0) {
            using TargetKey = std::pair<va_t, va_t>;
            std::map<TargetKey, std::vector<const LiteralDefinition *>> Groups;
            // An object's sections move independently, so an unrelocated
            // PC-relative difference can only name the ADD's own section.
            va_t AnchorOwner = InvalidVA;
            if (const Section *Anchor = Img.getSectionFor(Op.Addr);
                Anchor && Anchor->isExecutable() &&
                Img.isUnrelocatedObjectField(Rec.Addr, Rec.Size))
              AnchorOwner = Anchor->VA;
            for (const LiteralDefinition &Literal : Literals) {
              if (Literal.RelocationFree != RelocationFree ||
                  Literal.Occurrence.Value.Size != Op.Inputs[1 - PCSide].Size)
                continue;
              TargetKey Key{Literal.TargetVA, Literal.TargetOwnerVA};
              if (Literal.RelocationFree) {
                const va_t Target = static_cast<uint32_t>(PC + Literal.Encoded);
                Key = {Target, Literal.Linked ? Img.layoutAddressOwner(Target)
                                              : AnchorOwner};
                if (!Img.relocatedTargetBelongsToOwner(Key.first, Key.second,
                                                       ExecutableCodeOwners))
                  continue;
              } else if (static_cast<uint32_t>(PC + Literal.Encoded) !=
                         static_cast<uint32_t>(Literal.TargetVA)) {
                continue;
              }
              Groups[Key].push_back(&Literal);
            }
            for (auto &[Key, Sources] : Groups) {
              std::vector<JumpTableValueOccurrence> Alternatives;
              Alternatives.reserve(Sources.size());
              for (const LiteralDefinition *Source : Sources)
                Alternatives.push_back(Source->Occurrence);
              const size_t ExactQuery = Queries.size();
              Queries.push_back({Op.Inputs[1 - PCSide], Op.Addr, Op.Seq,
                                 Alternatives, false, false});
              PendingMaterialization Candidate{
                  &Op, Key.first, Key.second, ExactQuery, {}};
              Candidate.Sources.reserve(Sources.size());
              for (const LiteralDefinition *Source : Sources) {
                const size_t MayQuery = Queries.size();
                JumpTableValueQuery MayDepend{
                    Op.Inputs[1 - PCSide], Op.Addr, Op.Seq,
                    {Source->Occurrence},  false,   false};
                MayDepend.Relation = JumpTableValueRelation::MayDepend;
                Queries.push_back(std::move(MayDepend));
                Candidate.Sources.emplace_back(Source, MayQuery);
              }
              Pending.push_back(std::move(Candidate));
            }
          }
        }

        std::optional<uint32_t> Result;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
          Result = knownValue(Op.Inputs[0]);
        } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2) {
          const auto Left = knownValue(Op.Inputs[0]);
          const auto Right = knownValue(Op.Inputs[1]);
          if (Left && Right)
            Result = static_cast<uint32_t>(*Left + *Right);
        } else if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2) {
          const auto Left = knownValue(Op.Inputs[0]);
          const auto Right = knownValue(Op.Inputs[1]);
          if (Left && Right)
            Result = static_cast<uint32_t>(*Left - *Right);
        }
        if (Result && Op.Output.Size == 4)
          Known.emplace_back(Op.Output, *Result);
      }
    }
    if (Queries.empty())
      return;

    bool AnalysisComplete = false;
    const std::vector<bool> Matches =
        tableValuesMatchAtUses(Queries, &AnalysisComplete);
    if (!AnalysisComplete || Matches.size() != Queries.size())
      return;
    for (const PendingMaterialization &Candidate : Pending) {
      if (!Candidate.Op || Candidate.ExactQuery >= Matches.size())
        continue;
      const bool IsExact = Matches[Candidate.ExactQuery];
      for (const auto &[Source, MayQuery] : Candidate.Sources) {
        if (!Source || MayQuery >= Matches.size() || !Matches[MayQuery])
          continue;
        RelocatedInstructionAddressOccurrence Occurrence;
        if (Source->RelocationFree) {
          Occurrence.Authority =
              Source->Linked
                  ? RelocatedInstructionAddressProofKind::ARMLinkedLiteral
                  : RelocatedInstructionAddressProofKind::
                        ARMRelocationFreeLiteral;
          Occurrence.LiteralVA = Source->SlotVA;
          Occurrence.SeedInstructionAddr = Source->Occurrence.Addr;
          Occurrence.SeedOpSeq = Source->Occurrence.Seq;
          Occurrence.SeedOpcode = NdOp::LOAD;
          Occurrence.SeedOutputWitness = Source->Occurrence.Value;
        } else {
          Occurrence.FieldVA = Source->SlotVA;
        }
        Occurrence.InstructionAddr = Candidate.Op->Addr;
        Occurrence.OpSeq = Candidate.Op->Seq;
        Occurrence.TargetVA = Candidate.TargetVA;
        Occurrence.TargetOwnerVA = Candidate.TargetOwnerVA;
        Occurrence.Width = 4;
        Occurrence.Provenance = Img.hasExecutableCodeOwnerAt(
                                    Candidate.TargetVA, ExecutableCodeOwners)
                                    ? ConstantAddressProvenance::CodeAddress
                                    : ConstantAddressProvenance::DataAddress;
        Occurrence.DefinesOutput = true;
        Occurrence.OutputMayDepend = !IsExact;
        Occurrence.OutputOpcode = Candidate.Op->Opcode;
        Occurrence.OutputWitness = Candidate.Op->Output;
        if (!llvm::is_contained(RelocatedInstructionAddressOccurrences,
                                Occurrence))
          RelocatedInstructionAddressOccurrences.push_back(
              std::move(Occurrence));
      }
    }
  };
  // A relocation-free literal is far less selective than a loader field's
  // exact target, so it gets its own batch: its analysis budget can never
  // withdraw a loader field's proof.
  ProveLiterals(/*RelocationFree=*/false);
  ProveLiterals(/*RelocationFree=*/true);
}

//===----------------------------------------------------------------------===//
// multiStageResolve — retry unresolved INDIR_BR with more context
//===----------------------------------------------------------------------===//

void CFGBuilder::multiStageResolve(const BinaryImage &Img, Decoder &Dec,
                                   LowFunc &Func) {
  AmbiguousI386GOTPCBranches.clear();
  PendingAmbiguousI386GOTPCBranches.clear();
  PendingAmbiguousI386GOTPCKeys.clear();
  StageAmbiguousI386GOTPCBranches.clear();
  StageReplayedI386GOTPCKeys.clear();
  CurrentI386GOTOFFAmbiguityKeys.clear();
  for (va_t Addr : PreviouslyPublishedJumpTableBranches) {
    auto It = Insns.find(Addr);
    if (It != Insns.end() && It->second.IsBranch && It->second.IsIndirect &&
        !It->second.IsCall)
      EverPublishedJumpTableBranches.insert(Addr);
  }
  auto RememberPublishedJumpTables = [&]() {
    for (const auto &[Addr, Rec] : Insns)
      if (Rec.IsBranch && Rec.IsIndirect && !Rec.IsCall &&
          !Rec.JumpTableTargets.empty())
        EverPublishedJumpTableBranches.insert(Addr);
  };
  RememberPublishedJumpTables();

  // A stage is a deterministic function of the state it starts from: its
  // accounts and scratch inventories are reset below, and its caches answer
  // as a fresh computation would.  StageStart holds every other member a
  // stage reads or leaves behind, except the build-wide marker account, which
  // a stage only debits and reads only for exhaustion.  When a stage that
  // published no new graph starts where the previous one started, every
  // remaining retry repeats it and debits the same amount; if the account
  // covers all of those debits, stopping and charging them returns what the
  // bounded retries return.  One-shot testing injections make retries differ.
  struct StageStart {
    std::map<va_t, InsnRecord> Insns;
    std::set<va_t> BlockStarts;
    // explore() only adds to ExploredAddrs; equal sizes mean equal sets.
    size_t ExploredCount;
    std::vector<va_t> CallTargets;
    std::map<va_t, std::set<va_t>> OwnInteriorCallSites;
    std::vector<va_t> DiscoveredCodeRefs;
    std::map<va_t, std::set<va_t>> DiscoveredCodeRefSources;
    std::map<va_t, JumpTableInfo> ResolvedTableInfo;
    std::optional<GuardedJumpTableGroupProofContext> GroupProofContext;
    std::optional<GuardedJumpTableGroupKey> GroupIdentity;
    std::optional<GuardedJumpTableGroupState> GroupState;
    bool GroupRejected;
    std::map<va_t, StrongJumpTableRoleProposal> StrongProposals;
    std::map<va_t, ProvisionalRelativeEdgeProposal> ProvisionalEdges;
    std::set<va_t> Quarantined;
    std::set<va_t> EverStrongProposals;
    bool MarkerEvidenceIncomplete;
    bool ProofContextComplete;
    bool RequestedCompleteProof;
    std::set<va_t> PersistentRoots, OrdinaryRoots, DurableRoots;
    std::set<va_t> ExceptionalEntries;
    std::map<va_t, std::set<va_t>> RelocationRootSources;
    std::optional<std::set<va_t>> ActiveProofRoots;
    va_t ActiveCandidate;
    uint32_t ActiveProofRank, ActiveDependencyRank;
    bool ActiveConsumerAudit;
    std::set<va_t> PotentialBranches, EverPublishedBranches,
        LostValidatedBranches, StackIncompleteBranches, IndexIncompleteBranches,
        ValidatedPhysicalBranches;
    std::set<va_t> AmbiguousGOTPC, PendingAmbiguousGOTPC;
    std::set<I386GOTOFFAmbiguityReplayKey> PendingGOTPCKeys, CurrentGOTOFFKeys;
    std::set<va_t> PublishedReachable;
    std::vector<va_t> PublishedStarts;
    uint64_t Decoded, Lifted;
    std::set<va_t> DecodedAddrs, LiftedAddrs, DecodeFailures, Unsupported,
        Truncated;
    std::vector<RelocatedInstructionAddressOccurrence> AddressOccurrences;
    std::vector<I386GetPcOccurrence> GetPcOccurrences;
    std::vector<RelocatedInstructionScalarOperandOccurrence> ScalarOperands;
    std::vector<RelocatedInstructionScalarModelOccurrence> ScalarModels;
    size_t GOTOFFEvidenceRemaining;
    bool GOTOFFShapeClaimed, GOTOFFEvidenceIncomplete, GOTEvidenceIncomplete,
        GOTOFFAmbiguousReach;
    std::optional<std::pair<va_t, va_t>> FuncRange, AuthoritativeFuncRange;
    int FpuTop, RetPopBytes;
    std::vector<LowFunctionTemporary> Temporaries;

    bool operator==(const StageStart &Other) const = default;
  };
  const auto Sorted = [](const llvm::DenseSet<va_t> &Values) {
    std::vector<va_t> Result(Values.begin(), Values.end());
    llvm::sort(Result);
    return Result;
  };
  const auto CaptureStageStart = [&]() {
    return StageStart{Insns,
                      BlockStarts,
                      ExploredAddrs.size(),
                      Sorted(CallTargets),
                      OwnInteriorCallSites,
                      Sorted(DiscoveredCodeRefs),
                      DiscoveredCodeRefSources,
                      ResolvedTableInfo,
                      GuardedGroupProofContext,
                      GuardedGroupIdentity,
                      GuardedGroupState,
                      GuardedGroupRejected,
                      PriorStrongJumpTableProposals,
                      PriorProvisionalRelativeEdges,
                      QuarantinedJumpTableProposals,
                      EverStrongJumpTableProposalBranches,
                      IncompleteBranchMarkerEvidenceIncomplete,
                      JumpTableProofContextComplete,
                      RequestedCompleteJumpTableProof,
                      PersistentCFGRoots,
                      OrdinaryCFGRoots,
                      DurableCFGRoots,
                      CurrentExceptionalEntries,
                      RelocationCFGRootSources,
                      ActiveJumpTableProofRoots,
                      ActiveJumpTableCandidateAddr,
                      ActiveJumpTableCandidateProofRank,
                      ActiveJumpTableCandidateDependencyRank,
                      ActiveJumpTableConsumerAudit,
                      PotentialJumpTableBranches,
                      EverPublishedJumpTableBranches,
                      LostValidatedJumpTableBranches,
                      StackTableEvidenceIncompleteBranches,
                      IndexDomainEvidenceIncompleteBranches,
                      ValidatedPhysicalJumpTableBranches,
                      AmbiguousI386GOTPCBranches,
                      PendingAmbiguousI386GOTPCBranches,
                      PendingAmbiguousI386GOTPCKeys,
                      CurrentI386GOTOFFAmbiguityKeys,
                      PublishedReachableInsns,
                      PublishedBlockStarts,
                      DecodedInstructionCount,
                      LiftedInstructionCount,
                      DecodedInstructionAddresses,
                      LiftedInstructionAddresses,
                      DecodeFailureAddresses,
                      UnsupportedInstructionAddresses,
                      TruncatedPathAddresses,
                      RelocatedInstructionAddressOccurrences,
                      I386GetPcOccurrences,
                      RelocatedInstructionScalarOperandOccurrences,
                      RelocatedInstructionScalarModelOccurrences,
                      I386GOTOFFProposalEvidenceRemaining,
                      I386GOTOFFProposalShapeClaimed,
                      I386GOTOFFProposalEvidenceIncomplete,
                      I386GOTModelEvidenceIncomplete,
                      I386GOTOFFAmbiguousModelReach,
                      CurrentFuncRange,
                      AuthoritativeCurrentFuncRange,
                      Dec.getX86FpuTop(),
                      Dec.getX86RetPopBytes(),
                      Func.FunctionTemporaries};
  };
  const bool StageTestingInjections =
      I386GOTModelEvidenceBudgetForTesting ||
      I386GOTOFFProposalEvidenceBudgetForTesting ||
      IncompleteBranchMarkerEvidenceBudgetForTesting ||
      StackTableEvidenceBudgetForTesting ||
      MaskFixedPointEvidenceBudgetForTesting ||
      NestedMutationTrackingStageAllowanceForTesting ||
      FiniteSetSymbolEvidenceBudgetForTesting ||
      ExhaustUntrackedJumpTableCandidateForTesting ||
      ExhaustStableI386AmbiguityCommitTailForTesting ||
      ExhaustProposalStageCommitTailForTesting ||
      ExhaustPublishedJumpTableGroupCommitTailForTesting ||
      RevokePublishedJumpTableGroupOwnerForTesting ||
      ProposalOldStateCleanupEvidenceExhaustionForTesting;
  std::optional<StageStart> PreviousStart;
  size_t PreviousMarkerEvidence = 0;
  bool PreviousStageGrew = true;
  // The starts of the two stages before this one, from the third stage on,
  // and what the marker account held as each started.
  std::optional<StageStart> StartTwoBack, StartOneBack;
  size_t MarkerTwoBack = 0, MarkerOneBack = 0;
  // Once the stage that leaves the retries' last start runs: the debit of
  // the retries after it.
  std::optional<size_t> CycleDebit;

  bool ReachedFixedPoint = false;
  for (int Stage = 0; Stage < limits::kMaxMultiStageRetries; ++Stage) {
    if (CycleDebit) {
      IncompleteBranchMarkerEvidenceRemaining -= *CycleDebit;
      break;
    }
    // Only a stage after one that grew nothing can repeat it; capturing the
    // start of every other stage would cost a copy for nothing.
    if (PreviousStageGrew || StageTestingInjections) {
      PreviousStart.reset();
    } else {
      StageStart Start = CaptureStageStart();
      if (PreviousStart && *PreviousStart == Start &&
          PreviousMarkerEvidence >= IncompleteBranchMarkerEvidenceRemaining) {
        const size_t Debit =
            PreviousMarkerEvidence - IncompleteBranchMarkerEvidenceRemaining;
        const size_t Retries = limits::kMaxMultiStageRetries - Stage;
        if (!Debit ||
            IncompleteBranchMarkerEvidenceRemaining / Debit >= Retries) {
          IncompleteBranchMarkerEvidenceRemaining -= Debit * Retries;
          break;
        }
      }
      PreviousStart = std::move(Start);
      PreviousMarkerEvidence = IncompleteBranchMarkerEvidenceRemaining;
    }
    // Two stages can also alternate, as when one publishes a table that the
    // next withdraws.  A stage that starts where the stage before the
    // previous one started repeats the two since, and so does every retry
    // after it, debiting the account as they did.  The last retry leaves this
    // stage's start or the next one's, by parity.  If the account covers
    // every debit, stopping there and charging them returns what the bounded
    // retries return.  Few builds run three stages, so only theirs keep
    // starts.
    if (Stage >= 2 && !StageTestingInjections) {
      StageStart Start = CaptureStageStart();
      if (StartTwoBack && *StartTwoBack == Start &&
          MarkerTwoBack >= MarkerOneBack &&
          MarkerOneBack >= IncompleteBranchMarkerEvidenceRemaining) {
        // The retries left debit these two alternately, this one first.
        const size_t First = MarkerTwoBack - MarkerOneBack;
        const size_t Second =
            MarkerOneBack - IncompleteBranchMarkerEvidenceRemaining;
        const size_t Retries =
            static_cast<size_t>(limits::kMaxMultiStageRetries - Stage);
        const size_t Rest = llvm::SaturatingMultiply(
            llvm::SaturatingAdd(First, Second), Retries / 2);
        const size_t Debit =
            Retries % 2 ? llvm::SaturatingAdd(Rest, First) : Rest;
        if (Debit < std::numeric_limits<size_t>::max() &&
            IncompleteBranchMarkerEvidenceRemaining >= Debit) {
          if (Retries % 2 == 0) {
            IncompleteBranchMarkerEvidenceRemaining -= Debit;
            break;
          }
          // This stage leaves the next one's start, where the retries end.
          CycleDebit = Rest;
        }
      }
      StartTwoBack = std::move(StartOneBack);
      MarkerTwoBack = MarkerOneBack;
      StartOneBack = std::move(Start);
      MarkerOneBack = IncompleteBranchMarkerEvidenceRemaining;
    }
    PreviousStageGrew = false;
    // A joint proof has no individual member authority. Remove every live
    // member before rebuilding the next immutable graph. The cached complete
    // state is comparison-only, and publication prepaid this withdrawal.
    if (GuardedGroupState && GuardedGroupState->Published) {
      withdrawGuardedJumpTableGroup(/*Reject=*/false);
      rebuildBlocks(Func);
    }
    // Incomplete-evidence branch identity is persistent safety state.  Rebuild
    // it transactionally with the proposal graph: a stage may clear an old
    // marker only after every candidate and the proposal commit tail have
    // completed.  Moving the committed sets aside also avoids an unmetered
    // deep snapshot; the empty member sets are the stage-local shadows.
    std::set<va_t> SavedStackTableEvidenceIncompleteBranches;
    std::set<va_t> SavedIndexDomainEvidenceIncompleteBranches;
    SavedStackTableEvidenceIncompleteBranches.swap(
        StackTableEvidenceIncompleteBranches);
    SavedIndexDomainEvidenceIncompleteBranches.swap(
        IndexDomainEvidenceIncompleteBranches);
    bool IncompleteBranchMarkerStageActive = true;
    bool IncompleteBranchMarkerCleanupReserved = false;
    auto RestoreIncompleteBranchMarkers = [&]() {
      if (!IncompleteBranchMarkerStageActive)
        return;
      // The initial inventory reserve can fail before any candidate has had a
      // chance to populate the stage-local shadows.  Restore that boundary
      // with two constant-time swaps; merge/clear below is reserved only after
      // candidate mutation becomes possible.
      if (StackTableEvidenceIncompleteBranches.empty() &&
          IndexDomainEvidenceIncompleteBranches.empty()) {
        StackTableEvidenceIncompleteBranches.swap(
            SavedStackTableEvidenceIncompleteBranches);
        IndexDomainEvidenceIncompleteBranches.swap(
            SavedIndexDomainEvidenceIncompleteBranches);
        IncompleteBranchMarkerStageActive = false;
        return;
      }
      assert(IncompleteBranchMarkerCleanupReserved &&
             "marker rollback requires prepaid cleanup");
      // A failed stage cannot commit deletion of an entry marker, but newly
      // observed resource-incomplete candidates must remain fail-closed when
      // bounded retries end.  Node-merge preserves both sets without a second
      // allocation; duplicate working nodes are destroyed by the cleanup
      // reserve charged below.
      SavedStackTableEvidenceIncompleteBranches.merge(
          StackTableEvidenceIncompleteBranches);
      SavedIndexDomainEvidenceIncompleteBranches.merge(
          IndexDomainEvidenceIncompleteBranches);
      StackTableEvidenceIncompleteBranches.clear();
      IndexDomainEvidenceIncompleteBranches.clear();
      StackTableEvidenceIncompleteBranches.swap(
          SavedStackTableEvidenceIncompleteBranches);
      IndexDomainEvidenceIncompleteBranches.swap(
          SavedIndexDomainEvidenceIncompleteBranches);
      IncompleteBranchMarkerStageActive = false;
    };
    auto CommitIncompleteBranchMarkers = [&]() {
      assert(IncompleteBranchMarkerCleanupReserved &&
             "marker commit requires prepaid cleanup");
      SavedStackTableEvidenceIncompleteBranches.clear();
      SavedIndexDomainEvidenceIncompleteBranches.clear();
      IncompleteBranchMarkerStageActive = false;
    };
    bool ForcedStableAmbiguityCommitTailThisStage = false;
    bool ForcedProposalCommitTailThisStage = false;
    size_t ForcedPendingKeyCount = 0;
    // Reachability and relocation-root suppression can change after every
    // published candidate.  Reuse proposal roots within this stage only; the
    // fixed retry bound caps total work, while each newly published resolver
    // graph receives one transactional stack-evidence allowance shared by all
    // candidates in that stage.  Do not let a provisional graph consume the
    // final revalidation graph's proof budget.
    StackTableEvidenceRemaining =
        std::min<size_t>(limits::kMaxJumpTableStackEvidenceWork,
                         StackTableEvidenceBudgetForTesting.value_or(
                             limits::kMaxJumpTableStackEvidenceWork));
    CandidateFixedPointExplorationTargets.clear();
    StageAmbiguousI386GOTPCBranches.clear();
    StageReplayedI386GOTPCKeys.clear();
    I386GOTOFFProposalRootCache.clear();
    I386GOTOFFModelReachCache.clear();
    NextStrongJumpTableProposals.clear();
    NextProvisionalRelativeEdges.clear();
    StrongJumpTableProposalOutcomes.clear();
    CandidateProposalStageEvidenceRemaining =
        std::min<size_t>(limits::kMaxJumpTableProposalStageEvidenceWork,
                         MaskFixedPointEvidenceBudgetForTesting.value_or(
                             limits::kMaxJumpTableProposalStageEvidenceWork));
    // The nested-tracking boundary hook models a persistent attacker-sized
    // stage after the first recursively discovered candidate reaches that
    // boundary.  Do not clamp the initial parent stage: it must first decode
    // the exact nested branch whose identity the regression observes.
    if (NestedMutationTrackingEvidenceExhaustedForTesting &&
        NestedMutationTrackingStageAllowanceForTesting)
      CandidateProposalStageEvidenceRemaining =
          std::min(CandidateProposalStageEvidenceRemaining,
                   *NestedMutationTrackingStageAllowanceForTesting);
    CandidateProposalStageEvidenceIncomplete = false;
    CandidateProposalStageActive = true;
    UntrackedJumpTableCandidateExhaustedThisStageForTesting = false;
    assert(CandidateProposalStageMutationAddrs.empty() &&
           "proposal mutation inventory must be retired with its stage");
    const size_t QuarantineSizeAtStageStart =
        QuarantinedJumpTableProposals.size();
    auto ConsumeProposalStageEvidence = [&](size_t Amount = 1) {
      if (Amount > CandidateProposalStageEvidenceRemaining) {
        CandidateProposalStageEvidenceRemaining = 0;
        CandidateProposalStageEvidenceIncomplete = true;
        return false;
      }
      CandidateProposalStageEvidenceRemaining -= Amount;
      return true;
    };
    auto ConsumeProposalStageProducts =
        [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
          const size_t Max = std::numeric_limits<size_t>::max();
          size_t Total = 0;
          for (const auto &[Count, Cost] : Products) {
            if (Count != 0 && Cost > Max / Count)
              return ConsumeProposalStageEvidence(Max);
            const size_t Product = Count * Cost;
            if (Product > Max - Total)
              return ConsumeProposalStageEvidence(Max);
            Total += Product;
          }
          return ConsumeProposalStageEvidence(Total);
        };
    auto OrderedLookupWork = [](size_t Count) {
      size_t Work = 1;
      for (size_t N = Count; N > 1; N = N / 2 + N % 2)
        ++Work;
      return Work;
    };
    auto PrepayTargetSetOperations = [&](size_t TargetCount) {
      const size_t Max = std::numeric_limits<size_t>::max();
      if (Insns.size() > Max - BlockStarts.size() ||
          Insns.size() + BlockStarts.size() > Max - TargetCount ||
          Insns.size() > Max - ExploredAddrs.size() ||
          Insns.size() + ExploredAddrs.size() > Max - TargetCount)
        return ConsumeProposalStageEvidence(Max);
      const size_t FutureBlockStarts =
          Insns.size() + BlockStarts.size() + TargetCount;
      const size_t FutureExplored =
          Insns.size() + ExploredAddrs.size() + TargetCount;
      const size_t BlockWork = OrderedLookupWork(FutureBlockStarts);
      const size_t ExploreWork = OrderedLookupWork(FutureExplored);
      if (BlockWork > Max - 1 || ExploreWork > Max - (BlockWork + 1))
        return ConsumeProposalStageEvidence(Max);
      const size_t PerTarget = BlockWork + 1 + ExploreWork;
      return ConsumeProposalStageProducts({{TargetCount, PerTarget}});
    };
    auto ConsumeJumpTableInfoLifecycle = [&](const JumpTableInfo &Candidate) {
      if (!ConsumeProposalStageProducts(
              {{Candidate.AuthenticatedMaskCoordinates.size(), 1},
               {Candidate.AuthenticatedMaskKnownOneWitnesses.size(), 1},
               {Candidate.StorageRanges.size(), 1},
               {Candidate.SuppressibleRelocationSlots.size(), 1},
               {Candidate.IndexValueAlternatives.size(), 1},
               {Candidate.TargetLoads.size(), 1},
               {Candidate.AuthenticatedFrameStorage.Initializers.size(), 1},
               {Candidate.AuthenticatedStorageConsumers.size(), 1},
               {Candidate.LoadRoles.size(), 1},
               {Candidate.EntryIndices.size(), 1},
               {Candidate.RuntimeCaseLabels.size(), 1},
               {Candidate.RuntimeSlotIndices.size(), 1},
               {Candidate.ExplicitTargets.size(), 1}}))
        return false;
      for (const JumpTableFrameInitializerChunk &Initializer :
           Candidate.AuthenticatedFrameStorage.Initializers)
        if (!ConsumeProposalStageEvidence(Initializer.StaticSources.size()))
          return false;
      for (const JumpTableLoadRole &Role : Candidate.LoadRoles) {
        if (!ConsumeProposalStageEvidence(Role.AllowedBases.size()) ||
            !ConsumeProposalStageEvidence(Role.Indices.size()) ||
            !ConsumeProposalStageEvidence(
                Role.FrameStorage.Initializers.size()))
          return false;
        for (const JumpTableFrameInitializerChunk &Initializer :
             Role.FrameStorage.Initializers)
          if (!ConsumeProposalStageEvidence(Initializer.StaticSources.size()))
            return false;
      }
      return ConsumeProposalStageEvidence(1);
    };
    auto SameJumpTableInfoNoShortCircuit = [&](const JumpTableInfo &Left,
                                               const JumpTableInfo &Right) {
      auto Same = compareJumpTableInfo(Left, Right,
                                       CandidateProposalStageEvidenceRemaining);
      CandidateProposalStageEvidenceIncomplete |= !Same.has_value();
      return Same;
    };
    // Architecture address models are CFG proofs, not decoder-time numeric
    // facts.  Recompute them from the currently rebuilt graph before every
    // resolver stage so a frame initializer can consume the same exact
    // occurrence certificate that final module arbitration will replay.  The
    // next stage rebuilds these transactionally after any newly published
    // table edges change reachability.
    const bool RefreshAArch64Addresses = Img.Arch == Arch::AArch64;
    const bool RefreshARMAddresses =
        Img.Arch == Arch::ARM && Img.isELF() && Img.getPointerSize() == 4;
    const bool RefreshI386Addresses = Img.Arch == Arch::X86 && Img.isELF() &&
                                      Img.getPointerSize() == 4 &&
                                      !Img.I386GOTPCFields.empty();
    if (RefreshAArch64Addresses || RefreshARMAddresses ||
        RefreshI386Addresses) {
      ActiveJumpTableProofRoots.reset();
      JumpTableProofContextComplete = true;
      if (RefreshAArch64Addresses)
        completeExactAArch64PageBases(Img);
      if (RefreshARMAddresses)
        completeExactARMRelativeLiteralAddresses(Img);
      if (RefreshI386Addresses)
        completeExactI386GOTBaseModels(Img);
      JumpTableProofContextComplete = false;
    }

    std::vector<va_t> CandidateAddrs;
    // First build an immutable candidate-address inventory.  Every instruction
    // can conservatively be retained, so prepay the vector's full ownership
    // and the two read-only ordered lookups before reserving its storage.  The
    // later phases separately reserve old-state destruction and proposal-map
    // rollback before either proposal container can be mutated.
    const size_t ResolvedLookupWork =
        OrderedLookupWork(std::max(Insns.size(), ResolvedTableInfo.size()));
    const size_t PriorLookupWork =
        OrderedLookupWork(PriorStrongJumpTableProposals.size());
    const size_t PriorProvisionalLookupWork =
        OrderedLookupWork(PriorProvisionalRelativeEdges.size());
    const size_t RollbackLookupCeiling =
        OrderedLookupWork(std::numeric_limits<size_t>::max());
    const size_t MarkerRollbackLookupWork = RollbackLookupCeiling + 1;
    if (!ConsumeProposalStageProducts(
            {// CandidateAddrs owns a reserved buffer and its eventual cleanup
             // (2N), one retained value per candidate (N), and the complete
             // source-instruction traversal (N).  Fixed vector lifetime is
             // paid separately below.
             {Insns.size(), 4},
             {Insns.size(), ResolvedLookupWork},
             {Insns.size(), PriorLookupWork},
             {Insns.size(), PriorProvisionalLookupWork}}) ||
        !ConsumeProposalStageEvidence(2)) {
      CandidateProposalStageActive = false;
      NextStrongJumpTableProposals.clear();
      NextProvisionalRelativeEdges.clear();
      RestoreIncompleteBranchMarkers();
      continue;
    }
    CandidateAddrs.reserve(Insns.size());
    for (auto &[Addr, Rec] : Insns) {
      if (!Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall)
        continue;
      auto Info = ResolvedTableInfo.find(Addr);
      const bool NeedsRevalidation = Info != ResolvedTableInfo.end() &&
                                     Info->second.RequiresCompleteCFGProof;
      if (Rec.JumpTableTargets.empty() || NeedsRevalidation ||
          PriorStrongJumpTableProposals.count(Addr) ||
          PriorProvisionalRelativeEdges.count(Addr)) {
        CandidateAddrs.push_back(Addr);
      }
    }

    // This second traversal is real work of its own.  Prepay its source visit
    // and both ordered lookups at the maximum possible final tree height; the
    // stage may decode additional instructions before rollback actually uses
    // the corresponding cleanup reservation.
    if (!ConsumeProposalStageProducts(
            {{CandidateAddrs.size(), size_t{1} + 2 * RollbackLookupCeiling}})) {
      CandidateProposalStageActive = false;
      NextStrongJumpTableProposals.clear();
      NextProvisionalRelativeEdges.clear();
      RestoreIncompleteBranchMarkers();
      continue;
    }

    // CandidateAddrs is now an immutable, read-only stage inventory.  Before
    // inserting any address into the mutation set, reserve destruction of all
    // pre-existing persistent state for the complete batch.  A preceding
    // candidate may exhaust before a later candidate runs, yet rollback still
    // clears that later candidate's old targets/metadata; its per-candidate
    // reservation therefore cannot be deferred to the execution loop.
    bool StageStartRollbackStateReserved = true;
    for (va_t Addr : CandidateAddrs) {
      auto It = Insns.find(Addr);
      auto Info = ResolvedTableInfo.find(Addr);
      const size_t OldTargetCount =
          It == Insns.end() ? 0 : It->second.JumpTableTargets.size();
      const bool HasOldPersistentState =
          OldTargetCount != 0 || Info != ResolvedTableInfo.end();
      if (HasOldPersistentState &&
          ProposalOldStateCleanupEvidenceExhaustionForTesting) {
        ProposalOldStateCleanupEvidenceExhaustionForTesting = false;
        CandidateProposalStageEvidenceRemaining = 0;
        CandidateProposalStageEvidenceIncomplete = true;
        ProposalCleanupEvidenceForTesting.OldStateExhausted = true;
        StageStartRollbackStateReserved = false;
        break;
      }
      if (!ConsumeProposalStageEvidence(OldTargetCount) ||
          (Info != ResolvedTableInfo.end() &&
           !ConsumeJumpTableInfoLifecycle(Info->second))) {
        if (HasOldPersistentState)
          ProposalCleanupEvidenceForTesting.OldStateExhausted = true;
        StageStartRollbackStateReserved = false;
        break;
      }
      if (HasOldPersistentState)
        ProposalCleanupEvidenceForTesting.OldStateReserved = true;
    }
    if (!StageStartRollbackStateReserved) {
      CandidateProposalStageActive = false;
      CandidateProposalStageMutationAddrs.clear();
      NextStrongJumpTableProposals.clear();
      NextProvisionalRelativeEdges.clear();
      StrongJumpTableProposalOutcomes.clear();
      RestoreIncompleteBranchMarkers();
      continue;
    }

    // Only after every old dynamic object is covered may stage bookkeeping
    // itself become persistent.  Pay the third CandidateAddrs traversal, both
    // ordered-container insertions and node/value lifetimes, exact rollback,
    // and transactional incomplete-marker cleanup before either insert.
    const size_t CandidateInventoryLookup =
        OrderedLookupWork(CandidateAddrs.size());
    if (!ConsumeProposalStageProducts(
            {// Third-phase CandidateAddrs source traversal.
             {CandidateAddrs.size(), 1},
             // Mutation set lookup plus node/value/future destruction.
             {CandidateAddrs.size(), CandidateInventoryLookup},
             {CandidateAddrs.size(), 3},
             // Outcome map lookup plus node/value/future destruction.
             {CandidateAddrs.size(), CandidateInventoryLookup},
             {CandidateAddrs.size(), 3},
             // Rollback source/clear/erase constants and final-height lookups.
             {CandidateAddrs.size(), 3},
             {CandidateAddrs.size(), RollbackLookupCeiling},
             {CandidateAddrs.size(), RollbackLookupCeiling},
             {CandidateAddrs.size(), RollbackLookupCeiling},
             // Marker commit/rollback owns saved nodes plus at most one stack
             // and one index marker per exact stage-start candidate.
             {SavedStackTableEvidenceIncompleteBranches.size(), 1},
             {SavedIndexDomainEvidenceIncompleteBranches.size(), 1},
             {CandidateAddrs.size(), 2},
             {CandidateAddrs.size(), MarkerRollbackLookupWork},
             {CandidateAddrs.size(), MarkerRollbackLookupWork}})) {
      CandidateProposalStageActive = false;
      CandidateProposalStageMutationAddrs.clear();
      NextStrongJumpTableProposals.clear();
      NextProvisionalRelativeEdges.clear();
      StrongJumpTableProposalOutcomes.clear();
      RestoreIncompleteBranchMarkers();
      continue;
    }
    IncompleteBranchMarkerCleanupReserved = true;
    for (va_t Addr : CandidateAddrs) {
      CandidateProposalStageMutationAddrs.insert(Addr);
      StrongJumpTableProposalOutcomes.emplace(
          Addr, StrongJumpTableProposalOutcome::DefinitiveLocalProofLoss);
    }

    bool MadeProgress = false;
    bool RefreshedProofMetadata = false;
    recoverGuardedJumpTableGroup(Img, Func, CandidateAddrs, MadeProgress,
                                 RefreshedProofMetadata);
    // Candidates from this position on have not run in this stage.
    size_t UnevaluatedFrom = 0;
    for (size_t CandidateIndex = 0; CandidateIndex < CandidateAddrs.size();
         ++CandidateIndex) {
      const va_t UA = CandidateAddrs[CandidateIndex];
      if (CandidateProposalStageEvidenceIncomplete)
        break;
      if (guardedGroupContains(UA))
        continue;
      // Charge the invocation itself before resolveJumpTable snapshots the
      // remaining stage balance into its local account.  Every nested helper
      // then consumes only min(per-candidate-cap, stage-remaining); the exact
      // used delta is debited when the invocation returns.
      const size_t CurrentResolvedLookupWork =
          OrderedLookupWork(std::max(Insns.size(), ResolvedTableInfo.size()));
      const size_t CurrentMaskLookupWork = OrderedLookupWork(
          std::max(Insns.size(), CandidateFixedPointExplorationTargets.size()));
      if (!ConsumeProposalStageProducts({
              // Insns lookup plus the resolver's erase and the three outer
              // before/after comparison lookups.  Resolver writes debit the
              // nested candidate account and flow back as its used delta.
              {1, OrderedLookupWork(Insns.size())},
              {4, CurrentResolvedLookupWork},
              // Outer erase/find of the exploration record.
              {2, CurrentMaskLookupWork},
              // Quarantine and incomplete-domain set lookups.
              {1, OrderedLookupWork(QuarantinedJumpTableProposals.size())},
              {2,
               OrderedLookupWork(IndexDomainEvidenceIncompleteBranches.size())},
              {1, 1},
              // CandidateOutcome lookup.  Proposal/strong-set insertions
              // occur inside the candidate-local account and their exact
              // delta is debited from this same stage balance on return.
              {1, OrderedLookupWork(StrongJumpTableProposalOutcomes.size())},
          }))
        break;
      UnevaluatedFrom = CandidateIndex + 1;
      auto It = Insns.find(UA);
      if (It == Insns.end())
        continue;

      auto PriorInfo = ResolvedTableInfo.find(UA);
      const bool WasProofDependent = PriorInfo != ResolvedTableInfo.end() &&
                                     PriorInfo->second.RequiresCompleteCFGProof;

      if (It->second.JumpTableTargets.empty() && !PrepayTargetSetOperations(1))
        break;
      if (It->second.JumpTableTargets.empty() &&
          resolveRelocatedInteriorBranch(Img, It->second)) {
        const va_t Target = It->second.BranchTarget;
        if (Target != InvalidVA) {
          BlockStarts.insert(Target);
          if (!ExploredAddrs.count(Target))
            explore(Img, Dec, Target);
        }
        MadeProgress = true;
        continue;
      }

      // resolveJumpTable erases the old map entry first.  Move its dynamic
      // metadata into the comparison snapshot instead of copying it before
      // the candidate-local account begins.
      // The immutable stage-inventory preflight reserved destruction of every
      // old persistent object before any candidate mutation.  Move this map
      // value into the comparison snapshot without duplicating its storage;
      // candidate-local accounting separately reserves any replacement.
      PriorInfo = ResolvedTableInfo.find(UA);
      std::optional<JumpTableInfo> OldInfo;
      if (PriorInfo != ResolvedTableInfo.end())
        OldInfo.emplace(std::move(PriorInfo->second));
      CandidateFixedPointExplorationTargets.erase(UA);
      JumpTableProofContextComplete = true;
      std::vector<va_t> Targets;
      {
        struct ProposalOutcomeTrackingScope {
          bool &Tracked;
          const bool Saved;
          explicit ProposalOutcomeTrackingScope(bool &Tracked)
              : Tracked(Tracked), Saved(Tracked) {
            Tracked = true;
          }
          ~ProposalOutcomeTrackingScope() { Tracked = Saved; }
        } TrackProposalOutcome{CandidateProposalOutcomeTracked};
        Targets = resolveJumpTable(Img, It->second);
      }
      JumpTableProofContextComplete = false;
      if (ExhaustProposalStageAfterCandidateForTesting == UA) {
        CandidateProposalStageEvidenceRemaining = 0;
        CandidateProposalStageEvidenceIncomplete = true;
      }
      if (CandidateProposalStageEvidenceIncomplete)
        break;
      auto NewInfo = ResolvedTableInfo.find(UA);
      bool MetadataChanged =
          OldInfo.has_value() != (NewInfo != ResolvedTableInfo.end());
      if (OldInfo && NewInfo != ResolvedTableInfo.end()) {
        const std::optional<bool> SameMetadata =
            SameJumpTableInfoNoShortCircuit(*OldInfo, NewInfo->second);
        if (!SameMetadata)
          break;
        MetadataChanged = !*SameMetadata;
      }
      RefreshedProofMetadata |= WasProofDependent && MetadataChanged;
      // Old/new destruction was reserved before materialization.  Charge only
      // the element-wise equality walk here, still before comparing or moving
      // either target vector.
      const std::optional<size_t> TargetComparisonWork =
          detail::scalarVectorComparisonWork(It->second.JumpTableTargets.size(),
                                             Targets.size());
      if (!TargetComparisonWork ||
          !ConsumeProposalStageEvidence(*TargetComparisonWork))
        break;
      bool SameTargets = Targets.size() == It->second.JumpTableTargets.size();
      for (size_t I = 0;
           I < std::min(Targets.size(), It->second.JumpTableTargets.size());
           ++I)
        SameTargets &= Targets[I] == It->second.JumpTableTargets[I];
      if (!SameTargets) {
        It->second.JumpTableTargets = std::move(Targets);
        MadeProgress = true;
      }
      const std::vector<va_t> &PublishedTargets = It->second.JumpTableTargets;
      if (!PublishedTargets.empty()) {
        if (!ConsumeProposalStageEvidence(
                OrderedLookupWork(EverPublishedJumpTableBranches.size()) + 1))
          break;
        EverPublishedJumpTableBranches.insert(UA);
      }

      if (!ConsumeProposalStageEvidence(
              OrderedLookupWork(CandidateFixedPointExplorationTargets.size())))
        break;
      auto Exploration = CandidateFixedPointExplorationTargets.find(UA);
      if (PublishedTargets.empty() &&
          Exploration != CandidateFixedPointExplorationTargets.end()) {
        if (!PrepayTargetSetOperations(Exploration->second.Targets.size()))
          break;
        for (va_t T : Exploration->second.Targets) {
          MadeProgress |= BlockStarts.insert(T).second;
          if (!ExploredAddrs.count(T)) {
            explore(Img, Dec, T);
            MadeProgress = true;
          }
        }
      }

      if (!PrepayTargetSetOperations(PublishedTargets.size()))
        break;
      for (va_t T : PublishedTargets) {
        // A complete-CFG proof can recover a target only after recursive
        // descent already decoded it as ordinary fall-through.  It is still a
        // distinct jump-table destination and must split the containing block;
        // exploration state answers only whether its bytes need decoding.
        MadeProgress |= BlockStarts.insert(T).second;
        if (!ExploredAddrs.count(T)) {
          explore(Img, Dec, T);
          MadeProgress = true;
        }
      }
    }

    auto RollBackIncompleteProposalStage = [&]() {
      // A shared-stage exhaustion cannot publish the prefix that happened to
      // run first.  Remove every candidate result from this immutable stage,
      // preserve Prior for a bounded retry, and discard all provisional mask
      // exploration.  Decoded bytes may remain cached, but no edge/metadata
      // certificate from the incomplete stage survives rebuildBlocks.
      const bool HadPublishedGroup = guardedGroupIsComplete();
      if (HadPublishedGroup)
        markGuardedJumpTableGroupIncomplete();
      withdrawGuardedJumpTableGroup(/*Reject=*/false);
      if (HadPublishedGroup &&
          JumpTableGroupLifecycleForTesting.CommitTailInjected)
        JumpTableGroupLifecycleForTesting.CommitTailRollbackClearedAllMembers =
            guardedGroupHasNoLiveMembers();
      bool SawForcedUntrackedCandidate = false;
      bool ForcedUntrackedCandidateTargetsCleared = false;
      for (va_t Addr : CandidateProposalStageMutationAddrs) {
        auto It = Insns.find(Addr);
        const bool IsForcedUntrackedCandidate =
            Addr == ForcedUntrackedJumpTableCandidateAddrForTesting;
        if (It != Insns.end()) {
          if (IsForcedUntrackedCandidate)
            UntrackedJumpTableCandidateProvisionalStateObservedForTesting |=
                !It->second.JumpTableTargets.empty();
          It->second.JumpTableTargets.clear();
        }
        const size_t ErasedResolved = ResolvedTableInfo.erase(Addr);
        if (IsForcedUntrackedCandidate) {
          SawForcedUntrackedCandidate = true;
          UntrackedJumpTableCandidateProvisionalStateObservedForTesting |=
              ErasedResolved != 0;
          ForcedUntrackedCandidateTargetsCleared =
              It == Insns.end() || It->second.JumpTableTargets.empty();
        }
      }
      CandidateFixedPointExplorationTargets.clear();
      NextProvisionalRelativeEdges.clear();
      bool ForcedUntrackedCandidateResolvedCleared = false;
      bool ForcedUntrackedCandidateExplorationCleared = false;
      if (SawForcedUntrackedCandidate) {
        ForcedUntrackedCandidateResolvedCleared =
            ResolvedTableInfo.count(
                ForcedUntrackedJumpTableCandidateAddrForTesting) == 0;
        ForcedUntrackedCandidateExplorationCleared =
            CandidateFixedPointExplorationTargets.count(
                ForcedUntrackedJumpTableCandidateAddrForTesting) == 0;
      }
      if (UntrackedJumpTableCandidateExhaustedThisStageForTesting) {
        UntrackedJumpTableCandidateRollbackObservedForTesting = true;
        UntrackedJumpTableCandidateStateClearedOnRollbackForTesting |=
            SawForcedUntrackedCandidate &&
            ForcedUntrackedCandidateTargetsCleared &&
            ForcedUntrackedCandidateResolvedCleared &&
            ForcedUntrackedCandidateExplorationCleared;
      }
      CandidateProposalStageMutationAddrs.clear();
      NextStrongJumpTableProposals.clear();
      StrongJumpTableProposalOutcomes.clear();
      StageAmbiguousI386GOTPCBranches.clear();
      StageReplayedI386GOTPCKeys.clear();
      CandidateProposalStageActive = false;
      RestoreIncompleteBranchMarkers();
      ProposalStageRollbackMutatedQuarantineForTesting |=
          QuarantinedJumpTableProposals.size() != QuarantineSizeAtStageStart;
      rebuildBlocks(Func);
    };
    if (CandidateProposalStageEvidenceIncomplete) {
      // The shared stage ran out before the rest of its inventory ran.  A
      // branch claimed as table-shaped earlier in this build is undecided for
      // the same reason as a candidate whose own proof ran out, and keeps its
      // opaque identity across the bounded retries in the same way.  The
      // stage reserved one marker per inventoried candidate.
      for (size_t I = UnevaluatedFrom; I < CandidateAddrs.size(); ++I) {
        const va_t UA = CandidateAddrs[I];
        const auto It = Insns.find(UA);
        if (It != Insns.end() && It->second.JumpTableTargets.empty() &&
            ShapeClaimedBranches.count(UA) && !guardedGroupContains(UA))
          IndexDomainEvidenceIncompleteBranches.insert(UA);
      }
      RollBackIncompleteProposalStage();
      continue;
    }

    // A stage observes only PriorStrongJumpTableProposals.  Outcomes are
    // explicit: an independently complete loss is quarantined monotonically,
    // while evidence exhaustion invalidates the whole stage above and can
    // never masquerade as a definitive loss.  Compare the ordered maps
    // manually so every key/vector visit is prepaid before equality or state
    // mutation; std::map's deep operator== would hide attacker-shaped work.
    for (const auto &[Addr, Outcome] : StrongJumpTableProposalOutcomes) {
      (void)Addr;
      if (!ConsumeProposalStageEvidence(1))
        break;
      if (Outcome == StrongJumpTableProposalOutcome::EvidenceIncomplete) {
        CandidateProposalStageEvidenceIncomplete = true;
        break;
      }
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      continue;
    }

    if (PriorProvisionalRelativeEdges.size() >
            std::numeric_limits<size_t>::max() -
                PriorStrongJumpTableProposals.size() ||
        !ConsumeProposalStageEvidence(PriorStrongJumpTableProposals.size() +
                                      PriorProvisionalRelativeEdges.size())) {
      RollBackIncompleteProposalStage();
      continue;
    }
    std::vector<va_t> DefinitiveLosses;
    DefinitiveLosses.reserve(PriorStrongJumpTableProposals.size() +
                             PriorProvisionalRelativeEdges.size());
    size_t QuarantineTailWork = 0;
    auto RecordDefinitiveLoss = [&](va_t LostAddr) {
      // A self-replay token may also have a strong proposal at the same
      // address.  Prepay the duplicate scan and append only one quarantine
      // record, independent of which ordered proposal map observes the loss
      // first.
      if (!ConsumeProposalStageEvidence(DefinitiveLosses.size() + 2))
        return false;
      if (std::find(DefinitiveLosses.begin(), DefinitiveLosses.end(),
                    LostAddr) != DefinitiveLosses.end())
        return true;
      if (DefinitiveLosses.size() > std::numeric_limits<size_t>::max() -
                                        QuarantinedJumpTableProposals.size()) {
        CandidateProposalStageEvidenceIncomplete = true;
        return false;
      }
      const size_t Lookup = OrderedLookupWork(
          QuarantinedJumpTableProposals.size() + DefinitiveLosses.size());
      if (Lookup > std::numeric_limits<size_t>::max() - 1 ||
          QuarantineTailWork >
              std::numeric_limits<size_t>::max() - (Lookup + 1)) {
        CandidateProposalStageEvidenceIncomplete = true;
        return false;
      }
      QuarantineTailWork += Lookup + 1;
      DefinitiveLosses.push_back(LostAddr);
      return true;
    };
    size_t PriorClearWork = PriorStrongJumpTableProposals.size();
    auto AccumulatePriorClearWork =
        [&](const StrongJumpTableRoleProposal &Proposal) {
          const size_t Max = std::numeric_limits<size_t>::max();
          if (Proposal.StorageRanges.size() > Max - PriorClearWork) {
            CandidateProposalStageEvidenceIncomplete = true;
            return false;
          }
          PriorClearWork += Proposal.StorageRanges.size();
          if (Proposal.LoadRoles.size() > Max - PriorClearWork) {
            CandidateProposalStageEvidenceIncomplete = true;
            return false;
          }
          PriorClearWork += Proposal.LoadRoles.size();
          if (Proposal.SuppressibleRelocationSlots.size() >
              Max - PriorClearWork) {
            CandidateProposalStageEvidenceIncomplete = true;
            return false;
          }
          PriorClearWork += Proposal.SuppressibleRelocationSlots.size();
          return true;
        };
    auto PreflightComparisonWork = [&](std::optional<size_t> Work) {
      return ConsumeProposalStageEvidence(
          Work.value_or(std::numeric_limits<size_t>::max()));
    };
    auto SameStorageRange = [](const JumpTableStorageRange &Left,
                               const JumpTableStorageRange &Right) {
      bool Same = true;
      Same &= Left.BaseAddr == Right.BaseAddr;
      Same &= Left.EntrySize == Right.EntrySize;
      Same &= Left.EntryStride == Right.EntryStride;
      Same &= Left.PhysicalSlotCount == Right.PhysicalSlotCount;
      return Same;
    };
    auto SameLoadRole = [&](const StrongJumpTableLoadRole &Left,
                            const StrongJumpTableLoadRole &Right) {
      bool Same = true;
      Same &= Left.Load.Value.Space == Right.Load.Value.Space;
      Same &= Left.Load.Value.Offset == Right.Load.Value.Offset;
      Same &= Left.Load.Value.Size == Right.Load.Value.Size;
      Same &= Left.Load.Value.Provenance == Right.Load.Value.Provenance;
      Same &= Left.Load.Value.AddressOwnerVA == Right.Load.Value.AddressOwnerVA;
      Same &= Left.Load.Addr == Right.Load.Addr;
      Same &= Left.Load.Seq == Right.Load.Seq;
      Same &= Left.Load.DefinedAtPoint == Right.Load.DefinedAtPoint;
      Same &= Left.LoadWidth == Right.LoadWidth;
      return Same;
    };
    auto SameStrongProposal =
        [&](const StrongJumpTableRoleProposal &Left,
            const StrongJumpTableRoleProposal &Right) -> std::optional<bool> {
      if (!PreflightComparisonWork(
              detail::strongJumpTableProposalComparisonWork(
                  Left.StorageRanges.size(), Right.StorageRanges.size(),
                  Left.LoadRoles.size(), Right.LoadRoles.size(),
                  Left.SuppressibleRelocationSlots.size(),
                  Right.SuppressibleRelocationSlots.size())))
        return std::nullopt;
      bool Same = true;
      Same &= Left.StorageRanges.size() == Right.StorageRanges.size();
      for (size_t I = 0;
           I < std::min(Left.StorageRanges.size(), Right.StorageRanges.size());
           ++I)
        Same &= SameStorageRange(Left.StorageRanges[I], Right.StorageRanges[I]);
      const bool LeftHasExact = Left.ExactPhysicalStorageRange.has_value();
      const bool RightHasExact = Right.ExactPhysicalStorageRange.has_value();
      Same &= LeftHasExact == RightHasExact;
      if (LeftHasExact && RightHasExact)
        Same &= SameStorageRange(*Left.ExactPhysicalStorageRange,
                                 *Right.ExactPhysicalStorageRange);
      Same &= Left.LoadRoles.size() == Right.LoadRoles.size();
      for (size_t I = 0;
           I < std::min(Left.LoadRoles.size(), Right.LoadRoles.size()); ++I)
        Same &= SameLoadRole(Left.LoadRoles[I], Right.LoadRoles[I]);
      Same &= Left.SuppressibleRelocationSlots.size() ==
              Right.SuppressibleRelocationSlots.size();
      for (size_t I = 0; I < std::min(Left.SuppressibleRelocationSlots.size(),
                                      Right.SuppressibleRelocationSlots.size());
           ++I)
        Same &= Left.SuppressibleRelocationSlots[I] ==
                Right.SuppressibleRelocationSlots[I];
      Same &= Left.ProofRank == Right.ProofRank;
      return Same;
    };
    auto SameProvisionalProposal =
        [&](const ProvisionalRelativeEdgeProposal &Left,
            const ProvisionalRelativeEdgeProposal &Right)
        -> std::optional<bool> {
      if (!PreflightComparisonWork(
              detail::provisionalRelativeProposalComparisonWork(
                  Left.LoadRoles.size(), Right.LoadRoles.size(),
                  Left.Targets.size(), Right.Targets.size())))
        return std::nullopt;
      bool Same = SameStorageRange(Left.Storage, Right.Storage);
      Same &= Left.EdgeKind == Right.EdgeKind;
      Same &= Left.AuthenticatesPhysicalStorage ==
              Right.AuthenticatesPhysicalStorage;
      Same &= Left.CompleteDenseRuntimeCoordinates ==
              Right.CompleteDenseRuntimeCoordinates;
      Same &=
          Left.StablePublishedTargetReplay == Right.StablePublishedTargetReplay;
      const bool LeftHasRuntime = Left.CompleteRuntimeStorageRange.has_value();
      const bool RightHasRuntime =
          Right.CompleteRuntimeStorageRange.has_value();
      Same &= LeftHasRuntime == RightHasRuntime;
      if (LeftHasRuntime && RightHasRuntime)
        Same &= SameStorageRange(*Left.CompleteRuntimeStorageRange,
                                 *Right.CompleteRuntimeStorageRange);
      Same &= Left.TargetAnchor == Right.TargetAnchor;
      Same &= Left.EntryScale == Right.EntryScale;
      Same &= Left.IsSigned == Right.IsSigned;
      Same &= Left.LoadRoles.size() == Right.LoadRoles.size();
      for (size_t I = 0;
           I < std::min(Left.LoadRoles.size(), Right.LoadRoles.size()); ++I)
        Same &= SameLoadRole(Left.LoadRoles[I], Right.LoadRoles[I]);
      Same &= Left.Targets.size() == Right.Targets.size();
      for (size_t I = 0;
           I < std::min(Left.Targets.size(), Right.Targets.size()); ++I)
        Same &= Left.Targets[I] == Right.Targets[I];
      Same &= Left.ProofRank == Right.ProofRank;
      return Same;
    };
    bool ProposalUniverseChanged = PriorStrongJumpTableProposals.size() !=
                                   NextStrongJumpTableProposals.size();
    if (!PreflightComparisonWork(detail::orderedProposalMapMergeComparisonWork(
            PriorStrongJumpTableProposals.size(),
            NextStrongJumpTableProposals.size()))) {
      RollBackIncompleteProposalStage();
      continue;
    }
    auto PriorIt = PriorStrongJumpTableProposals.begin();
    auto NextIt = NextStrongJumpTableProposals.begin();
    while (PriorIt != PriorStrongJumpTableProposals.end() ||
           NextIt != NextStrongJumpTableProposals.end()) {
      if (NextIt == NextStrongJumpTableProposals.end() ||
          (PriorIt != PriorStrongJumpTableProposals.end() &&
           PriorIt->first < NextIt->first)) {
        if (!AccumulatePriorClearWork(PriorIt->second)) {
          CandidateProposalStageEvidenceIncomplete = true;
          break;
        }
        const va_t LostAddr = PriorIt->first;
        if (!ConsumeProposalStageEvidence(
                OrderedLookupWork(StrongJumpTableProposalOutcomes.size())))
          break;
        auto OutcomeIt = StrongJumpTableProposalOutcomes.find(LostAddr);
        if (OutcomeIt == StrongJumpTableProposalOutcomes.end() ||
            OutcomeIt->second ==
                StrongJumpTableProposalOutcome::EvidenceIncomplete) {
          CandidateProposalStageEvidenceIncomplete = true;
          break;
        }
        if (OutcomeIt->second !=
                StrongJumpTableProposalOutcome::DefinitiveLocalProofLoss &&
            OutcomeIt->second != StrongJumpTableProposalOutcome::
                                     SelfReplayDefinitiveLocalProofLoss &&
            OutcomeIt->second != StrongJumpTableProposalOutcome::
                                     AwaitingSiblingRuntimeCertificate &&
            OutcomeIt->second !=
                StrongJumpTableProposalOutcome::SemanticOpaque) {
          CandidateProposalStageEvidenceIncomplete = true;
          break;
        }
        if (OutcomeIt->second ==
            StrongJumpTableProposalOutcome::SemanticOpaque) {
          // Complete semantic ambiguity removes the provisional role but is
          // not a monotonic proof loss: the next immutable graph must replay
          // MayDepend before the branch-level opaque certificate can commit.
          ProposalUniverseChanged = true;
          ++PriorIt;
          continue;
        }
        if (OutcomeIt->second ==
            StrongJumpTableProposalOutcome::AwaitingSiblingRuntimeCertificate) {
          // The candidate saw a distinct raw sibling in the frozen prior
          // universe.  Defer quarantine only if it also retained its own edge
          // for the next immutable replay; this carries no strong authority.
          // An ordinary self-edge has the definitive outcome above and cannot
          // spin until the global retry limit.
          if (!ConsumeProposalStageEvidence(
                  OrderedLookupWork(NextProvisionalRelativeEdges.size())))
            break;
          if (NextProvisionalRelativeEdges.count(LostAddr)) {
            ProposalUniverseChanged = true;
            ++PriorIt;
            continue;
          }
        }
        if (!RecordDefinitiveLoss(LostAddr))
          break;
        ProposalUniverseChanged = true;
        ++PriorIt;
        continue;
      }
      if (PriorIt == PriorStrongJumpTableProposals.end() ||
          NextIt->first < PriorIt->first) {
        ProposalUniverseChanged = true;
        ++NextIt;
        continue;
      }

      const StrongJumpTableRoleProposal &Prior = PriorIt->second;
      const StrongJumpTableRoleProposal &Next = NextIt->second;
      const std::optional<bool> Same = SameStrongProposal(Prior, Next);
      if (!Same || !AccumulatePriorClearWork(Prior))
        break;
      ProposalUniverseChanged |= !*Same;
      ++PriorIt;
      ++NextIt;
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      continue;
    }

    // Provisional relative edges form a second immutable universe.  They are
    // compared and swapped transactionally like strong role proposals, but a
    // lost edge is never quarantined: it grants only a temporary resolver-graph
    // overlay and must disappear before the final fixed point can commit.
    bool ProvisionalEdgeUniverseChanged =
        PriorProvisionalRelativeEdges.size() !=
        NextProvisionalRelativeEdges.size();
    if (!PreflightComparisonWork(detail::orderedProposalMapMergeComparisonWork(
            PriorProvisionalRelativeEdges.size(),
            NextProvisionalRelativeEdges.size()))) {
      RollBackIncompleteProposalStage();
      continue;
    }
    size_t PriorProvisionalClearWork = PriorProvisionalRelativeEdges.size();
    auto AccumulateProvisionalClearWork =
        [&](const ProvisionalRelativeEdgeProposal &Proposal) {
          const size_t Max = std::numeric_limits<size_t>::max();
          if (Proposal.LoadRoles.size() > Max - PriorProvisionalClearWork ||
              Proposal.Targets.size() >
                  Max - PriorProvisionalClearWork - Proposal.LoadRoles.size()) {
            CandidateProposalStageEvidenceIncomplete = true;
            return false;
          }
          PriorProvisionalClearWork +=
              Proposal.LoadRoles.size() + Proposal.Targets.size();
          return true;
        };
    auto PriorEdgeIt = PriorProvisionalRelativeEdges.begin();
    auto NextEdgeIt = NextProvisionalRelativeEdges.begin();
    while (PriorEdgeIt != PriorProvisionalRelativeEdges.end() ||
           NextEdgeIt != NextProvisionalRelativeEdges.end()) {
      if (NextEdgeIt == NextProvisionalRelativeEdges.end() ||
          (PriorEdgeIt != PriorProvisionalRelativeEdges.end() &&
           PriorEdgeIt->first < NextEdgeIt->first)) {
        ProvisionalEdgeUniverseChanged = true;
        if (!AccumulateProvisionalClearWork(PriorEdgeIt->second))
          break;
        const va_t LostAddr = PriorEdgeIt->first;
        if (!ConsumeProposalStageEvidence(
                OrderedLookupWork(StrongJumpTableProposalOutcomes.size())))
          break;
        const auto Outcome = StrongJumpTableProposalOutcomes.find(LostAddr);
        if (Outcome == StrongJumpTableProposalOutcomes.end()) {
          CandidateProposalStageEvidenceIncomplete = true;
          break;
        }
        if (Outcome->second == StrongJumpTableProposalOutcome::
                                   SelfReplayDefinitiveLocalProofLoss &&
            !RecordDefinitiveLoss(LostAddr))
          break;
        ++PriorEdgeIt;
        continue;
      }
      if (PriorEdgeIt == PriorProvisionalRelativeEdges.end() ||
          NextEdgeIt->first < PriorEdgeIt->first) {
        ProvisionalEdgeUniverseChanged = true;
        ++NextEdgeIt;
        continue;
      }
      const ProvisionalRelativeEdgeProposal &Prior = PriorEdgeIt->second;
      const ProvisionalRelativeEdgeProposal &Next = NextEdgeIt->second;
      const std::optional<bool> Same = SameProvisionalProposal(Prior, Next);
      if (!Same || !AccumulateProvisionalClearWork(Prior))
        break;
      ProvisionalEdgeUniverseChanged |= !*Same;
      ++PriorEdgeIt;
      ++NextEdgeIt;
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      continue;
    }
    // A producer may retain either its exact complete runtime range or, for a
    // sparse domain, only its exact ordinary published target vector.  Both
    // forms are durable candidate-local evidence rather than resolver edge
    // overlays; neither grants physical ownership.  Any raw edge proposal
    // still prevents stability and must retire in a later immutable stage.
    bool StableDurableCertificatesOnly = true;
    size_t StableRuntimeCertificateRetireWork =
        PriorProvisionalRelativeEdges.size();
    for (const auto &[Addr, Proposal] : PriorProvisionalRelativeEdges) {
      const size_t Max = std::numeric_limits<size_t>::max();
      if (!ConsumeProposalStageEvidence(
              OrderedLookupWork(Insns.size()) +
              detail::kStableRelativeRuntimeCertificateFixedWork) ||
          !ConsumeProposalStageEvidence(Proposal.Targets.size()))
        break;
      const auto Producer = Insns.find(Addr);
      const JumpTableStorageRange *RuntimeRange =
          Proposal.CompleteRuntimeStorageRange
              ? &*Proposal.CompleteRuntimeStorageRange
              : nullptr;
      bool StableCertificate = false;
      if (Producer != Insns.end()) {
        if (Proposal.StablePublishedTargetReplay) {
          StableCertificate =
              !Proposal.AuthenticatesPhysicalStorage &&
              !Proposal.CompleteDenseRuntimeCoordinates && !RuntimeRange &&
              !Proposal.Targets.empty() &&
              Proposal.Targets == Producer->second.JumpTableTargets;
        } else {
          StableCertificate =
              !Proposal.AuthenticatesPhysicalStorage &&
              !Proposal.CompleteDenseRuntimeCoordinates &&
              detail::isStableProvisionalRelativeRuntimeCertificate(
                  RuntimeRange, Proposal.Targets,
                  Producer->second.JumpTableTargets);
        }
      }
      if (!StableCertificate) {
        StableDurableCertificatesOnly = false;
        continue;
      }
      if (Proposal.LoadRoles.size() >
              Max - StableRuntimeCertificateRetireWork ||
          Proposal.Targets.size() > Max - StableRuntimeCertificateRetireWork -
                                        Proposal.LoadRoles.size()) {
        CandidateProposalStageEvidenceIncomplete = true;
        break;
      }
      StableRuntimeCertificateRetireWork +=
          Proposal.LoadRoles.size() + Proposal.Targets.size();
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      continue;
    }
    const bool StableProvisionalUniverse =
        PriorProvisionalRelativeEdges.empty() ||
        (!ProvisionalEdgeUniverseChanged && StableDurableCertificatesOnly);
    const bool StableStage =
        !MadeProgress && !RefreshedProofMetadata && !ProposalUniverseChanged &&
        !ProvisionalEdgeUniverseChanged && StableProvisionalUniverse;
    std::set<va_t> StableSafelyPublishedI386GOTPCBranches;
    if (StableStage) {
      const size_t InsnLookup = OrderedLookupWork(Insns.size());
      const size_t SafeBranchLookup =
          OrderedLookupWork(PendingAmbiguousI386GOTPCBranches.size());
      if (!ConsumeProposalStageProducts(
              {{PendingAmbiguousI386GOTPCBranches.size(),
                InsnLookup + SafeBranchLookup + 4}})) {
        CandidateProposalStageEvidenceIncomplete = true;
      }
      if (!CandidateProposalStageEvidenceIncomplete) {
        for (va_t BranchAddr : PendingAmbiguousI386GOTPCBranches) {
          const auto Branch = Insns.find(BranchAddr);
          if (Branch != Insns.end() && !Branch->second.JumpTableTargets.empty())
            StableSafelyPublishedI386GOTPCBranches.insert(BranchAddr);
        }
      }
      for (const I386GOTOFFAmbiguityReplayKey &Key :
           PendingAmbiguousI386GOTPCKeys) {
        constexpr size_t KeyWork = 5;
        const size_t ReplayLookup =
            OrderedLookupWork(StageReplayedI386GOTPCKeys.size());
        const size_t StablePublishedLookup =
            OrderedLookupWork(StableSafelyPublishedI386GOTPCBranches.size());
        if (!ConsumeProposalStageProducts(
                {// Preflight plus commit each perform the exact ordered
                 // replay/safe-publication membership checks.
                 {KeyWork, 2 * ReplayLookup + 2},
                 {2, StablePublishedLookup},
                 // One possible erase and the insertion-prepaid node cleanup.
                 {1, 1}}))
          break;
      }
      // Rebuild the derived branch index allocation-free after key retirement.
      // Prepay its full branch-by-key membership walk before persistent commit.
      if (!CandidateProposalStageEvidenceIncomplete &&
          !ConsumeProposalStageProducts(
              {{PendingAmbiguousI386GOTPCBranches.size(),
                PendingAmbiguousI386GOTPCKeys.size() + 1}})) {
        CandidateProposalStageEvidenceIncomplete = true;
      }
    }
    if (!CandidateProposalStageEvidenceIncomplete && StableStage &&
        ExhaustStableI386AmbiguityCommitTailForTesting &&
        !ExhaustedStableI386AmbiguityCommitTailForTesting &&
        !PendingAmbiguousI386GOTPCKeys.empty()) {
      ExhaustedStableI386AmbiguityCommitTailForTesting = true;
      ForcedStableAmbiguityCommitTailThisStage = true;
      ForcedPendingKeyCount = PendingAmbiguousI386GOTPCKeys.size();
      ProposalStageCommitTailEvidenceExhaustedForTesting = true;
      CandidateProposalStageEvidenceRemaining = 0;
      CandidateProposalStageEvidenceIncomplete = true;
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      if (ForcedStableAmbiguityCommitTailThisStage &&
          PendingAmbiguousI386GOTPCKeys.size() == ForcedPendingKeyCount)
        CommitTailRollbackRetainedPendingI386AmbiguityForTesting = true;
      continue;
    }
    if (!CandidateProposalStageEvidenceIncomplete &&
        ExhaustProposalStageCommitTailForTesting &&
        !ExhaustedProposalStageCommitTailForTesting &&
        !DefinitiveLosses.empty()) {
      ExhaustedProposalStageCommitTailForTesting = true;
      ForcedProposalCommitTailThisStage = true;
      ProposalStageCommitTailEvidenceExhaustedForTesting = true;
      CandidateProposalStageEvidenceRemaining = 0;
      CandidateProposalStageEvidenceIncomplete = true;
    }
    if (!CandidateProposalStageEvidenceIncomplete &&
        ExhaustPublishedJumpTableGroupCommitTailForTesting &&
        !JumpTableGroupLifecycleForTesting.CommitTailInjected &&
        JumpTableGroupLifecycleForTesting.PublishedMemberCount != 0 &&
        guardedGroupIsComplete()) {
      JumpTableGroupLifecycleForTesting.CommitTailInjected = true;
      ProposalStageCommitTailEvidenceExhaustedForTesting = true;
      CandidateProposalStageEvidenceRemaining = 0;
      CandidateProposalStageEvidenceIncomplete = true;
    }
    // Preflight every ordered insertion and every container traversal needed
    // by the commit before the first persistent mutation.  In particular, an
    // exact budget boundary cannot insert a quarantine record and then fail on
    // swap/clear, which would poison the bounded retry despite rollback.
    if (CandidateProposalStageEvidenceIncomplete ||
        !ConsumeProposalStageProducts(
            {{1, QuarantineTailWork},
             {1, 1}, // Prior/Next swap.
             {1, PriorClearWork},
             {1, 1}, // Provisional-edge Prior/Next swap.
             {1, PriorProvisionalClearWork},
             {1, StableStage ? StableRuntimeCertificateRetireWork : 0},
             {1, StrongJumpTableProposalOutcomes.size()}})) {
      ProposalStageCommitTailEvidenceExhaustedForTesting = true;
      RollBackIncompleteProposalStage();
      if (ForcedProposalCommitTailThisStage &&
          QuarantinedJumpTableProposals.size() == QuarantineSizeAtStageStart)
        ProposalStageForcedCommitTailRollbackPreservedStateForTesting = true;
      continue;
    }
    for (va_t Addr : DefinitiveLosses)
      QuarantinedJumpTableProposals.insert(Addr);
    PriorStrongJumpTableProposals.swap(NextStrongJumpTableProposals);
    NextStrongJumpTableProposals.clear();
    PriorProvisionalRelativeEdges.swap(NextProvisionalRelativeEdges);
    NextProvisionalRelativeEdges.clear();
    if (StableStage && StableDurableCertificatesOnly)
      PriorProvisionalRelativeEdges.clear();
    StrongJumpTableProposalOutcomes.clear();
    CandidateProposalStageMutationAddrs.clear();
    CandidateProposalStageActive = false;

    if (!MadeProgress) {
      // Targets can remain byte-for-byte equal while the complete CFG changes
      // case labels, range/identity metadata, or mutation state.  Publish the
      // freshly cached JumpTableInfo, then require another complete resolver
      // round on that published CFG.  Only an equal target set *and* equal
      // proof metadata in the following round is a fixed point; a metadata
      // cycle consumes the bounded retry budget and is discarded below.
      if (!StableStage) {
        // This stage committed proposal metadata, but the next round observes
        // a different proof universe.  Keep both the prior safety markers and
        // any newly incomplete candidates until a later no-progress round can
        // revalidate their removal on the stable graph.
        RestoreIncompleteBranchMarkers();
        rebuildBlocks(Func);
        continue;
      }
      CommitIncompleteBranchMarkers();
      ReachedFixedPoint = true;
      // A changed graph may publish some other table at the same branch, but
      // that does not replay this exact use/base/occurrence query.
      detail::retireReplayedI386GOTPCAmbiguities(
          PendingAmbiguousI386GOTPCKeys, StageReplayedI386GOTPCKeys,
          &StableSafelyPublishedI386GOTPCBranches);
      for (auto It = PendingAmbiguousI386GOTPCBranches.begin();
           It != PendingAmbiguousI386GOTPCBranches.end();) {
        const va_t BranchAddr = *It;
        const bool HasPendingKey =
            std::any_of(PendingAmbiguousI386GOTPCKeys.begin(),
                        PendingAmbiguousI386GOTPCKeys.end(),
                        [&](const I386GOTOFFAmbiguityReplayKey &Key) {
                          return std::get<0>(Key) == BranchAddr;
                        });
        if (!HasPendingKey)
          It = PendingAmbiguousI386GOTPCBranches.erase(It);
        else
          ++It;
      }
      AmbiguousI386GOTPCBranches.swap(StageAmbiguousI386GOTPCBranches);
      StageAmbiguousI386GOTPCBranches.clear();
      break;
    }

    // Newly decoded targets change reaching definitions and table ownership.
    // Marker deletion is a declassification and therefore cannot commit on a
    // graph-growing round; carry the conservative union into revalidation.
    PreviousStageGrew = true;
    RestoreIncompleteBranchMarkers();
    completeExactAArch64PageBases(Img);
    splitBlocks();
    rebuildBlocks(Func);

    LLVM_DEBUG(llvm::dbgs()
               << "  multi-stage " << (Stage + 1) << ": rebuilt to "
               << Func.Blocks.size() << " blocks\n");
  }
  CandidateProposalStageActive = false;
  CandidateProposalStageMutationAddrs.clear();

  // A reachable unresolved transfer can re-enter an indexed LOAD after its
  // local mask, invalidating the selector-domain proof even for a single
  // dispatch.  Check after the proposal round stabilizes, when all sibling
  // table edges are present; a temporary missing sibling edge is not an
  // opaque re-entry.  If the round never stabilizes, withdraw relative
  // tables instead.  A direct call into this function can also enter after
  // the mask; an external direct call and a non-resumable trap cannot.  An
  // indirect call enters a function entry and resumes at its fall-through,
  // as the call model assumes for every other indirect call.  A resumable
  // trap (`__debugbreak()`) also resumes at its fall-through; only one whose
  // continuation was not decoded leaves the graph incomplete.
  //
  // A register jump that runs once its block has released the frame every
  // path into the block allocated leaves this function as a tail call does:
  // the function's code past its prologue runs only inside its frame.  Like
  // an indirect call, it enters a function entry, not a dispatch.
  if (Img.Arch == Arch::X64 && !ResolvedTableInfo.empty()) {
    size_t ClosureBudget = limits::kMaxJumpTableMaskFixedPointEvidenceWork;
    auto DebitClosure = [&](size_t Work) {
      if (Work > ClosureBudget) {
        ClosureBudget = 0;
        return false;
      }
      ClosureBudget -= Work;
      return true;
    };
    std::optional<std::set<va_t>> FrameReleasingJumps;
    auto CanReenterAtUnknownAddress = [&](const InsnRecord &Rec) {
      if (Rec.IsIndirect && !Rec.IsRet && !Rec.IsCall &&
          Rec.JumpTableTargets.empty()) {
        if (!FrameReleasingJumps) {
          // The frame analysis visits every operation; an exhausted budget
          // grants nothing.
          size_t Operations = 0;
          for (const LowBlock &Block : Func.Blocks)
            Operations += Block.Ops.size();
          FrameReleasingJumps = DebitClosure(Operations)
                                    ? frameReleasingIndirectJumps(Img, Func)
                                    : std::set<va_t>{};
        }
        return !FrameReleasingJumps->count(Rec.Addr);
      }
      return (Rec.IsResumableTerminator && Rec.IsRet) ||
             (Rec.IsBranch && !Rec.IsIndirect && !Rec.IsCall &&
              Rec.BranchTarget == InvalidVA);
    };
    bool HasRelativeTable = false;
    bool RevokePublishedTables = !DebitClosure(ResolvedTableInfo.size());
    if (RevokePublishedTables)
      HasRelativeTable = true;
    if (!RevokePublishedTables)
      for (const auto &[Addr, Info] : ResolvedTableInfo) {
        (void)Addr;
        HasRelativeTable |= Info.IsRelative;
      }
    // An independent local bound may have been published before the proposal
    // stage exhausted its iteration/work budget.  It still needs this final
    // re-entry audit, which cannot certify an unstable graph.
    if (HasRelativeTable && !ReachedFixedPoint)
      RevokePublishedTables = true;
    // The resolver flow graph models calls by their fallthrough.  A known call
    // target in this same decoded function is another entry with potentially
    // different selector state.  Two targets are not:
    // - the function's own entry: a recursive invocation enters where every
    //   caller enters, so it reaches a dispatch only through the code an
    //   ordinary call runs;
    // - another function's entry that this graph reaches as a tail call. The
    //   walk models that edge as `call entry; ret` at the entry, a record that
    //   holds none of this function's code: a call there runs that function.
    auto TailCallsItsOwnAddress = [](const InsnRecord &Rec) {
      if (!Rec.IsCall || !Rec.IsRet || Rec.IsIndirect)
        return false;
      for (const LowOp &Op : Rec.Ops)
        if (Op.Opcode == NdOp::CALL)
          return Op.NumInputs >= 1 && Op.Inputs[0].isConst() &&
                 Op.Inputs[0].Offset == Rec.Addr;
      return false;
    };
    auto EntersFunctionInterior = [&](va_t Target) {
      if (Target == CurrentFuncEntry)
        return false;
      const auto It = Insns.find(Target);
      return It != Insns.end() && !TailCallsItsOwnAddress(It->second);
    };
    // The first scan is a cheap negative gate.  Most switch functions have no
    // unknown target or direct call into this function and pay no graph-copy/
    // reachability cost here.
    bool HasUnknownTransfer = false;
    if (HasRelativeTable) {
      if (!DebitClosure(Insns.size()))
        RevokePublishedTables = true;
      else
        for (const auto &[Addr, Rec] : Insns) {
          (void)Addr;
          HasUnknownTransfer = CanReenterAtUnknownAddress(Rec);
          if (!HasUnknownTransfer && Rec.IsCall && !Rec.IsIndirect &&
              Rec.Immediate) {
            if (Insns.size() == std::numeric_limits<size_t>::max() ||
                !DebitClosure(Insns.size() + 1)) {
              RevokePublishedTables = true;
              break;
            }
            HasUnknownTransfer = EntersFunctionInterior(*Rec.Immediate);
          }
          if (HasUnknownTransfer)
            break;
        }
    }
    if (HasUnknownTransfer && !RevokePublishedTables) {
      if (!DebitClosure(ResolvedTableInfo.size()))
        RevokePublishedTables = true;
      for (const auto &[Addr, Info] : ResolvedTableInfo) {
        if (RevokePublishedTables)
          break;
        if (!Info.IsRelative)
          continue;
        // A balanced-tree lookup takes at most its node count plus one
        // comparisons.  Reserve both it and the candidate graph query.
        if (Insns.size() == std::numeric_limits<size_t>::max() ||
            !DebitClosure(Insns.size() + 1)) {
          RevokePublishedTables = true;
          break;
        }
        const auto Branch = Insns.find(Addr);
        if (Branch == Insns.end() || Branch->second.JumpTableTargets.empty())
          continue;
        bool ReachabilityComplete = false;
        const std::set<va_t> Reachable = candidateReachableInstructions(
            Branch->second, Branch->second.JumpTableTargets, PersistentCFGRoots,
            Info.StorageRanges, &ClosureBudget, &ReachabilityComplete);
        if (!ReachabilityComplete ||
            Reachable.size() == std::numeric_limits<size_t>::max() ||
            !DebitClosure(Reachable.size() + 1) ||
            !Reachable.count(CurrentFuncEntry)) {
          RevokePublishedTables = true;
          break;
        }
        if (Insns.size() > std::numeric_limits<size_t>::max() - 2) {
          RevokePublishedTables = true;
          break;
        }
        const size_t LookupWork = Insns.size() + 2;
        if ((Reachable.size() != 0 &&
             LookupWork > ClosureBudget / Reachable.size()) ||
            !DebitClosure(Reachable.size() * LookupWork)) {
          RevokePublishedTables = true;
          break;
        }
        for (va_t ReachableAddr : Reachable) {
          const auto Open = Insns.find(ReachableAddr);
          if (Open != Insns.end() && CanReenterAtUnknownAddress(Open->second)) {
            RevokePublishedTables = true;
            break;
          }
          if (Open != Insns.end() && Open->second.IsCall &&
              !Open->second.IsIndirect && Open->second.Immediate) {
            if (!DebitClosure(Insns.size() + 1) ||
                EntersFunctionInterior(*Open->second.Immediate)) {
              RevokePublishedTables = true;
              break;
            }
          }
        }
      }
    }
    if (HasRelativeTable && RevokePublishedTables) {
      // The target vectors and metadata were charged when retained by the
      // proposal stage; withdrawing them needs no new allocation.
      bool RemovedTableEdges = false;
      for (auto &[Addr, Rec] : Insns) {
        (void)Addr;
        RemovedTableEdges |= !Rec.JumpTableTargets.empty();
        Rec.JumpTableTargets.clear();
      }
      ResolvedTableInfo.clear();
      PriorStrongJumpTableProposals.clear();
      PriorProvisionalRelativeEdges.clear();
      CandidateFixedPointExplorationTargets.clear();
      if (RemovedTableEdges)
        rebuildBlocks(Func);
    }
  }

  // A proof-dependent target set may only escape after one whole round saw no
  // new decoded targets and no target-set change.  If the bounded iteration
  // budget is exhausted first, discard those provisional edges rather than
  // publishing a CFG whose validity depends on traversal order.
  if (!ReachedFixedPoint) {
    const bool HadPublishedGroup = guardedGroupIsComplete();
    if (HadPublishedGroup)
      markGuardedJumpTableGroupIncomplete();
    withdrawGuardedJumpTableGroup(/*Reject=*/false);
    if (HadPublishedGroup)
      rebuildBlocks(Func);
    // A complete ambiguity result from a graph that never stabilized is not a
    // semantic certificate, but neither is it callback evidence.  The
    // insertion-time-prepaid pending set carries those exact candidates into
    // tail-call conversion without allocating after evidence exhaustion.
    bool Changed = false;
    for (auto &[Addr, Rec] : Insns) {
      auto Info = ResolvedTableInfo.find(Addr);
      if (Info == ResolvedTableInfo.end() ||
          !Info->second.RequiresCompleteCFGProof)
        continue;
      Changed |= !Rec.JumpTableTargets.empty();
      Rec.JumpTableTargets.clear();
      ResolvedTableInfo.erase(Info);
    }
    if (Changed)
      rebuildBlocks(Func);
    PriorProvisionalRelativeEdges.clear();
    NextProvisionalRelativeEdges.clear();
    CandidateFixedPointExplorationTargets.clear();
  }
  StageAmbiguousI386GOTPCBranches.clear();
  StageReplayedI386GOTPCKeys.clear();

  // A transient empty target set can recover in a later stage, so classify a
  // lost validated table only after convergence (or the bounded cleanup above)
  // has established the final target state.  These addresses are not generic
  // detector claims: every member published at least one concrete table edge
  // earlier in this build or in a seeded build of this same function.
  for (auto It = EverPublishedJumpTableBranches.begin();
       It != EverPublishedJumpTableBranches.end();) {
    const va_t Addr = *It;
    auto InsnIt = Insns.find(Addr);
    if (InsnIt == Insns.end() || !InsnIt->second.IsBranch ||
        !InsnIt->second.IsIndirect || InsnIt->second.IsCall) {
      It = EverPublishedJumpTableBranches.erase(It);
      continue;
    }
    if (InsnIt->second.JumpTableTargets.empty())
      LostValidatedJumpTableBranches.insert(Addr);
    ++It;
  }
  for (va_t Addr : EverStrongJumpTableProposalBranches) {
    auto InsnIt = Insns.find(Addr);
    if (InsnIt != Insns.end() && InsnIt->second.IsBranch &&
        InsnIt->second.IsIndirect && !InsnIt->second.IsCall &&
        InsnIt->second.JumpTableTargets.empty())
      LostValidatedJumpTableBranches.insert(Addr);
  }
}

} // namespace neverd
