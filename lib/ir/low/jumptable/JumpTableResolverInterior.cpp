//===- JumpTableResolverInterior.cpp - Interior code-pointer roots -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Ownership, root discovery, and scalar relay resolution for relocation-
/// proven pointers to basic blocks inside a function.  These values are not
/// callable function entries and must retain the owning frame.
///
//===----------------------------------------------------------------------===//

#include "../X86/RegistrationStackOwner.h"
#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/AArch64AddressResult.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/MIPS/MIPSControlTarget.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/CodePointerSources.h"
#include "neverd/loader/MachO/FunctionBody.h"
#include "neverd/support/AArch64Literal.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

namespace {

template <typename Visitor>
void visitAbsoluteRelocationSources(const BinaryImage &Img, Visitor Visit) {
  for (va_t Slot : Img.CodePtrRelocSlots)
    if (const auto Target = normalizedRelocatedCodeTarget(Img, Slot))
      Visit(*Target, Slot);
}

} // namespace

struct detail::AbsoluteRelocationRootIndex::RuntimeRoleCache {
  std::once_flag Once;
  std::map<va_t, va_t> Sources;
};

detail::AbsoluteRelocationRootIndex::AbsoluteRelocationRootIndex(
    const BinaryImage &Image)
    : Image(&Image), RuntimeRoles(std::make_shared<RuntimeRoleCache>()) {
  Sources.reserve(Image.CodePtrRelocSlots.size());
  visitAbsoluteRelocationSources(Image, [&](va_t Target, va_t Slot) {
    Sources.emplace_back(Target, Slot);
  });
  std::sort(Sources.begin(), Sources.end());
}

bool detail::AbsoluteRelocationRootIndex::collectSources(
    const BinaryImage &Img, va_t Begin, va_t End,
    std::map<va_t, std::set<va_t>> &RootSources) const {
  return collectSources(Img, Begin, End, RootSources, false);
}

bool detail::AbsoluteRelocationRootIndex::collectOrdinarySources(
    const BinaryImage &Img, va_t Begin, va_t End,
    std::map<va_t, std::set<va_t>> &RootSources) const {
  return collectSources(Img, Begin, End, RootSources, true);
}

bool detail::AbsoluteRelocationRootIndex::collectSources(
    const BinaryImage &Img, va_t Begin, va_t End,
    std::map<va_t, std::set<va_t>> &RootSources, bool OrdinaryOnly) const {
  if (Image != &Img)
    return false;
  if (Begin >= End)
    return true;
  const auto First =
      std::upper_bound(Sources.begin(), Sources.end(), Begin,
                       [](va_t Address, const std::pair<va_t, va_t> &Source) {
                         return Address < Source.first;
                       });
  const auto Last =
      std::lower_bound(First, Sources.end(), End,
                       [](const std::pair<va_t, va_t> &Source, va_t Address) {
                         return Source.first < Address;
                       });
  if (First == Last)
    return true;
  // Raw ownership arbitration needs no runtime-role parsing. Publish this
  // immutable image-only result once, including a bounded refusal, before
  // concurrent ordinary-source queries inspect it.
  if (OrdinaryOnly)
    std::call_once(RuntimeRoles->Once, [&] {
      if (auto Roles = coff_loader::getCheckedX86RegistrationPointerRoles(Img))
        RuntimeRoles->Sources = std::move(Roles->Sources);
    });
  for (auto It = First; It != Last; ++It) {
    if (OrdinaryOnly) {
      const auto Runtime = RuntimeRoles->Sources.find(It->second);
      if (Runtime != RuntimeRoles->Sources.end() &&
          Runtime->second == It->first)
        continue;
    }
    RootSources[It->first].insert(It->second);
  }
  return true;
}

const detail::AbsoluteRelocationRootIndex &
CFGBuilder::absoluteRelocationRootIndex(const BinaryImage &Img) {
  if (AbsoluteRelocationRoots && AbsoluteRelocationRoots->ownsImage(Img))
    return *AbsoluteRelocationRoots;
  if (const auto *Roots =
          registration_abi::StackProofOwnerScope::relocationRoots(Img))
    return *Roots;
  if (BorrowedRegistrationCallees &&
      BorrowedRegistrationCallees->ownsImage(Img))
    return BorrowedRegistrationCallees->absoluteRelocationRoots();
  if (!LocalAbsoluteRelocationRoots)
    LocalAbsoluteRelocationRoots.emplace(Img);
  return *LocalAbsoluteRelocationRoots;
}

void CFGBuilder::establishCurrentFuncRange(const BinaryImage &Img,
                                           const ExceptionFunction *Exception) {
  CurrentFuncRange.reset();
  AuthoritativeCurrentFuncRange.reset();
  // Win64 requires an unwind record for every function that allocates stack
  // or saves a register; one without operations describes neither.  An
  // image with no exception directory at all proves nothing either way.
  const bool HasExceptionDirectory =
      !Img.ExceptionMetadata.Functions.empty() || !Img.COFFPDataRecords.empty();
  CurrentFuncIsFramelessLeaf =
      Img.Format == BinaryFormat::COFF && Img.Arch == Arch::X64 &&
      HasExceptionDirectory &&
      (!Exception ||
       (Exception->Kind == RuntimeFunctionKind::Primary &&
        Exception->CodeRange.Begin == CurrentFuncEntry &&
        Exception->ParseStatus == ExceptionParseStatus::Complete &&
        Exception->UnwindOperations.empty() && Exception->FrameRegister == 0));
  va_t End = InvalidVA;
  va_t AuthoritativeEnd = InvalidVA;
  auto ConsiderEnd = [&](va_t Candidate) {
    if (Candidate > CurrentFuncEntry && (End == InvalidVA || Candidate < End))
      End = Candidate;
  };
  auto ConsiderAuthoritativeEnd = [&](va_t Candidate) {
    if (Candidate <= CurrentFuncEntry)
      return;
    if (AuthoritativeEnd == InvalidVA || Candidate < AuthoritativeEnd)
      AuthoritativeEnd = Candidate;
    ConsiderEnd(Candidate);
  };

  // A containing unwind range can also contain a separately callable local
  // entry.  Only a primary record that starts here proves ownership.
  if (Exception && Exception->Kind == RuntimeFunctionKind::Primary &&
      Exception->CodeRange.Begin == CurrentFuncEntry)
    ConsiderAuthoritativeEnd(Exception->CodeRange.End);

  ConsiderAuthoritativeEnd(
      Img.getFunctionMetadataEnd(CurrentFuncEntry, ExecutableCodeOwners));

  // Even without sized metadata, the next independently detected entry is a
  // hard upper boundary.  It does not prove an unbounded last function.
  if (const va_t Next = nextKnownFunctionEntry(CurrentFuncEntry);
      Next != InvalidVA)
    ConsiderEnd(Next);

  if (End != InvalidVA)
    CurrentFuncRange = std::make_pair(CurrentFuncEntry, End);
  if (AuthoritativeEnd != InvalidVA)
    AuthoritativeCurrentFuncRange =
        std::make_pair(CurrentFuncEntry, AuthoritativeEnd);
}

bool CFGBuilder::isOwnedInteriorTarget(const BinaryImage &Img,
                                       va_t Target) const {
  if (!CurrentFuncRange || Target <= CurrentFuncRange->first ||
      Target >= CurrentFuncRange->second)
    return false;
  if (isKnownFunctionEntry(Target) && !isCurrentExceptionalEntry(Target) &&
      !isCurrentOwnedFragment(Target))
    return false;
  if ((Target % getInsnAlignment()) != 0 ||
      !Img.hasExecutableCodeOwnerAt(Target, ExecutableCodeOwners) ||
      !Img.hasExecutableCodeOwnerRange(Target, getInsnAlignment()) ||
      !Img.readVA(Target, 1))
    return false;
  // Never let an x86 data value that happens to point into an instruction
  // turn its suffix into a second decode stream.
  auto AtOrAfter = Insns.upper_bound(Target);
  if (AtOrAfter != Insns.begin()) {
    const auto &Prev = *std::prev(AtOrAfter);
    if (Prev.first < Target &&
        Target < Prev.first + static_cast<va_t>(Prev.second.Size))
      return false;
  }
  return true;
}

void CFGBuilder::exploreAddressTakenRoots(const BinaryImage &Img,
                                          Decoder &Dec) {
  if (!CurrentFuncRange)
    return;

  // Runtime metadata can name callbacks outside its installing function.
  // Those exact fields cannot seed an unrelated function whose conservative
  // decode envelope happens to contain the callback. The shared loader role
  // proof excludes only checked sources, preserving every ordinary alias.
  // Function ownership remains local and is checked after each graph extension.
  std::set<va_t> RelocationCandidates;
  std::map<va_t, std::set<va_t>> RelocationSources;
  absoluteRelocationRootIndex(Img).collectOrdinarySources(
      Img, CurrentFuncRange->first, CurrentFuncRange->second,
      RelocationSources);
  if (!RelocationSources.empty() && Img.isArm64e()) {
    // The old entry is just a branch to a separately owned body. A rebased
    // pointer into the bytes after it supplies an address, but no CFG edge
    // from that branch. Explicit control and exceptional roots remain live.
    const auto Body =
        machOLiftedSource(Img, CurrentFuncEntry, CurrentFuncRange->second);
    if (Body && Body->ForwardTarget &&
        (*Body->ForwardTarget < CurrentFuncEntry ||
         *Body->ForwardTarget >= CurrentFuncRange->second))
      RelocationSources.clear();
    else if (Body && !Body->ForwardTarget) {
      // A literal pointer explicitly returned by this CFG names original
      // storage. Remove only that slot's contribution to the candidate.
      // Another slot or a control-flow use remains independent evidence.
      constexpr auto Word = aarch64::kInsnSize;
      for (const auto &[PC, Record] : Insns) {
        if (Record.Size != Word || PC < CurrentFuncEntry ||
            !rangeInBounds(PC - CurrentFuncEntry, Word, Body->Bytes.size()))
          continue;
        const auto *At = Body->Bytes.data() + (PC - CurrentFuncEntry);
        const auto Encoding = readLE<uint32_t>(At);
        const auto Target = machORebasedLiteralAddress(Img, Encoding, PC);
        if (!Target)
          continue;
        const auto Candidate = RelocationSources.find(*Target);
        if (Candidate == RelocationSources.end())
          continue;
        const va_t Slot = aarch64::decodeLiteralLoad(Encoding, PC)->Address;
        if (!Candidate->second.contains(Slot))
          continue;
        DecodedInsn Load{};
        if (Dec.decodeOne(At, Word, PC, Load) == int(Word) && Load.Raw &&
            Load.Raw->detail &&
            darwinAddressValueEscapes(
                Dec, Body->Bytes, CurrentFuncEntry, PC + Word,
                Load.Raw->detail->aarch64.operands[0].reg))
          Candidate->second.erase(Slot);
      }
    }
  }
  const ExceptionFunction *Exception =
      Img.ExceptionMetadata.findFunction(CurrentFuncEntry);
  for (auto &[Target, Sources] : RelocationSources) {
    // A pointer table among the code is read by the function dispatching
    // through it, whose jump-table resolver recovers what it names; it roots
    // no function's code.  The range of a function before the dispatcher
    // reaches the dispatcher's cases when its entry is only a guess.
    std::erase_if(Sources, [&](va_t Slot) {
      return Img.hasExecutableCodeOwnerAt(Slot, ExecutableCodeOwners);
    });
    if (Exception && Exception->Registration &&
        (Exception->Encoding == ExceptionEncoding::X86ScopeTableEH3 ||
         Exception->Encoding == ExceptionEncoding::X86ScopeTableEH4))
      std::erase_if(Sources, [&](va_t Slot) {
        return Exception->Registration->scopePointerTarget(Slot) == Target;
      });
    if (!Sources.empty())
      RelocationCandidates.insert(Target);
  }

  std::set<va_t> Processed;
  for (;;) {
    std::set<va_t> Candidates;
    std::set<va_t> ProtectedRelativeRelocationCandidates;
    std::set<va_t> CrossFunctionContinuationCandidates;

    // Relative table entries are normally admitted only through their
    // authenticated selector coordinates.  Module arbitration can explicitly
    // protect a different physical slot, however; that slot must remain an
    // independent CFG root even when an exact sparse selector excludes it.
    // Decode only those protected sources through the width/format-qualified
    // cache.  Do not add them to PersistentCFGRoots here: doing so would let an
    // excluded physical slot participate in the selector proof.  Final block
    // formation joins the decoded boundary back to its protected source and
    // preserves it independently.  The ordinary owner and instruction-boundary
    // checks below still decide whether the target belongs to this function.
    if (ProtectedJumpTableRelocationSlots &&
        !ProtectedJumpTableRelocationSlots->empty()) {
      prepareRelativeRelocationRootSourceCache();
      for (const auto &[Target, Slot] : RelativeRelocationRootSourceCache)
        if (ProtectedJumpTableRelocationSlots->count(Slot))
          ProtectedRelativeRelocationCandidates.insert(Target);
    }
    Candidates.insert(RelocationCandidates.begin(), RelocationCandidates.end());
    Candidates.insert(ProtectedRelativeRelocationCandidates.begin(),
                      ProtectedRelativeRelocationCandidates.end());

    // These references were observed by this builder while decoding this
    // function and therefore may identify an owned local label directly.  Do
    // not substitute Img.CodeRefTargets: that image-global union carries no
    // current-function ownership or EH provenance.
    for (va_t Target : DiscoveredCodeRefs)
      Candidates.insert(normalizeCodeAddress(Target, Img.Arch, Img.Mode));

    // A separately emitted funclet can identify an otherwise unreachable
    // continuation in its parent.  The module EH closure supplies only roots
    // proven for this exact owner.  Recheck the authoritative body here; a
    // weaker next-entry bound cannot turn padding or an undetected function
    // into a disconnected local block.
    if (CrossFunctionContinuationRoots && AuthoritativeCurrentFuncRange)
      for (va_t Target : *CrossFunctionContinuationRoots) {
        Target = normalizeCodeAddress(Target, Img.Arch, Img.Mode);
        if (Target > AuthoritativeCurrentFuncRange->first &&
            Target < AuthoritativeCurrentFuncRange->second) {
          Candidates.insert(Target);
          CrossFunctionContinuationCandidates.insert(Target);
        }
      }

    bool Added = false;
    for (va_t Target : Candidates) {
      if (!Processed.insert(Target).second ||
          !isOwnedInteriorTarget(Img, Target))
        continue;

      BlockStarts.insert(Target);
      if (RelocationCandidates.count(Target)) {
        PersistentCFGRoots.insert(Target);
        OrdinaryCFGRoots.insert(Target);
        const auto Sources = RelocationSources.find(Target);
        if (Sources != RelocationSources.end())
          RelocationCFGRootSources[Target].insert(Sources->second.begin(),
                                                  Sources->second.end());
      }
      if (CrossFunctionContinuationCandidates.count(Target)) {
        // EH ownership and source-value proofs are established by the module
        // closure.  Once the local range and decode-boundary checks above also
        // pass, the continuation remains a disconnected root independently of
        // relocation-table arbitration.
        PersistentCFGRoots.insert(Target);
        OrdinaryCFGRoots.insert(Target);
        DurableCFGRoots.insert(Target);
      }
      if (!ExploredAddrs.count(Target))
        explore(Img, Dec, Target);
      Added = true;
    }
    if (!Added)
      break;
  }
}

namespace {

/// What a value computes toward a jump through a table of code addresses: a
/// constant, some number times a slot size, a table's address plus such a
/// number, the slot read there, that 32-bit slot sign-extended, or the
/// table's address plus that.  A value of no such form is not kept.
struct TableReadValue {
  enum Kind : uint8_t {
    Constant,
    Scaled,
    SlotAddress,
    Slot,
    ExtendedSlot,
    RelativeTarget
  };
  Kind K = Constant;
  /// The constant, or the table's address.
  uint64_t Value = 0;
  /// The slot size of Scaled, SlotAddress and Slot.
  uint16_t Bytes = 0;
  /// The LOAD that read the slot, for Slot, ExtendedSlot and RelativeTarget.
  va_t SlotReadAddr = InvalidVA;
  int SlotReadSeq = -1;
};

using TableReadKey = std::pair<bool, uint64_t>;

/// The storage a write to \p Value replaces part or all of.
std::optional<TableReadKey> tableReadStorage(const NdVar &Value) {
  if (Value.isTemp())
    return TableReadKey{false, Value.Offset};
  if (Value.isReg())
    return TableReadKey{true, Value.Offset & ~uint64_t{7}};
  return std::nullopt;
}

/// The values a run of LowIR ops computes toward a table read, op by op.
/// Each value is kept with the size it was written at; a register keeps one
/// only while all eight bytes of it were written together.  A call keeps
/// only the registers the callee preserves.
class TableReadState {
  using K = TableReadValue::Kind;
  std::map<TableReadKey, std::pair<TableReadValue, uint16_t>> Values;
  llvm::ArrayRef<uint64_t> Preserved;

  static bool isConstant(const std::optional<TableReadValue> &Value) {
    return Value && Value->K == K::Constant;
  }

public:
  explicit TableReadState(llvm::ArrayRef<uint64_t> Preserved)
      : Preserved(Preserved) {}

  /// Start with \p Constants in the whole registers they name.
  void seed(const std::map<uint64_t, uint64_t> &Constants) {
    for (const auto &[Register, Constant] : Constants)
      Values[TableReadKey{true, Register}] = {
          TableReadValue{K::Constant, Constant, 0}, 8};
  }

  /// The constants the whole registers hold.
  std::map<uint64_t, uint64_t> constants() const {
    std::map<uint64_t, uint64_t> Result;
    for (const auto &[Key, Value] : Values)
      if (Key.first && Value.first.K == K::Constant && Value.second == 8)
        Result.emplace(Key.second, Value.first.Value);
    return Result;
  }

  std::optional<TableReadValue> valueOf(const NdVar &Value) const {
    if (Value.isConst())
      return TableReadValue{K::Constant, Value.Offset, 0};
    const auto Key = tableReadStorage(Value);
    if (!Key || (Value.isReg() && Value.Offset % 8 != 0))
      return std::nullopt;
    const auto It = Values.find(*Key);
    if (It == Values.end() || It->second.second != Value.Size)
      return std::nullopt;
    return It->second.first;
  }

  // Every register a LowIR op writes is its output: an intrinsic's other
  // results are copied out by ops of their own.
  void step(const LowOp &Op) {
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
      for (auto It = Values.begin(); It != Values.end();)
        if (It->first.first && std::find(Preserved.begin(), Preserved.end(),
                                         It->first.second) != Preserved.end())
          ++It;
        else
          It = Values.erase(It);
      return;
    }
    const auto In = [&](unsigned Input) -> std::optional<TableReadValue> {
      return Input < Op.NumInputs ? valueOf(Op.Inputs[Input]) : std::nullopt;
    };
    std::optional<TableReadValue> Result;
    switch (Op.Opcode) {
    case NdOp::COPY:
      if (Op.NumInputs == 1)
        Result = In(0);
      break;
    case NdOp::INT_ADD: {
      if (Op.NumInputs != 2 || Op.Output.Size != 8)
        break;
      auto Sum = [&](const std::optional<TableReadValue> &Left,
                     const std::optional<TableReadValue> &Right)
          -> std::optional<TableReadValue> {
        if (!Left || !isConstant(Right))
          return std::nullopt;
        switch (Left->K) {
        case K::Constant:
          return TableReadValue{K::Constant, Left->Value + Right->Value, 0};
        case K::Scaled:
          return TableReadValue{K::SlotAddress, Right->Value, Left->Bytes};
        case K::ExtendedSlot:
          if (Left->Value == Right->Value) {
            TableReadValue Target = *Left;
            Target.K = K::RelativeTarget;
            return Target;
          }
          return std::nullopt;
        default:
          return std::nullopt;
        }
      };
      const auto Left = In(0), Right = In(1);
      Result = Sum(Left, Right);
      if (!Result)
        Result = Sum(Right, Left);
      break;
    }
    case NdOp::INT_MULT:
    case NdOp::INT_LEFT: {
      if (Op.NumInputs != 2 || Op.Output.Size != 8)
        break;
      const auto Left = In(0), Right = In(1);
      if (isConstant(Left) && isConstant(Right)) {
        if (Op.Opcode == NdOp::INT_MULT)
          Result = TableReadValue{K::Constant, Left->Value * Right->Value, 0};
        break;
      }
      auto Scale = [&](const std::optional<TableReadValue> &Factor)
          -> std::optional<TableReadValue> {
        if (!isConstant(Factor))
          return std::nullopt;
        const uint64_t Bytes =
            Op.Opcode == NdOp::INT_MULT
                ? Factor->Value
                : (Factor->Value < 8 ? uint64_t{1} << Factor->Value : 0);
        if (Bytes != sizeof(int32_t))
          return std::nullopt;
        return TableReadValue{K::Scaled, 0, static_cast<uint16_t>(Bytes)};
      };
      Result = Scale(Right);
      if (!Result && Op.Opcode == NdOp::INT_MULT)
        Result = Scale(Left);
      break;
    }
    case NdOp::LOAD: {
      const NdVar &Address = Op.NumInputs >= 2 ? Op.Inputs[1] : Op.Inputs[0];
      const auto At = valueOf(Address);
      if (At && At->K == K::SlotAddress && Op.Output.Size == At->Bytes)
        Result = TableReadValue{K::Slot, At->Value, At->Bytes, Op.Addr, Op.Seq};
      break;
    }
    case NdOp::INT_SEXT: {
      auto Slot = In(0);
      if (Slot && Slot->K == K::Slot && Slot->Bytes == 4 &&
          Op.Output.Size == 8) {
        Slot->K = K::ExtendedSlot;
        Result = Slot;
      }
      break;
    }
    default:
      break;
    }
    if (const auto Key = tableReadStorage(Op.Output)) {
      Values.erase(*Key);
      if (Result && (Op.Output.isTemp() ||
                     (Op.Output.Size == 8 && Op.Output.Offset % 8 == 0)))
        Values.emplace(*Key, std::make_pair(*Result, Op.Output.Size));
    }
  }
};

/// What the indirect jump ending the last of \p Chain's blocks jumps to,
/// with \p Entry in the registers as the first block starts.  Each block of
/// \p Chain is the only way into the next.
std::optional<TableReadValue>
tableReadTarget(const LowFunc &Func, const std::vector<size_t> &Chain,
                const std::map<uint64_t, uint64_t> &Entry,
                llvm::ArrayRef<uint64_t> Preserved) {
  TableReadState State(Preserved);
  State.seed(Entry);
  for (size_t Index : Chain) {
    const LowBlock &Block = Func.Blocks[Index];
    const bool Last = Index == Chain.back();
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const LowOp &Op = Block.Ops[I];
      if (Last && I + 1 == Block.Ops.size()) {
        if (Op.Opcode != NdOp::INDIR_BR || Op.NumInputs != 1)
          return std::nullopt;
        return State.valueOf(Op.Inputs[0]);
      }
      State.step(Op);
    }
  }
  return std::nullopt;
}

/// The constants every path into each block leaves in a whole register:
/// the address of a table hoisted out of a loop.  A block also entered
/// another way, as \p Root says, starts with none.
std::vector<std::map<uint64_t, uint64_t>>
blockEntryConstants(const LowFunc &Func, llvm::ArrayRef<uint64_t> Preserved,
                    const std::function<bool(const LowBlock &)> &Root) {
  const size_t Count = Func.Blocks.size();
  std::vector<std::map<uint64_t, uint64_t>> Entry(Count);
  std::vector<bool> Reached(Count, false);
  std::vector<size_t> Work;
  for (size_t B = 0; B < Count; ++B)
    if (Root(Func.Blocks[B])) {
      Reached[B] = true;
      Work.push_back(B);
    }
  while (!Work.empty()) {
    const size_t B = Work.back();
    Work.pop_back();
    TableReadState State(Preserved);
    State.seed(Entry[B]);
    for (const LowOp &Op : Func.Blocks[B].Ops)
      State.step(Op);
    const std::map<uint64_t, uint64_t> Out = State.constants();
    for (int S : Func.Blocks[B].Succs) {
      if (S < 0 || static_cast<size_t>(S) >= Count || Root(Func.Blocks[S]))
        continue;
      std::map<uint64_t, uint64_t> &In = Entry[S];
      if (!Reached[S]) {
        Reached[S] = true;
        In = Out;
        Work.push_back(S);
        continue;
      }
      bool Narrowed = false;
      for (auto It = In.begin(); It != In.end();) {
        const auto Other = Out.find(It->first);
        if (Other == Out.end() || Other->second != It->second) {
          It = In.erase(It);
          Narrowed = true;
        } else {
          ++It;
        }
      }
      if (Narrowed)
        Work.push_back(S);
    }
  }
  return Entry;
}

} // namespace

bool CFGBuilder::recoverOwnLabelTableBranches(const BinaryImage &Img,
                                              Decoder &Dec, LowFunc &Func) {
  if (Img.Arch != Arch::X64 || !CurrentFuncRange)
    return false;
  // The instruction starts of one linear decode of the function, made only
  // when a jump needs them.  They stand only while every instruction
  // recursive descent decoded there is one of them, which each round checks
  // again.  A label must be one: a slot past the table's end that happens to
  // point into the function must not start a second decode stream inside an
  // instruction.
  std::optional<std::set<va_t>> Starts;
  bool Swept = false;
  auto LinearStarts = [&]() -> const std::set<va_t> * {
    if (!Swept) {
      Swept = true;
      Decoder Scan;
      if (!Scan.init(Img))
        return nullptr;
      Scan.setDetail(false);
      std::set<va_t> Found;
      for (va_t Cur = CurrentFuncRange->first;
           Cur < CurrentFuncRange->second;) {
        if (Found.size() >= limits::kMaxOwnLabelTableSweepInstructions)
          return nullptr;
        const auto *Seg = Img.getSegmentFor(Cur);
        if (!Seg || Cur - Seg->VA >= Seg->Data.size())
          return nullptr;
        const size_t Offset = static_cast<size_t>(Cur - Seg->VA);
        DecodedInsn Insn;
        const int Size = Scan.decodeOneLight(
            Seg->Data.data() + Offset, Seg->Data.size() - Offset, Cur, Insn);
        if (Size <= 0)
          return nullptr;
        Found.insert(Cur);
        Cur += static_cast<va_t>(Size);
      }
      Starts = std::move(Found);
    }
    if (!Starts)
      return nullptr;
    for (auto It = Insns.lower_bound(CurrentFuncRange->first);
         It != Insns.end() && It->first < CurrentFuncRange->second; ++It)
      if (!Starts->count(It->first)) {
        Starts.reset();
        return nullptr;
      }
    return &*Starts;
  };

  const llvm::ArrayRef<uint64_t> Preserved =
      getTargetRegInfo(Img.Arch).CalleeSaveRegs;
  // A block entered another way than from its predecessors starts with no
  // known register.
  auto Root = [&](const LowBlock &Block) {
    return Block.Preds.empty() || !Block.ExceptionalPreds.empty() ||
           Block.StartAddr == Func.Entry ||
           OrdinaryCFGRoots.count(Block.StartAddr) ||
           isKnownFunctionEntry(Block.StartAddr);
  };

  // The table read of the jump ending block \p B on the graph as it stands:
  // its block after the blocks each the only way into the next, starting
  // with what every path leaves in the registers.
  using EntryState = std::vector<std::map<uint64_t, uint64_t>>;
  auto ReadTarget =
      [&](size_t B, const EntryState &Entry) -> std::optional<TableReadValue> {
    std::vector<size_t> Chain{B};
    while (Chain.size() <= limits::kMaxOwnLabelTableReadBlocks) {
      const LowBlock &Into = Func.Blocks[Chain.back()];
      if (Into.Preds.size() != 1 || Into.StartAddr == Func.Entry)
        break;
      const int Pred = Into.Preds.front();
      if (Pred < 0 || static_cast<size_t>(Pred) >= Func.Blocks.size() ||
          std::find(Chain.begin(), Chain.end(), static_cast<size_t>(Pred)) !=
              Chain.end())
        break;
      Chain.push_back(static_cast<size_t>(Pred));
    }
    std::reverse(Chain.begin(), Chain.end());
    const auto Target =
        tableReadTarget(Func, Chain, Entry[Chain.front()], Preserved);
    if (!Target || Target->K != TableReadValue::RelativeTarget)
      return std::nullopt;
    return Target;
  };
  // A dispatch stands only while the graph still reads its table the same
  // way: code its labels reach can enter its blocks another way, with another
  // address in the register.  One withdrawn is not tried again.
  std::set<va_t> Withdrawn;
  auto Revalidate = [&] {
    for (bool Changed = true; Changed;) {
      Changed = false;
      const EntryState Entry = blockEntryConstants(Func, Preserved, Root);
      for (size_t B = 0; B < Func.Blocks.size(); ++B) {
        const LowBlock &Block = Func.Blocks[B];
        if (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::INDIR_BR)
          continue;
        const va_t Address = Block.Ops.back().Addr;
        const auto Read = OwnLabelTableReads.find(Address);
        if (Read == OwnLabelTableReads.end())
          continue;
        const auto Target = ReadTarget(B, Entry);
        if (Target && Target->Value == Read->second.Base &&
            Target->SlotReadAddr == Read->second.Slot.Addr &&
            Target->SlotReadSeq == Read->second.Slot.Seq)
          continue;
        InsnRecord &Rec = Insns.at(Address);
        Rec.JumpTableTargets.clear();
        Rec.ValueDispatch = false;
        OwnLabelTableReads.erase(Read);
        Withdrawn.insert(Address);
        Changed = true;
      }
      if (Changed)
        rebuildBlocks(Func);
    }
  };

  // Each round dispatches the jumps the code decoded so far holds; the code
  // their labels reach can hold another (a `match` after a `match`).
  bool Recovered = false;
  for (uint32_t Round = 0; Round < limits::kMaxOwnLabelTableRounds; ++Round) {
    const std::set<va_t> *Lines = nullptr;
    bool Checked = false;
    std::optional<EntryState> EntryConstants;
    std::vector<va_t> Explore;
    for (size_t B = 0; B < Func.Blocks.size(); ++B) {
      const LowBlock &Block = Func.Blocks[B];
      if (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::INDIR_BR)
        continue;
      const va_t Address = Block.Ops.back().Addr;
      const auto Found = Insns.find(Address);
      if (Found == Insns.end())
        continue;
      InsnRecord &Rec = Found->second;
      // An index domain whose evidence ran out leaves the jump unproved, and
      // a dispatch by value claims no domain.  A jump whose table evidence or
      // whose function's marker evidence ran out stays opaque.
      if (!Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall || Rec.IsRet ||
          Rec.IsCond || !Rec.JumpTableTargets.empty() ||
          Rec.ScalarBranchInput ||
          LostValidatedJumpTableBranches.count(Address) ||
          StackTableEvidenceIncompleteBranches.count(Address) ||
          IncompleteBranchMarkerEvidenceIncomplete || Withdrawn.count(Address))
        continue;
      if (!EntryConstants)
        EntryConstants = blockEntryConstants(Func, Preserved, Root);
      const auto Target = ReadTarget(B, *EntryConstants);
      if (!Target)
        continue;
      const va_t Base = Target->Value;
      const Section *Holder = Img.getSectionFor(Base);
      if (!Holder || Holder->isWritable() || Holder->isExecutable())
        continue;
      if (!Checked) {
        Checked = true;
        Lines = LinearStarts();
      }
      if (!Lines)
        return Recovered;
      // The leading slots that name code of this function.  Nothing proves
      // how many slots the table has, so the first slot that names anything
      // else ends it.
      std::vector<va_t> Targets;
      std::set<va_t> Distinct;
      for (uint64_t Slot = 0; Slot < limits::kMaxJumpTableEntries; ++Slot) {
        if (Slot * sizeof(int32_t) > InvalidVA - sizeof(int32_t) - Base)
          break;
        const va_t At = Base + Slot * sizeof(int32_t);
        if (Img.getSectionFor(At) != Holder ||
            Img.hasRuntimeCallablePointerSlotAt(At))
          break;
        const uint8_t *Bytes = Img.readVA(At, sizeof(int32_t));
        if (!Bytes)
          break;
        int32_t Offset = 0;
        std::memcpy(&Offset, Bytes, sizeof(Offset));
        const va_t Label =
            Base + static_cast<uint64_t>(static_cast<int64_t>(Offset));
        if (!Lines->count(Label) || !isOwnedInteriorTarget(Img, Label))
          break;
        if (Distinct.insert(Label).second)
          Targets.push_back(Label);
      }
      if (Targets.empty())
        continue;
      Rec.JumpTableTargets = Targets;
      Rec.ValueDispatch = true;
      ResolvedTableInfo.erase(Address);
      OwnLabelTableReads[Address] = {
          Base,
          JumpTableSelectorUseRef{
              Target->SlotReadAddr, Target->SlotReadSeq, NdOp::LOAD,
              JumpTableSelectorUseRef::ValueRole::Output, 0, Target->Bytes}};
      Explore.insert(Explore.end(), Targets.begin(), Targets.end());
    }
    if (Explore.empty())
      break;
    Recovered = true;
    for (va_t Label : Explore) {
      BlockStarts.insert(Label);
      if (!ExploredAddrs.count(Label))
        explore(Img, Dec, Label);
    }
    splitBlocks();
    rebuildBlocks(Func);
    Revalidate();
  }
  return Recovered;
}

void CFGBuilder::dispatchUnprovedBranchesByValue(const BinaryImage &Img,
                                                 LowFunc &Func) {
  if (!CurrentFuncRange)
    return;
  // The jumps recoverOwnLabelTableBranches gave the labels of the table they
  // read.  Those edges stay only where the check below admits them, and every
  // way out before it withdraws them.
  std::vector<va_t> TableJumps;
  for (const auto &[Addr, Rec] : Insns)
    if (Rec.ValueDispatch && !Rec.JumpTableTargets.empty() &&
        OwnLabelTableReads.count(Addr))
      TableJumps.push_back(Addr);
  auto WithdrawTableJumps = [&] {
    for (va_t Addr : TableJumps) {
      InsnRecord &Rec = Insns.at(Addr);
      Rec.JumpTableTargets.clear();
      Rec.ValueDispatch = false;
      OwnLabelTableReads.erase(Addr);
    }
    if (!TableJumps.empty())
      rebuildBlocks(Func);
  };
  // The labels a jump of this function can name: the blocks whose addresses
  // relocation-proven pointers outside the code hold.  They are roots of this
  // function already, entered with its frame.
  std::vector<va_t> Labels;
  for (const auto &[Target, Sources] : RelocationCFGRootSources)
    if (!Sources.empty() && OrdinaryCFGRoots.count(Target) &&
        Insns.count(Target) && isOwnedInteriorTarget(Img, Target))
      Labels.push_back(Target);
  if (Labels.size() > static_cast<size_t>(limits::kMaxJumpTableEntries))
    Labels.clear();
  std::vector<va_t> Jumps;
  if (!Labels.empty())
    for (const auto &[Addr, Rec] : Insns)
      if (Rec.IsBranch && Rec.IsIndirect && !Rec.IsCall && !Rec.IsRet &&
          !Rec.IsCond && Rec.JumpTableTargets.empty() && !Rec.ScalarBranchInput)
        Jumps.push_back(Addr);
  // The jumps before UnprovedJumps may take any of the first RootLabels
  // labels; a table jump after them takes only the labels its table holds.
  const size_t UnprovedJumps = Jumps.size();
  const size_t RootLabels = Labels.size();
  std::map<va_t, size_t> LabelIndex;
  for (size_t L = 0; L < Labels.size(); ++L)
    LabelIndex.emplace(Labels[L], L);
  for (va_t Addr : TableJumps) {
    Jumps.push_back(Addr);
    for (va_t Target : Insns.at(Addr).JumpTableTargets)
      if (LabelIndex.emplace(Target, Labels.size()).second)
        Labels.push_back(Target);
  }
  if (Jumps.empty())
    return;

  // Every label is a candidate of every jump, which says more than any one
  // jump can do: a table has entries its index never selects, and another
  // jump's table is not this one's.  An edge the program cannot take must not
  // make the function read what it never wrote, or a register its caller
  // keeps would become one of its inputs.  So an edge is kept only where
  // every such register the label's code reads first has been written on
  // every path to the jump.  A jump to a label left out traps, as every jump
  // of this kind did before.
  const auto &TRI = getTargetRegInfo(Img.Arch);
  const llvm::ArrayRef<uint64_t> Kept = TRI.CalleeSaveRegs;
  if (Kept.size() > 64) {
    WithdrawTableJumps();
    return;
  }
  const uint16_t Pointer = TRI.PointerSize;
  // A write of four bytes or more to a 64-bit register sets all of it.
  const uint16_t WholeWrite = Pointer == 8 ? 4 : Pointer;
  auto Bits = [&](const NdVar &Value, bool Whole) {
    uint64_t Mask = 0;
    if (!Value.isReg() || Value.Size == 0)
      return Mask;
    for (size_t I = 0; I < Kept.size(); ++I) {
      const uint64_t Base = Kept[I];
      if (Whole ? Value.Offset == Base && Value.Size >= WholeWrite
                : Value.Offset < Base + Pointer &&
                      Base < Value.Offset + Value.Size)
        Mask |= uint64_t{1} << I;
    }
    return Mask;
  };
  const size_t Count = Func.Blocks.size();
  std::map<va_t, size_t> BlockAt;
  std::vector<uint64_t> Reads(Count, 0), Writes(Count, 0);
  for (size_t B = 0; B < Count; ++B) {
    const LowBlock &Block = Func.Blocks[B];
    BlockAt.emplace(Block.StartAddr, B);
    for (const LowOp &Op : Block.Ops) {
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Reads[B] |= Bits(Op.Inputs[I], false) & ~Writes[B];
      Writes[B] |= Bits(Op.Output, true);
    }
  }
  std::vector<size_t> LabelBlocks;
  for (va_t Label : Labels) {
    const auto At = BlockAt.find(Label);
    if (At == BlockAt.end()) {
      WithdrawTableJumps();
      return;
    }
    LabelBlocks.push_back(At->second);
  }
  std::vector<size_t> JumpBlocks;
  for (va_t Jump : Jumps) {
    size_t Found = Count;
    for (size_t B = 0; B < Count && Found == Count; ++B)
      if (!Func.Blocks[B].Ops.empty() && Func.Blocks[B].Ops.back().Addr == Jump)
        Found = B;
    if (Found == Count) {
      WithdrawTableJumps();
      return;
    }
    JumpBlocks.push_back(Found);
  }
  size_t Entry = Count;
  if (const auto At = BlockAt.find(Func.Entry); At != BlockAt.end())
    Entry = At->second;
  if (Entry == Count) {
    WithdrawTableJumps();
    return;
  }
  // A table jump's block leaves only by the edges decided below.
  std::vector<bool> TableJumpBlock(Count, false);
  for (size_t J = UnprovedJumps; J < Jumps.size(); ++J)
    TableJumpBlock[JumpBlocks[J]] = true;

  // A jump whose table is known can take only what that table holds: the
  // label slots from where it starts to the next address another field of
  // the code names, which starts another table.  The table is the one an
  // exact field names, or the one a frame copy the jump reads was made from.
  std::vector<std::vector<bool>> Allowed(
      Jumps.size(), std::vector<bool>(Labels.size(), false));
  for (size_t J = 0; J < UnprovedJumps; ++J)
    for (size_t L = 0; L < RootLabels; ++L)
      Allowed[J][L] = true;
  for (size_t J = UnprovedJumps; J < Jumps.size(); ++J)
    for (va_t Target : Insns.at(Jumps[J]).JumpTableTargets)
      Allowed[J][LabelIndex.at(Target)] = true;
  std::optional<std::set<va_t>> Anchors;
  for (size_t J = 0; J < UnprovedJumps; ++J) {
    std::optional<va_t> Table =
        jumpReadsOwnLabelsThroughI386GOTOFF(Insns.at(Jumps[J]));
    if (!Table)
      if (const auto Copied = FrameLabelTableSources.find(Jumps[J]);
          Copied != FrameLabelTableSources.end())
        Table = Copied->second;
    if (!Table)
      continue;
    if (!Anchors) {
      Anchors.emplace();
      for (const auto &[Field, Operand] : Img.DataAddressRelocOperands)
        Anchors->insert(Operand.TargetVA);
    }
    std::set<va_t> Held;
    for (va_t Slot = *Table;
         Held.size() <= static_cast<size_t>(limits::kMaxJumpTableEntries) &&
         Img.CodePtrRelocSlots.count(Slot) &&
         (Slot == *Table || !Anchors->count(Slot)) &&
         Slot <= InvalidVA - Pointer;
         Slot += Pointer) {
      const auto Target = normalizedRelocatedCodeTarget(Img, Slot);
      if (!Target)
        break;
      Held.insert(*Target);
    }
    for (size_t L = 0; L < RootLabels; ++L)
      Allowed[J][L] = Held.count(Labels[L]) != 0;
  }

  // Edges[J][L]: jump J reaches label L.
  std::vector<std::vector<bool>> Edges(Jumps.size(),
                                       std::vector<bool>(Labels.size(), false));
  std::vector<bool> Reached, Decided(Jumps.size(), false);
  std::vector<uint64_t> Written, Needed;
  auto Successors = [&](size_t B, auto Visit) {
    if (!TableJumpBlock[B])
      for (int S : Func.Blocks[B].Succs)
        if (S >= 0 && static_cast<size_t>(S) < Count)
          Visit(static_cast<size_t>(S));
    for (size_t J = 0; J < Jumps.size(); ++J)
      if (JumpBlocks[J] == B)
        for (size_t L = 0; L < Labels.size(); ++L)
          if (Edges[J][L])
            Visit(LabelBlocks[L]);
  };
  // Written[B]: the registers every path from the entry has written when B
  // ends.  Needed[B]: those some path from B's start reads before a write.
  auto Solve = [&] {
    Reached.assign(Count, false);
    Written.assign(Count, ~uint64_t{0});
    std::vector<uint64_t> AtStart(Count, ~uint64_t{0});
    std::vector<size_t> Work{Entry};
    Reached[Entry] = true;
    AtStart[Entry] = 0;
    Written[Entry] = Writes[Entry];
    while (!Work.empty()) {
      const size_t B = Work.back();
      Work.pop_back();
      Successors(B, [&](size_t S) {
        const uint64_t Meet = AtStart[S] & Written[B];
        if (Reached[S] && Meet == AtStart[S])
          return;
        Reached[S] = true;
        AtStart[S] = Meet;
        Written[S] = Meet | Writes[S];
        Work.push_back(S);
      });
    }
    Needed.assign(Count, 0);
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (size_t B = Count; B-- > 0;) {
        uint64_t After = 0;
        Successors(B, [&](size_t S) { After |= Needed[S]; });
        const uint64_t Now = Reads[B] | (After & ~Writes[B]);
        if (Now != Needed[B]) {
          Needed[B] = Now;
          Changed = true;
        }
      }
    }
  };
  // Decide the jumps in the order control reaches them, each with the edges
  // decided before it.
  for (bool Progress = true; Progress;) {
    Progress = false;
    Solve();
    for (size_t J = 0; J < Jumps.size(); ++J) {
      if (Decided[J] || !Reached[JumpBlocks[J]])
        continue;
      for (size_t L = 0; L < Labels.size(); ++L)
        Edges[J][L] = Allowed[J][L] &&
                      (Needed[LabelBlocks[L]] & ~Written[JumpBlocks[J]]) == 0;
      Decided[J] = true;
      Progress = true;
      break;
    }
  }
  // An edge decided early can fail once later ones exist: drop it.  Each
  // removal only makes the remaining ones easier to keep.
  for (bool Removed = true; Removed;) {
    Removed = false;
    Solve();
    for (size_t J = 0; J < Jumps.size(); ++J)
      for (size_t L = 0; L < Labels.size(); ++L)
        if (Edges[J][L] &&
            (!Reached[JumpBlocks[J]] ||
             (Needed[LabelBlocks[L]] & ~Written[JumpBlocks[J]]) != 0)) {
          Edges[J][L] = false;
          Removed = true;
        }
  }

  bool Changed = false;
  for (size_t J = 0; J < Jumps.size(); ++J) {
    std::vector<va_t> Targets;
    for (size_t L = 0; L < Labels.size(); ++L)
      if (Edges[J][L])
        Targets.push_back(Labels[L]);
    InsnRecord &Rec = Insns.at(Jumps[J]);
    if (J >= UnprovedJumps) {
      // A table jump keeps the labels admitted, and traps on every value
      // when none is.
      if (Targets == Rec.JumpTableTargets)
        continue;
      Rec.ValueDispatch = !Targets.empty();
      if (Targets.empty())
        OwnLabelTableReads.erase(Jumps[J]);
      Rec.JumpTableTargets = std::move(Targets);
      Changed = true;
      continue;
    }
    if (Targets.empty())
      continue;
    Rec.JumpTableTargets = std::move(Targets);
    Rec.ValueDispatch = true;
    ResolvedTableInfo.erase(Jumps[J]);
    Changed = true;
  }
  if (Changed)
    rebuildBlocks(Func);
}

bool CFGBuilder::resolveRelocatedInteriorBranch(const BinaryImage &Img,
                                                InsnRecord &Rec) {
  if (!CurrentFuncRange || !Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall ||
      Rec.IsCond || Rec.IsRet || Img.getPointerSize() == 0)
    return false;

  auto BI = BlockStarts.upper_bound(Rec.Addr);
  if (BI == BlockStarts.begin())
    return false;
  --BI;
  const va_t BranchBlockStart = *BI;

  std::set<va_t> Visited{BranchBlockStart};
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);
  std::sort(Preds.begin(), Preds.end());
  Preds.erase(std::unique(Preds.begin(), Preds.end()), Preds.end());
  if (Preds.size() > 1)
    return false;

  std::vector<LowOp> Ops = collectPathOps(BranchBlockStart, Rec.Addr);
  int BranchIdx = -1;
  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
    if (Ops[I].Opcode == NdOp::INDIR_BR && Ops[I].NumInputs >= 1) {
      BranchIdx = I;
      break;
    }
  if (BranchIdx < 0)
    return false;

  auto traceLoad = [&](NdVar Value, int From) -> int {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!Value.isReg() && !Value.isTemp())
        return -1;
      int D = reachingDefIdx(Ops, From, Value);
      if (D < 0)
        return -1;
      const LowOp &Def = Ops[D];
      if (Def.Opcode == NdOp::LOAD)
        return D;
      if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT ||
           Def.Opcode == NdOp::INT_SEXT ||
           (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs >= 2 &&
            Def.Inputs[1].isConst() && Def.Inputs[1].Offset == 0)) &&
          Def.NumInputs >= 1) {
        Value = Def.Inputs[0];
        From = D - 1;
        continue;
      }
      return -1;
    }
    return -1;
  };

  const auto &TRI = getTargetRegInfo(Img.Arch);
  int ValueLoad = traceLoad(Ops[BranchIdx].Inputs[0], BranchIdx - 1);
  if (ValueLoad < 0)
    return false;

  auto loadAddress = [&](const LowOp &Load) -> const NdVar * {
    if (Load.Opcode != NdOp::LOAD || Load.NumInputs < 1)
      return nullptr;
    return &Load.Inputs[Load.NumInputs >= 2 ? 1 : 0];
  };

  // A branch usually reloads the target from a frame slot.  Forward exactly
  // the last dominating store; an unknown intervening store or call makes the
  // value ambiguous and therefore ineligible for folding.
  const NdVar *FirstAddr = loadAddress(Ops[ValueLoad]);
  if (!FirstAddr)
    return false;
  uint64_t SlotBase = InvalidVA;
  int64_t SlotOff = 0;
  if (frameSlotKey(Ops, ValueLoad - 1, *FirstAddr, TRI, SlotBase, SlotOff)) {
    int StoreIdx = -1;
    for (int I = ValueLoad - 1; I >= 0; --I) {
      const LowOp &Candidate = Ops[I];
      if (Candidate.Opcode == NdOp::CALL ||
          Candidate.Opcode == NdOp::INDIR_CALL)
        return false;
      if (Candidate.Opcode != NdOp::STORE || Candidate.NumInputs < 2)
        continue;
      uint64_t Base = InvalidVA;
      int64_t Off = 0;
      if (!frameSlotKey(Ops, I - 1, Candidate.Inputs[0], TRI, Base, Off))
        return false; // a potentially aliasing store on the dominating path
      if (Base == SlotBase && Off == SlotOff) {
        StoreIdx = I;
        break;
      }
    }
    if (StoreIdx < 0)
      return false;
    ValueLoad = traceLoad(Ops[StoreIdx].Inputs[1], StoreIdx - 1);
    if (ValueLoad < 0)
      return false;
  }

  const LowOp &SourceLoad = Ops[ValueLoad];
  if (SourceLoad.Output.Size != Img.getPointerSize())
    return false;
  const NdVar *SourceAddr = loadAddress(SourceLoad);
  if (!SourceAddr)
    return false;

  std::function<std::optional<uint64_t>(NdVar, int, int)> foldValue =
      [&](NdVar Value, int From, int Depth) -> std::optional<uint64_t> {
    if (Depth > limits::kMaxQuasiCopyDepth)
      return std::nullopt;
    if (Value.isConst())
      return Value.Offset;
    if (!Value.isReg() && !Value.isTemp())
      return std::nullopt;
    int D = reachingDefIdx(Ops, From, Value);
    if (D < 0)
      return std::nullopt;
    const LowOp &Def = Ops[D];
    if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT ||
         Def.Opcode == NdOp::INT_SEXT ||
         (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs >= 2 &&
          Def.Inputs[1].isConst() && Def.Inputs[1].Offset == 0)) &&
        Def.NumInputs >= 1)
      return foldValue(Def.Inputs[0], D - 1, Depth + 1);
    if ((Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB) &&
        Def.NumInputs >= 2) {
      auto A = foldValue(Def.Inputs[0], D - 1, Depth + 1);
      auto B = foldValue(Def.Inputs[1], D - 1, Depth + 1);
      if (!A || !B)
        return std::nullopt;
      return Def.Opcode == NdOp::INT_ADD ? *A + *B : *A - *B;
    }
    return std::nullopt;
  };

  auto SlotVA = foldValue(*SourceAddr, ValueLoad - 1, 0);
  if (!SlotVA || !Img.CodePtrRelocSlots.count(*SlotVA))
    return false;
  if (!Img.readOnlyAfterRelocations(*SlotVA, Img.getPointerSize()))
    return false;

  const uint8_t *P = Img.readVA(*SlotVA, Img.getPointerSize());
  if (!P)
    return false;
  const va_t RawTarget =
      static_cast<va_t>(readPtr(P, Img.is64Bit(), archLittleEndian(Img.Arch)));
  va_t Target = normalizeCodeAddress(RawTarget, Img.Arch, Img.Mode);
  std::optional<InstructionMode> MIPSMode;
  if (hasCompressedMIPSCode(Img.Mode)) {
    auto Destination = canonicalizeImageControlTarget(
        Img, Rec.Mode, Rec.TargetMode, RawTarget);
    if (!Destination) {
      llvm::consumeError(Destination.takeError());
      return false;
    }
    if (Img.instructionModeAt(Destination->Address, Destination->Mode) !=
        Destination->Mode)
      return false;
    Target = Destination->Address;
    MIPSMode = Destination->Mode;
  }
  if (!isOwnedInteriorTarget(Img, Target))
    return false;

  for (LowOp &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      const uint16_t Width =
          Op.Inputs[0].Size ? Op.Inputs[0].Size : Img.getPointerSize();
      Op.Opcode = NdOp::BRANCH;
      Op.Inputs[0] = NdVar::cst(Target, Width);
      Rec.IsIndirect = false;
      Rec.BranchTarget = Target;
      Rec.Immediate = Target;
      if (Img.Arch == Arch::ARM)
        Rec.TargetMode = (RawTarget & 1) ? LowInstructionTargetMode::Thumb
                                         : LowInstructionTargetMode::ARM;
      else if (MIPSMode)
        Rec.TargetMode = *MIPSMode == InstructionMode::MIPS16
                             ? LowInstructionTargetMode::MIPS16
                         : *MIPSMode == InstructionMode::MicroMIPS
                             ? LowInstructionTargetMode::MicroMIPS
                             : LowInstructionTargetMode::MIPSClassic;
      else
        Rec.TargetMode = LowInstructionTargetMode::Preserve;
      return true;
    }
  return false;
}

} // namespace neverd
