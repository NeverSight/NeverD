//===- SEHFilterValues.cpp - SEH filters as __except expressions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SEHFilterValues.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace neverd::seh_filter {

const HighExpr *peelViews(const HighExpr *E) {
  for (unsigned Depth = 0; E && Depth <= limits::kMaxIntegerViewUnwrapDepth;
       ++Depth) {
    const bool View =
        E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast ||
        (E->Kind == ExprKind::UnaryOp &&
         (E->Op == NdOp::CAST || E->Op == NdOp::INT_ZEXT ||
          E->Op == NdOp::INT_SEXT)) ||
        (E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
         E->Operands.size() == 2 && E->Operands[1] &&
         E->Operands[1]->Kind == ExprKind::Const && !E->Operands[1]->ConstVal);
    if (!View || E->Operands.empty())
      return E;
    E = E->Operands[0].get();
  }
  return E;
}

bool callsNamed(const HighExpr *E, llvm::StringRef Name) {
  return E && E->Kind == ExprKind::Call && E->CallTarget == Name;
}

ExprPtr callOfRuntime(llvm::StringRef Name, TypeRef Type) {
  ExprPtr Call = HighExpr::makeCall(Name.str(), 0, {});
  // A pointer in <excpt.h>, which casts to the word the machine holds it as
  // wherever that is what something reads.
  Call->Type = Name == kExceptionInformation ? NdType::makePtr() : Type;
  return Call;
}

ExprPtr keepViewValid(ExprPtr E) {
  if (E->Kind == ExprKind::BitCast && E->Type && E->Operands.size() == 1 &&
      E->Operands[0] && E->Operands[0]->Type &&
      E->Type->Size != E->Operands[0]->Type->Size) {
    E->Kind = ExprKind::Cast;
    E->CastTo = E->Type;
  }
  return E;
}

bool isEntryStackPointer(const HighExpr &E, Arch TargetArch) {
  return E.Kind == ExprKind::Var && E.Var.Kind == MedVar::Reg &&
         E.Var.SSAVer == 0 &&
         E.Var.RegOff == getTargetRegInfo(TargetArch).StackPointer;
}

std::optional<int64_t> frameDistance(const HighExpr *E,
                                     const std::map<VarKey, int64_t> &Frames,
                                     Arch TargetArch) {
  E = peelViews(E);
  if (!E)
    return std::nullopt;
  if (E->Kind == ExprKind::Var) {
    if (isEntryStackPointer(*E, TargetArch))
      return 0;
    const auto It = Frames.find(varKey(E->Var));
    return It == Frames.end() ? std::nullopt
                              : std::optional<int64_t>(It->second);
  }
  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
      (E->Op != NdOp::INT_ADD && E->Op != NdOp::INT_SUB))
    return std::nullopt;
  const HighExpr *Step = peelViews(E->Operands[1].get());
  const HighExpr *From = E->Operands[0].get();
  // A sum can hold its constant first, as `[edx + eax - 0x18]` with EDX known.
  if (E->Op == NdOp::INT_ADD && (!Step || Step->Kind != ExprKind::Const)) {
    From = E->Operands[1].get();
    Step = peelViews(E->Operands[0].get());
  }
  if (!Step || Step->Kind != ExprKind::Const)
    return std::nullopt;
  const auto Base = frameDistance(From, Frames, TargetArch);
  if (!Base)
    return std::nullopt;
  const int64_t Delta = llvm::SignExtend64<32>(Step->ConstVal);
  return E->Op == NdOp::INT_ADD ? *Base + Delta : *Base - Delta;
}

unsigned ExceptionReads::depthOf(const HighExpr *E) const {
  E = peelViews(E);
  if (!E)
    return 0;
  if (callsNamed(E, kExceptionInformation))
    return 1;
  if (E->Kind == ExprKind::Var) {
    const auto It = Depths.find(varKey(E->Var));
    return It == Depths.end() ? 0 : It->second;
  }
  if (E->Kind != ExprKind::Load || E->Operands.empty())
    return 0;
  return depthOf(E->Operands[0].get()) == 1 ? 2 : 0;
}

void ExceptionReads::noteAssign(const HighExpr &Dst, const HighExpr &Value) {
  if (Dst.Kind != ExprKind::Var)
    return;
  if (const unsigned Depth = depthOf(&Value))
    Depths[varKey(Dst.Var)] = Depth;
}

ExprPtr ExceptionReads::rewrite(const ExprPtr &E) const {
  return rewrite(E, limits::kMaxCExprPrintDepth);
}

ExprPtr ExceptionReads::rewrite(const ExprPtr &E, unsigned Budget) const {
  if (!E || !Budget)
    return E;
  if (E->Kind == ExprKind::Load && !E->Operands.empty() &&
      E->MemoryOrdering == NdMemoryOrdering::None &&
      E->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
    const auto Word = frameDistance(E->Operands[0].get(), Frames, TargetArch);
    if (Word && Slot && *Word == *Slot)
      return callOfRuntime(kExceptionInformation, E->Type);
    if (Word && CodeSlots && CodeSlots->count(*Word))
      return callOfRuntime(kExceptionCode, E->Type);
    if (E->Type && E->Type->Size == sizeof(int32_t) &&
        depthOf(E->Operands[0].get()) == 2)
      return callOfRuntime(kExceptionCode, E->Type);
  }
  ExprPtr Result = E;
  for (size_t I = 0; I < E->Operands.size(); ++I) {
    ExprPtr Operand = rewrite(E->Operands[I], Budget - 1);
    if (Operand == E->Operands[I])
      continue;
    if (Result == E)
      Result = std::make_shared<HighExpr>(*E);
    Result->Operands[I] = std::move(Operand);
  }
  return Result == E ? Result : keepViewValid(std::move(Result));
}

namespace {

const std::map<VarKey, int64_t> NoFrames;

/// One expression for a filter body: its assignments folded into what reads
/// them and its branches into conditional expressions.
class FilterFolder {
public:
  /// Whether a write to the frame word at a distance from the entry stack
  /// pointer is the filter's own business: it says whether the store can go,
  /// and takes the value in case that makes it GetExceptionCode()'s.
  using WriteIsPrivate = std::function<bool(int64_t, const HighExpr &)>;

  explicit FilterFolder(const FilterFrame &Frame)
      : Frames(Frame.Frames ? *Frame.Frames : NoFrames),
        TargetArch(Frame.TargetArch),
        Reads(Frames, Frame.PointersSlot, Frame.TargetArch),
        IsPrivate(Frame.IsPrivate), StackFloor(Frame.StackFloor) {}

  ExprPtr fold(const std::vector<HighStmt> &Body) {
    indexLabels(Body, nullptr);
    walkStmts(Body, [&](const HighStmt &S) {
      forEachExpr(const_cast<HighStmt &>(S), [&](ExprPtr &E) {
        if (E && &E != &S.Dst)
          countReads(*E);
      });
    });
    return foldList(Body, 0, nullptr);
  }

private:
  struct Rest {
    const std::vector<HighStmt> *List;
    size_t From;
    const Rest *Next;
  };

  const std::map<VarKey, int64_t> &Frames;
  Arch TargetArch;
  ExceptionReads Reads;
  WriteIsPrivate IsPrivate;
  /// Where the frame's own stack ends: a write below it is a call's argument.
  std::optional<int64_t> StackFloor;
  std::map<VarKey, ExprPtr> Values;
  /// What the filter last wrote to each frame word it can still read back.
  std::map<int64_t, ExprPtr> Words;
  /// Where a jump lands in the lists of the body: an `if` jumping past what it
  /// guards holds no statement the jump's target is in.
  struct Label {
    const std::vector<HighStmt> *List;
    size_t Index;
    const Rest *Next;
  };
  std::map<va_t, Label> Labels;
  size_t Budget = limits::ir::kSEHFilterValueBudget;
  /// The filter's reads of each variable.
  std::map<VarKey, unsigned> ReadCounts;
  /// The call's value no one has read yet: nothing else that can be seen can
  /// run first, since it would then run after the call.
  std::optional<VarKey> Unread;

  void countReads(const HighExpr &E) {
    if (E.Kind == ExprKind::Var)
      ++ReadCounts[varKey(E.Var)];
    for (const ExprPtr &Operand : E.Operands)
      if (Operand)
        countReads(*Operand);
  }

  /// Whether \p E runs code of its own.  What the runtime left in the frame
  /// only reads as a call.
  static bool runsCode(const ExprPtr &E) {
    if (!E)
      return false;
    if (E->Kind == ExprKind::Call && !callsNamed(E.get(), kExceptionCode) &&
        !callsNamed(E.get(), kExceptionInformation))
      return true;
    return llvm::any_of(E->Operands, runsCode);
  }

  void indexLabels(const std::vector<HighStmt> &List, const Rest *Next) {
    for (size_t I = 0; I < List.size(); ++I)
      if (List[I].Addr && List[I].Addr != InvalidVA)
        Labels.emplace(List[I].Addr, Label{&List, I, Next});
  }

  ExprPtr substitute(const ExprPtr &E) {
    if (!E || !Budget)
      return nullptr;
    --Budget;
    if (E->Kind == ExprKind::Load && !E->Operands.empty())
      if (const auto Word =
              frameDistance(E->Operands[0].get(), Frames, TargetArch);
          Word && Words.count(*Word)) {
        ExprPtr Value = Words.at(*Word);
        // A second read would run the call again.
        if (Value && hasCall(Value))
          Words[*Word] = nullptr;
        return Value;
      }
    switch (E->Kind) {
    case ExprKind::Const:
      return E;
    case ExprKind::Var: {
      if (E->Var.Kind == MedVar::Param || isEntryStackPointer(*E, TargetArch))
        return E;
      const auto Known = Values.find(varKey(E->Var));
      if (Known != Values.end()) {
        // A second read would run the call again.
        if (hasCall(Known->second) && !FirstRead.insert(varKey(E->Var)).second)
          return nullptr;
        if (Unread == varKey(E->Var))
          Unread.reset();
        return Known->second;
      }
      const auto Frame = Frames.find(varKey(E->Var));
      if (Frame == Frames.end())
        return nullptr;
      return frameAddress(*E, Frame->second);
    }
    case ExprKind::Load:
      if (E->MemoryOrdering != NdMemoryOrdering::None ||
          E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return nullptr;
      [[fallthrough]];
    case ExprKind::BinOp:
    case ExprKind::UnaryOp:
    case ExprKind::Cast:
    case ExprKind::BitCast:
    case ExprKind::Call: {
      if (E->Kind == ExprKind::Call &&
          (E->IsIndirectCall || E->IntrinsicId != Intrinsic::None ||
           E->DoesNotReturn))
        return nullptr;
      auto Copy = std::make_shared<HighExpr>(*E);
      for (ExprPtr &Operand : Copy->Operands) {
        ExprPtr Value = substitute(Operand);
        // A callee can name a register the filter never set, which reads as the
        // unknown value it is in any statement.
        if (!Value && E->Kind == ExprKind::Call && Operand &&
            Operand->Kind == ExprKind::Var && Operand->Var.Kind == MedVar::Reg)
          Value = Operand;
        if (!(Operand = std::move(Value)))
          return nullptr;
      }
      return readable(Reads.rewrite(keepViewValid(std::move(Copy))));
    }
    default:
      return nullptr;
    }
  }

  std::set<VarKey> FirstRead;

  /// \p E as the source says it, which the function's own statements get from
  /// passes a filter's value does not go through: `a - b == 0` is how the
  /// machine compares, and a wrapping sum is unsigned.
  static ExprPtr readable(ExprPtr E) {
    if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
      return E;
    if (E->Op == NdOp::INT_ADD || E->Op == NdOp::INT_SUB) {
      if (!E->Type || E->Type->Kind != NdTypeKind::Int || !E->Type->IsSigned)
        return E;
      const TypeRef Unsigned = NdType::makeInt(E->Type->Size, /*S=*/false);
      auto Sum = std::make_shared<HighExpr>(*E);
      Sum->Type = Unsigned;
      for (ExprPtr &Operand : Sum->Operands)
        if (Operand && Operand->Kind == ExprKind::Load) {
          Operand = std::make_shared<HighExpr>(*Operand);
          Operand->Type = Unsigned;
        }
      return Sum;
    }
    if (E->Op != NdOp::INT_EQUAL && E->Op != NdOp::INT_NOTEQUAL)
      return E;
    const HighExpr *Zero = peelViews(E->Operands[1].get());
    const HighExpr *Left = peelViews(E->Operands[0].get());
    if (!Zero || Zero->Kind != ExprKind::Const || Zero->ConstVal || !Left ||
        Left->Kind != ExprKind::BinOp || Left->Operands.size() != 2)
      return E;
    ExprPtr A = Left->Operands[0];
    ExprPtr B = Left->Operands[1];
    if (Left->Op == NdOp::INT_ADD) {
      const HighExpr *Added = peelViews(B.get());
      if (!Added || Added->Kind != ExprKind::Const)
        return E;
      const uint16_t Size = Added->Type ? Added->Type->Size : sizeof(int32_t);
      const uint64_t Mask = Size >= sizeof(uint64_t)
                                ? ~uint64_t{0}
                                : (uint64_t{1} << (Size * 8)) - 1;
      B = HighExpr::makeConst((0 - Added->ConstVal) & Mask, Size,
                              ConstantAddressProvenance::Scalar);
    } else if (Left->Op != NdOp::INT_SUB) {
      return E;
    }
    auto Compare = std::make_shared<HighExpr>(*E);
    Compare->Operands = {std::move(A), std::move(B)};
    return Compare;
  }

  /// What a comparison's value is whatever width carries it.
  static ExprPtr comparisonOf(ExprPtr E) {
    const HighExpr *Inner = peelViews(E.get());
    if (Inner && Inner->Kind == ExprKind::BinOp &&
        (Inner->Op == NdOp::INT_EQUAL || Inner->Op == NdOp::INT_NOTEQUAL ||
         Inner->Op == NdOp::INT_LESS || Inner->Op == NdOp::INT_SLESS ||
         Inner->Op == NdOp::INT_LESSEQUAL || Inner->Op == NdOp::INT_SLESSEQUAL))
      return std::make_shared<HighExpr>(*Inner);
    return E;
  }

  /// What was read before something that can change memory has to run before
  /// it, so reading it later cannot fold.
  void forgetLoads() {
    for (auto It = Values.begin(); It != Values.end();)
      It = hasLoad(It->second) ? Values.erase(It) : std::next(It);
    for (auto It = Words.begin(); It != Words.end();)
      It = hasLoad(It->second) ? Words.erase(It) : std::next(It);
  }

  /// \p First and then \p Rest, as HighEHClause::FilterValue spells a comma.
  static ExprPtr sequenced(ExprPtr First, ExprPtr Rest) {
    auto Sequence = std::make_shared<HighExpr>();
    Sequence->Kind = ExprKind::BinOp;
    Sequence->Op = NdOp::NOP;
    Sequence->Type = Rest->Type;
    Sequence->Operands = {std::move(First), std::move(Rest)};
    return Sequence;
  }

  static bool hasCall(const ExprPtr &E) {
    if (!E)
      return false;
    if (E->Kind == ExprKind::Call && E->CallAddr)
      return true;
    return llvm::any_of(E->Operands, hasCall);
  }

  static bool hasLoad(const ExprPtr &E) {
    if (!E)
      return false;
    if (E->Kind == ExprKind::Load)
      return true;
    return llvm::any_of(E->Operands, hasLoad);
  }

  ExprPtr frameAddress(const HighExpr &E, int64_t Offset) const {
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    MedVar EntrySP;
    EntrySP.Kind = MedVar::Reg;
    EntrySP.TheArch = TargetArch;
    EntrySP.RegOff = TRI.StackPointer;
    EntrySP.Size = TRI.PointerSize;
    ExprPtr Result = HighExpr::makeVar(EntrySP, E.Type);
    if (Offset) {
      const bool Down = Offset < 0;
      Result = HighExpr::makeBinop(
          Down ? NdOp::INT_SUB : NdOp::INT_ADD, Result,
          HighExpr::makeConst(static_cast<uint64_t>(Down ? -Offset : Offset),
                              TRI.PointerSize,
                              ConstantAddressProvenance::Scalar));
      Result->Type = E.Type;
    }
    return Result;
  }

  /// Whether \p Address is on the stack the runtime calls a filter on, which
  /// the function's frame says nothing about: a call's stack argument.
  bool isOutgoingStackAddress(const HighExpr *Address) const {
    Address = peelViews(Address);
    while (Address && Address->Kind == ExprKind::BinOp &&
           Address->Operands.size() == 2 && Address->Op == NdOp::INT_SUB)
      Address = peelViews(Address->Operands[0].get());
    if (!Address)
      return false;
    return Address->Kind == ExprKind::Undef ||
           (Address->Kind == ExprKind::Var &&
            Address->Var.Kind == MedVar::Reg &&
            !isEntryStackPointer(*Address, TargetArch) &&
            Address->Var.RegOff == getTargetRegInfo(TargetArch).StackPointer);
  }

  ExprPtr foldList(const std::vector<HighStmt> &List, size_t From,
                   const Rest *Next) {
    for (size_t I = From; I < List.size(); ++I) {
      const HighStmt &S = List[I];
      switch (S.Kind) {
      case StmtKind::Nop:
        continue;
      case StmtKind::Block: {
        if (S.Body.empty())
          continue;
        const Rest After{&List, I + 1, Next};
        return foldList(S.Body, 0, &After);
      }
      case StmtKind::Assign: {
        if (!S.Dst || S.Dst->Kind != ExprKind::Var || !S.Val)
          return nullptr;
        // What a runtime entry starts from, such as a register it restores, is
        // not part of the value unless something reads it.  A call has to run.
        ExprPtr Value = substitute(S.Val);
        if (!Value) {
          if (runsCode(S.Val))
            return nullptr;
          Values.erase(varKey(S.Dst->Var));
          continue;
        }
        const bool Calls = hasCall(Value);
        // The call before this has to run first, and a load read after it.
        if (Unread && (Calls || hasLoad(Value)))
          return nullptr;
        // What a load read before a call has to run before it.
        if (Calls)
          forgetLoads();
        // A call whose value nothing reads runs for what it does.
        if (Calls && !ReadCounts.count(varKey(S.Dst->Var))) {
          ExprPtr Rest = foldList(List, I + 1, Next);
          return Rest ? sequenced(std::move(Value), std::move(Rest)) : nullptr;
        }
        if (Calls)
          Unread = varKey(S.Dst->Var);
        Reads.noteAssign(*S.Dst, *Value);
        Values[varKey(S.Dst->Var)] = std::move(Value);
        continue;
      }
      case StmtKind::Store: {
        // A call's stack argument goes with the call.
        if (isOutgoingStackAddress(S.StoreAddr.get()))
          continue;
        ExprPtr Address = substitute(S.StoreAddr);
        if (!Address)
          return nullptr;
        const auto Word = frameDistance(Address.get(), Frames, TargetArch);
        // Below the frame is the stack the runtime calls the filter on, where
        // it keeps a register or makes room with a `push`: what it holds is no
        // business of the value.
        if (Word && StackFloor && *Word < *StackFloor)
          continue;
        ExprPtr Value = substitute(S.StoreVal);
        if (!Value)
          return nullptr;
        forgetLoads();
        if (Word)
          Words[*Word] = Value;
        if (Word && IsPrivate && IsPrivate(*Word, *Value))
          continue;
        if (Unread)
          return nullptr;
        auto Write = std::make_shared<HighExpr>();
        Write->Kind = ExprKind::Store;
        Write->Type = Value->Type;
        Write->Operands = {std::move(Address), std::move(Value)};
        ExprPtr Rest = foldList(List, I + 1, Next);
        return Rest ? sequenced(std::move(Write), std::move(Rest)) : nullptr;
      }
      case StmtKind::Call:
      case StmtKind::ExprStmt: {
        // A call run for what it does, whose value nothing reads.
        ExprPtr Effect =
            substitute(S.Kind == StmtKind::Call ? S.CallExpr : S.Val);
        if (!Effect || Unread)
          return nullptr;
        forgetLoads();
        ExprPtr Rest = foldList(List, I + 1, Next);
        return Rest ? sequenced(std::move(Effect), std::move(Rest)) : nullptr;
      }
      case StmtKind::Return: {
        ExprPtr Value = S.RetVal ? substitute(S.RetVal) : nullptr;
        return Value && !Unread ? comparisonOf(std::move(Value)) : nullptr;
      }
      case StmtKind::Goto: {
        const auto Target = Labels.find(S.GotoTarget);
        if (Target == Labels.end() || !Budget)
          return nullptr;
        --Budget;
        return foldList(*Target->second.List, Target->second.Index,
                        Target->second.Next);
      }
      case StmtKind::If:
      case StmtKind::IfElse: {
        ExprPtr Cond = substitute(S.Cond);
        if (!Cond || hasCall(Cond) || Unread)
          return nullptr;
        const auto Before = Values;
        const auto WordsBefore = Words;
        const Rest After{&List, I + 1, Next};
        ExprPtr Then = foldList(S.Body, 0, &After);
        Values = Before;
        Words = WordsBefore;
        ExprPtr Else = foldList(S.ElseBody, 0, &After);
        Values = Before;
        Words = WordsBefore;
        if (!Then || !Else)
          return nullptr;
        // The source compares for what it wants; the branch jumps past it.
        if (Cond->Kind == ExprKind::BinOp && Cond->Op == NdOp::INT_NOTEQUAL) {
          Cond = std::make_shared<HighExpr>(*Cond);
          Cond->Op = NdOp::INT_EQUAL;
          std::swap(Then, Else);
        }
        auto Select = std::make_shared<HighExpr>();
        Select->Kind = ExprKind::BinOp;
        Select->Op = NdOp::SELECT;
        Select->Type = Then->Type;
        Select->Operands = {std::move(Cond), std::move(Then), std::move(Else)};
        return Select;
      }
      default:
        return nullptr;
      }
    }
    return Next ? foldList(*Next->List, Next->From, Next->Next) : nullptr;
  }
};

} // namespace

ExprPtr foldFilterBody(const std::vector<HighStmt> &Body,
                       const FilterFrame &Frame) {
  return FilterFolder(Frame).fold(Body);
}

} // namespace neverd::seh_filter
