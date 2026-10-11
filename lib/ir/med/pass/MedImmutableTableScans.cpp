//===- MedImmutableTableScans.cpp - Immutable table scans -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedConstantPropagation.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {
namespace {
constexpr size_t MaxEvaluationWork = 65536;
constexpr unsigned MaxEvaluationDepth = 64;
constexpr unsigned MaxScanIterations = 4096;
constexpr unsigned MaxFoldRounds = 4;

using Key = std::tuple<MedVar::VarKind, int, int, uint16_t>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer, V.Size}; }
using Values = std::map<Key, MedVar>;
uint64_t mask(unsigned Size) {
  return Size >= 8 ? ~uint64_t{0} : (uint64_t{1} << (Size * 8)) - 1;
}
bool pointer(const MedVar &V) {
  return V.isConst() && isExactAddressProvenance(V.Provenance);
}
bool independentEntry(const MedFunc &Func, const MedBlock &Block,
                      const BinaryImage &Image) {
  auto Within = [&](va_t VA) {
    return VA == Block.StartAddr ||
           (VA > Block.StartAddr && VA < Block.EndAddr);
  };
  auto ContainsEntry = [&](const auto &Entries) {
    const auto It = Entries.lower_bound(Block.StartAddr);
    return It != Entries.end() && Within(*It);
  };
  return Within(Func.Entry) || ContainsEntry(Func.ModuleAnalysisRoots) ||
         ContainsEntry(Image.CodeRefTargets);
}

class Evaluator {
  const MedFunc &Func;
  const BinaryImage &Image;
  std::map<Key, const MedOp *> Defs;
  std::map<Key, std::pair<const MedBlock *, const PhiNode *>> Phis;
  std::map<int, const MedBlock *> Blocks;
  std::set<Key> CompletePhis;
  std::set<Key> Active;
  Values Memo;
  size_t &Budget;
  bool Complete = false;

  bool spend(size_t Work = 1) {
    if (Work > Budget) {
      Budget = 0;
      return false;
    }
    Budget -= Work;
    return true;
  }

  bool scalar(const MedVar &V) const {
    if (!V.isConst() || isAddressProvenance(V.Provenance))
      return false;
    // Untagged constants keep the image's conservative relocation model.
    // A coincidentally mapped integer is not evidence for either an address
    // or a scalar, and folding it would erase that uncertainty for emitters.
    return V.Provenance == ConstantAddressProvenance::Scalar ||
           !Image.isPotentiallyRelocatableAddress(V.ConstVal);
  }

  MedVar literal(uint64_t Bits, unsigned Size) const {
    return MedVar::makeConst(Bits & mask(Size), Size,
                             ConstantAddressProvenance::Scalar);
  }
  MedVar address(va_t VA, bool Code) const {
    return MedVar::makeConst(VA, Image.getPointerSize(),
                             Code ? ConstantAddressProvenance::CodeAddress
                                  : ConstantAddressProvenance::DataAddress,
                             Image.getSectionFor(VA)->VA);
  }

public:
  Evaluator(const MedFunc &F, const BinaryImage &I, size_t &Remaining)
      : Func(F), Image(I), Budget(Remaining) {
    std::map<int, std::set<int>> Incoming;
    std::set<Key> Outputs;
    for (const auto &B : F.Blocks) {
      if (!spend(1 + B.Phis.size() + B.Ops.size()) || B.Id < 0 ||
          !Blocks.emplace(B.Id, &B).second)
        return;
      for (int S : B.Succs) {
        if (!spend() || !Incoming[S].insert(B.Id).second)
          return;
      }
      for (const auto &P : B.Phis) {
        if (!spend(P.Args.size()) || !Outputs.insert(key(P.Output)).second)
          return;
        Phis.emplace(key(P.Output), std::pair{&B, &P});
      }
      for (const auto &Op : B.Ops) {
        // Source-call certificates authenticate this value graph and frame,
        // including original pointer LOAD occurrences. Retain the graph until
        // this transformation can also rebind its occurrence-sensitive proofs.
        if (Op.NumInputs > Op.Inputs.size() || Op.SourceCallHint)
          return;
        if (Op.Output.Size && !Op.Output.isConst()) {
          if (!Outputs.insert(key(Op.Output)).second)
            return;
          Defs.emplace(key(Op.Output), &Op);
        }
      }
    }
    for (const auto &[Id, Preds] : Incoming)
      if (!Blocks.count(Id))
        return;
    for (const auto &B : F.Blocks)
      for (const auto &P : B.Phis) {
        if (!spend(B.Preds.size() + P.Args.size()))
          return;
        if (hasCompleteOrdinaryPhiInputs(B, P, Incoming[B.Id]))
          CompletePhis.insert(key(P.Output));
      }
    Complete = true;
  }

  bool exhausted() const { return !Budget; }
  bool complete() const { return Complete; }
  bool hasCompletePhi(const PhiNode &P) const {
    return CompletePhis.count(key(P.Output));
  }
  void clearMemo() { Memo.clear(); }

  std::optional<MedVar> value(const MedVar &V, const Values *Bindings = nullptr,
                              unsigned Depth = 0) {
    if (!Complete || !Budget || Depth > MaxEvaluationDepth || !V.Size ||
        V.Size > 8)
      return std::nullopt;
    --Budget;
    if (V.isConst()) {
      if (V.Provenance == ConstantAddressProvenance::AddressFragment)
        return std::nullopt;
      if (pointer(V) &&
          (V.Size < Image.getPointerSize() ||
           V.ConstVal != (V.ConstVal & mask(Image.getPointerSize()))))
        return std::nullopt;
      return V;
    }
    if (Bindings)
      if (auto It = Bindings->find(key(V)); It != Bindings->end())
        return It->second;
    if (!Bindings)
      if (auto It = Memo.find(key(V)); It != Memo.end())
        return It->second;
    if (!Active.insert(key(V)).second)
      return std::nullopt;
    std::optional<MedVar> Result;
    if (auto It = Phis.find(key(V)); It != Phis.end()) {
      const auto &[Block, Phi] = It->second;
      // An independently enterable root has an implicit incoming value that
      // no ordinary PHI arm describes.
      if (CompletePhis.count(key(V)) &&
          !independentEntry(Func, *Block, Image)) {
        bool AllValues = true;
        for (const auto &[Pred, Arg] : Phi->Args) {
          const auto Found = Blocks.find(Pred);
          if (Found == Blocks.end() ||
              std::find(Found->second->Succs.begin(),
                        Found->second->Succs.end(),
                        Block->Id) == Found->second->Succs.end()) {
            AllValues = false;
            break;
          }
          if (auto Chosen = successor(*Found->second, Bindings, Depth + 1);
              Chosen && *Chosen != Block->Id)
            continue;
          auto A = value(Arg, Bindings, Depth + 1);
          if (!A || (Result && *A != *Result)) {
            AllValues = false;
            break;
          }
          Result = A;
        }
        if (!AllValues)
          Result.reset();
      }
    } else if (auto It = Defs.find(key(V)); It != Defs.end()) {
      const auto &Op = *It->second;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          !Op.Inputs[0].isConst() && key(Op.Inputs[0]) == key(V))
        Result = V;
      else
        Result = operation(Op, Bindings, Depth + 1);
    } else {
      // Preserve the identity of an unchanged external input, never invent a
      // concrete value for it. Only copies and invariant PHIs can carry it.
      Result = V;
    }
    Active.erase(key(V));
    if (Result && !Bindings)
      Memo.emplace(key(V), *Result);
    return Result;
  }

  std::optional<int> successor(const MedBlock &B,
                               const Values *Bindings = nullptr,
                               unsigned Depth = 0) {
    if (B.Succs.size() == 1)
      return B.Succs.front();
    if (B.Succs.size() != 2 || B.Ops.empty())
      return std::nullopt;
    const auto &Branch = B.Ops.back();
    if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs != 2 ||
        !Branch.Inputs[0].isConst())
      return std::nullopt;
    auto Cond = value(Branch.Inputs[1], Bindings, Depth + 1);
    if (!Cond || !scalar(*Cond))
      return std::nullopt;
    for (int S : B.Succs) {
      const auto It = Blocks.find(S);
      if (It != Blocks.end() &&
          It->second->StartAddr == Branch.Inputs[0].ConstVal)
        return Cond->ConstVal ? S : (B.Succs[0] == S ? B.Succs[1] : B.Succs[0]);
    }
    return std::nullopt;
  }

  std::optional<MedVar> operation(const MedOp &Op,
                                  const Values *Bindings = nullptr,
                                  unsigned Depth = 0) {
    if (!Complete || !Budget || Depth > MaxEvaluationDepth || !Op.Output.Size ||
        Op.Output.Size > 8 || Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Op.SourceCallHint || !Op.IntrinsicOutputs.empty())
      return std::nullopt;
    auto A =
        Op.NumInputs ? value(Op.Inputs[0], Bindings, Depth + 1) : std::nullopt;
    if (!A)
      return std::nullopt;
    const unsigned Size = Op.Output.Size;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 && A->Size == Size)
      return A;
    // LowIR uses a widened integer carrier for a 32-bit memory address.
    // Retain the exact pointer occurrence through lossless views instead of
    // erasing its owner or refusing the immutable load that consumes it.
    if (pointer(*A) && A->Size >= Image.getPointerSize() &&
        Size >= Image.getPointerSize() &&
        ((Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs == 1 &&
          Size >= A->Size) ||
         (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
          Op.Inputs[1].isConst() && scalar(Op.Inputs[1]) &&
          Op.Inputs[1].ConstVal == 0 && Size <= A->Size))) {
      MedVar Result = *A;
      Result.Size = Size;
      return Result;
    }
    if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 && A->isConst()) {
      const auto Slot = A->ConstVal;
      if (pointer(*A) && A->AddressOwnerVA != InvalidVA) {
        const auto *Owner = Image.getSectionFor(A->AddressOwnerVA);
        if (!Owner || Image.getSectionFor(Slot) != Owner)
          return std::nullopt;
      }
      if (Size == Image.getPointerSize()) {
        if (auto Target = readImmutableImagePointer(Image, Slot))
          return address(*Target, false);
        if (auto Target = readImmutableImageCodePointer(Image, Slot))
          return address(*Target, true);
      }
      auto Bytes = readImmutableImageBytes(Image, Slot, Size);
      if (!Bytes)
        return std::nullopt;
      uint64_t Bits = 0;
      for (unsigned I = 0; I < Size; ++I)
        Bits |= uint64_t{(*Bytes)[I]} << (I * 8);
      // Exact bytes alone cannot distinguish an address from an integer with
      // the same bits. Only the pointer readers above may publish an address;
      // retaining this LOAD preserves the backend's relocation proof.
      if (Size == Image.getPointerSize() &&
          Image.isPotentiallyRelocatableAddress(Bits))
        return std::nullopt;
      return literal(Bits, Size);
    }
    auto B = Op.NumInputs >= 2 ? value(Op.Inputs[1], Bindings, Depth + 1)
                               : std::nullopt;
    if ((Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_NOTEQUAL) &&
        B && (pointer(*A) || pointer(*B))) {
      std::optional<bool> Equal;
      if (pointer(*A) && pointer(*B) && A->Provenance == B->Provenance &&
          A->AddressOwnerVA == B->AddressOwnerVA && A->ConstVal == B->ConstVal)
        Equal = true;
      else if ((pointer(*A) && scalar(*B) && !B->ConstVal) ||
               (pointer(*B) && scalar(*A) && !A->ConstVal))
        Equal = false;
      if (Equal)
        return literal(Op.Opcode == NdOp::INT_EQUAL ? *Equal : !*Equal, Size);
    }
    if (Op.Opcode == NdOp::INT_ADD && B && pointer(*B) && scalar(*A))
      std::swap(A, B);
    if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) && B &&
        pointer(*A) &&
        A->Provenance == ConstantAddressProvenance::DataAddress && scalar(*B) &&
        Size == A->Size) {
      const uint64_t Offset = B->ConstVal & mask(B->Size);
      const va_t VA = (Op.Opcode == NdOp::INT_ADD ? A->ConstVal + Offset
                                                  : A->ConstVal - Offset) &
                      mask(Size);
      const auto *Owner = Image.getSectionFor(
          A->AddressOwnerVA == InvalidVA ? A->ConstVal : A->AddressOwnerVA);
      if (Owner && A->ConstVal >= Owner->VA &&
          A->ConstVal - Owner->VA <= Owner->Size && VA >= Owner->VA &&
          VA - Owner->VA <= Owner->Size)
        return MedVar::makeConst(VA, Size, A->Provenance, Owner->VA);
      return std::nullopt;
    }
    if (!scalar(*A) || (B && !scalar(*B)))
      return std::nullopt;
    const uint64_t X = A->ConstVal & mask(A->Size);
    const uint64_t Y = B ? B->ConstVal & mask(B->Size) : 0;
    auto Signed = [](uint64_t V, unsigned Size) {
      if (Size < 8 && (V & (uint64_t{1} << (Size * 8 - 1))))
        V |= ~mask(Size);
      return static_cast<int64_t>(V);
    };
    if (Op.NumInputs == 1) {
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
        return literal(X, Size);
      case NdOp::INT_SEXT:
        return literal(Signed(X, A->Size), Size);
      case NdOp::BOOL_NOT:
        return literal(!X, Size);
      default:
        return std::nullopt;
      }
    }
    if (!B || Op.NumInputs != 2)
      return std::nullopt;
    // MedLLVM executes binary integer operations at the common operand width
    // and only then converts to the destination. In particular an i8 sum
    // assigned to i64 wraps before zero extension; signed comparisons first
    // zero-extend a narrower bitvector to that common width.
    const unsigned OperandSize = std::max(A->Size, B->Size);
    auto Binary = [&](uint64_t Bits) {
      return literal(Bits & mask(OperandSize), Size);
    };
    switch (Op.Opcode) {
    case NdOp::INT_ADD:
      return Binary(X + Y);
    case NdOp::INT_SUB:
      return Binary(X - Y);
    case NdOp::INT_MULT:
      return Binary(X * Y);
    case NdOp::INT_AND:
      return Binary(X & Y);
    case NdOp::INT_OR:
      return Binary(X | Y);
    case NdOp::INT_XOR:
      return Binary(X ^ Y);
    case NdOp::SUBBYTES:
      return literal(Y < 8 ? X >> (Y * 8) : 0, Size);
    case NdOp::INT_LEFT:
      return Binary(Y < OperandSize * 8 ? X << Y : 0);
    case NdOp::INT_RIGHT:
      return Binary(Y < OperandSize * 8 ? X >> Y : 0);
    case NdOp::INT_EQUAL:
      return literal(X == Y, Size);
    case NdOp::INT_NOTEQUAL:
      return literal(X != Y, Size);
    case NdOp::INT_LESS:
      return literal(X < Y, Size);
    case NdOp::INT_LESSEQUAL:
      return literal(X <= Y, Size);
    case NdOp::INT_SLESS:
      return literal(Signed(X, OperandSize) < Signed(Y, OperandSize), Size);
    case NdOp::INT_SLESSEQUAL:
      return literal(Signed(X, OperandSize) <= Signed(Y, OperandSize), Size);
    default:
      return std::nullopt;
    }
  }
};

bool foldScan(MedFunc &Func, MedBlock &Block, const BinaryImage &Image,
              size_t &Budget) {
  if (Block.Id == 0 || Block.Preds.size() != 2 || Block.Succs.size() != 2 ||
      Block.Phis.empty() || Block.Ops.empty() ||
      !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
      independentEntry(Func, Block, Image) ||
      std::count(Block.Preds.begin(), Block.Preds.end(), Block.Id) != 1 ||
      std::count(Block.Succs.begin(), Block.Succs.end(), Block.Id) != 1 ||
      Block.Ops.back().Opcode != NdOp::COND_BR)
    return false;
  const int Entry =
      Block.Preds[0] == Block.Id ? Block.Preds[1] : Block.Preds[0];
  const int Exit = Block.Succs[0] == Block.Id ? Block.Succs[1] : Block.Succs[0];
  if (Block.Phis.size() > Budget ||
      Block.Ops.size() > Budget - Block.Phis.size())
    return false;
  Budget -= Block.Phis.size() + Block.Ops.size();
  std::set<Key> Local;
  for (const auto &P : Block.Phis)
    Local.insert(key(P.Output));
  for (size_t I = 0; I + 1 < Block.Ops.size(); ++I) {
    const auto &Op = Block.Ops[I];
    if (!Op.Output.Size || Op.Opcode == NdOp::STORE ||
        Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
        Op.Opcode == NdOp::INTRINSIC || Op.DoesNotReturn)
      return false;
    Local.insert(key(Op.Output));
  }
  Evaluator Eval(Func, Image, Budget);
  if (!Eval.complete())
    return false;
  Values State;
  for (const auto &P : Block.Phis) {
    if (!Eval.hasCompletePhi(P))
      return false;
    bool Found = false;
    for (const auto &[Pred, Arg] : P.Args)
      if (Pred == Entry) {
        auto V = Eval.value(Arg);
        if (V && !V->isConst() && Local.count(key(*V))) {
          return false;
        }
        // A lowered register PHI can carry a dead incoming ABI value that the
        // first iteration overwrites. Keep it explicitly unknown while
        // simulating; it may neither decide control nor survive the exit.
        State.emplace(key(P.Output), V.value_or(MedVar::makeUnspecified(
                                         P.Output.Size, Image.Arch)));
        Found = true;
      }
    if (!Found)
      return false;
  }
  for (unsigned Iteration = 0; Iteration < MaxScanIterations; ++Iteration) {
    Values Current = State;
    for (size_t I = 0; I + 1 < Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      auto V = Eval.operation(Op, &Current);
      if (!V || (!V->isConst() && Local.count(key(*V)))) {
        return false;
      }
      Current.insert_or_assign(key(Op.Output), *V);
    }
    const auto Next = Eval.successor(Block, &Current);
    if (!Next || Eval.exhausted())
      return false;
    if (*Next == Exit) {
      for (const auto &[K, V] : Current)
        if (V.Kind == MedVar::Unspecified)
          return false;
      // Commit only after a terminating iteration has been proved. Retain the
      // original output identities so every external use and sidecar remains
      // valid; the scan's complete effects are just these final values.
      std::vector<MedOp> Copies;
      auto Copy = [&](const MedVar &Output, va_t Addr, int Seq) {
        MedOp Op;
        Op.Opcode = NdOp::COPY;
        Op.Output = Output;
        Op.Addr = Addr;
        Op.OriginSeq = Seq;
        Op.addInput(Current.at(key(Output)));
        Copies.push_back(std::move(Op));
      };
      for (const auto &P : Block.Phis)
        Copy(P.Output, Block.StartAddr, -1);
      for (size_t I = 0; I + 1 < Block.Ops.size(); ++I)
        Copy(Block.Ops[I].Output, Block.Ops[I].Addr, Block.Ops[I].OriginSeq);
      auto Branch = Block.Ops.back();
      Branch.Opcode = NdOp::BRANCH;
      Branch.NumInputs = 1;
      for (const auto &B : Func.Blocks)
        if (B.Id == Exit)
          Branch.Inputs[0] =
              MedVar::makeConst(B.StartAddr, Image.getPointerSize());
      Copies.push_back(std::move(Branch));
      Block.Ops = std::move(Copies);
      Block.Phis.clear();
      Block.Preds.erase(
          std::remove(Block.Preds.begin(), Block.Preds.end(), Block.Id),
          Block.Preds.end());
      Block.Succs = {Exit};
      return true;
    }
    Values NextState;
    for (const auto &P : Block.Phis) {
      bool Found = false;
      for (const auto &[Pred, Arg] : P.Args)
        if (Pred == Block.Id) {
          auto V = Eval.value(Arg, &Current);
          if (!V)
            return false;
          NextState.emplace(key(P.Output), *V);
          Found = true;
        }
      if (!Found)
        return false;
    }
    State = std::move(NextState);
  }
  return false;
}
} // namespace

bool foldImmutableTableScans(MedFunc &Func, const BinaryImage &Image) {
  // The loader owns format and target admission; every LOAD separately proves
  // exact immutable bytes or a resolved pointer, never just a numeric address.
  if (!supportsImmutableImageReads(Image) || Func.SkippedSSA)
    return false;
  bool Changed = false;
  size_t Budget = MaxEvaluationWork;
  for (unsigned Round = 0; Round != MaxFoldRounds; ++Round) {
    bool Progress = false;
    Evaluator Eval(Func, Image, Budget);
    if (!Eval.complete())
      break;
    // A folded scan often feeds a later loop through an invariant PHI.
    // Publish those exact incoming values as well as operation results, so
    // every consumer sees the scan's count without re-evaluating image bytes.
    for (auto &Block : Func.Blocks)
      for (auto &Phi : Block.Phis)
        for (auto &[Pred, Arg] : Phi.Args) {
          if (Arg.isConst())
            continue;
          auto Value = Eval.value(Arg);
          if (Value && Value->isConst() && Value->Size == Arg.Size &&
              !Eval.exhausted()) {
            Arg = *Value;
            Progress = true;
          }
        }
    for (auto &Block : Func.Blocks)
      for (auto &Op : Block.Ops) {
        if (Op.Dead || (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                        Op.Inputs[0].isConst()))
          continue;
        auto Value = Eval.operation(Op);
        if (!Value || !Value->isConst() || Eval.exhausted())
          continue;
        Op.Opcode = NdOp::COPY;
        Op.NumInputs = 1;
        Op.Inputs[0] = *Value;
        Progress = true;
      }
    for (auto &Block : Func.Blocks) {
      if (!Budget)
        break;
      Progress |= foldScan(Func, Block, Image, Budget);
    }
    Changed |= Progress;
    if (!Progress)
      break;
  }
  return Changed;
}
} // namespace neverd
