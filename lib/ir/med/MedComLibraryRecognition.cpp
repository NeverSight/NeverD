//===- MedComLibraryRecognition.cpp - Finite COM lifetime policies -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLibraryRecognitionInternal.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolSpelling.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>
#include <tuple>

using namespace neverd;
using namespace neverd::sigs;

namespace {
using Key = std::tuple<int, int, int>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }
bool zero(const MedVar &V) { return V.isConst() && V.Size == 8 && !V.ConstVal; }
bool same(const MedVar &A, const MedVar &B) {
  // A null test's zero is one number, whatever provenance folding gave it.
  return (A == B && A.Size == B.Size) || (zero(A) && zero(B));
}
bool plain(const MedOp &O) {
  return O.MemoryOrdering == NdMemoryOrdering::None &&
         O.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}
bool scalar(NdOp O) {
  switch (O) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::SUBBYTES:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SBOR:
  case NdOp::INT_SOVF:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
    return true;
  default:
    return false;
  }
}

struct Guard {
  MedVar A, B;
  bool Equal;
  const MedOp *Operation;
};
struct Effect {
  enum Kind { Store, AddRef, Release } Type;
  MedVar Value;
  const MedOp *Operation;
  size_t Position;
};
struct Path {
  std::map<Key, MedVar> Phis;
  std::vector<Guard> Guards;
  std::vector<Effect> Effects;
  std::vector<const MedOp *> Operations;
  std::set<int> Blocks;
  std::optional<MedVar> This;
  std::optional<MedVar> Return;
};

class ComMatcher {
public:
  ComMatcher(const MedFunc &F, uint64_t Receiver, uint64_t Source,
             uint64_t Stack, size_t &Work, bool &Exhausted,
             const BinaryImage &Image)
      : F(F), Receiver(Receiver), Source(Source), Stack(Stack), Work(Work),
        Exhausted(Exhausted), Image(Image) {
    for (const auto &B : F.Blocks) {
      for (const auto &P : B.Phis)
        Definitions[key(P.Output)] = nullptr;
      for (const auto &O : B.Ops) {
        if (!spend() || O.NumInputs > O.Inputs.size() ||
            !O.IntrinsicOutputs.empty()) {
          Valid = false;
          return;
        }
        if (O.Dead || !O.Output.Size ||
            (O.Opcode == NdOp::COPY && O.NumInputs == 1 &&
             same(O.Output, O.Inputs[0])))
          continue;
        auto [At, Added] = Definitions.emplace(key(O.Output), &O);
        if (!Added) {
          Valid = false;
          return;
        }
      }
    }
    for (const auto &C : F.CallClobbers)
      Definitions.try_emplace(key(C.Value), nullptr);
  }
  bool spend() {
    if (!Work) {
      Exhausted = true;
      return false;
    }
    --Work;
    return true;
  }
  const MedOp *definition(const MedVar &V) const {
    auto At = Definitions.find(key(V));
    return At == Definitions.end() ? nullptr : At->second;
  }
  std::optional<MedVar> value(MedVar V, const Path &P, unsigned Depth = 0) {
    if (!spend() ||
        Depth >= ::neverd::limits::ir::kMedComLibraryRecognitionValueDepthLimit)
      return std::nullopt;
    if (V.isConst())
      return V;
    if (auto At = P.Phis.find(key(V)); At != P.Phis.end())
      return value(At->second, P, Depth + 1);
    const auto *D = definition(V);
    if (D && D->Opcode == NdOp::COPY) {
      if (D->NumInputs != 1 || D->Inputs[0].Size != V.Size ||
          D->Output.Size != V.Size)
        return std::nullopt;
      return value(D->Inputs[0], P, Depth + 1);
    }
    return V;
  }
  bool input(const MedVar &V, uint64_t Reg) const {
    return V.Size == 8 && V.Kind == MedVar::Reg && V.RegOff == Reg &&
           V.SSAVer == 0 && !Definitions.contains(key(V));
  }
  bool stackAddress(MedVar V, const Path &P, unsigned Depth = 0) {
    auto C = value(V, P);
    if (!C || Depth >= ::neverd::limits::ir::
                           kMedComLibraryRecognitionStackAddressDepthLimit)
      return false;
    if (input(*C, Stack))
      return true;
    const auto *D = definition(*C);
    return D && (D->Opcode == NdOp::INT_ADD || D->Opcode == NdOp::INT_SUB) &&
           D->NumInputs == 2 && C->Size == 8 && D->Inputs[1].isConst() &&
           D->Inputs[1].Size == 8 && stackAddress(D->Inputs[0], P, Depth + 1);
  }
  bool loadInput(MedVar V, uint64_t Register, const Path &P) {
    auto C = value(V, P);
    const auto *D = C ? definition(*C) : nullptr;
    if (!D || D->Opcode != NdOp::LOAD || D->NumInputs != 1 ||
        D->Output.Size != 8 || !plain(*D))
      return false;
    auto A = value(D->Inputs[0], P);
    return A && input(*A, Register);
  }
  std::optional<Guard> condition(const MedOp &Branch, const Path &P,
                                 bool EqualEdge) {
    MedVar V = Branch.Inputs[1];
    auto C = value(V, P);
    const auto *D = C ? definition(*C) : nullptr;
    if (!D ||
        (D->Opcode != NdOp::INT_EQUAL && D->Opcode != NdOp::INT_NOTEQUAL) ||
        D->NumInputs != 2 || D->Output.Size != 1)
      return std::nullopt;
    auto A = value(D->Inputs[0], P), B = value(D->Inputs[1], P);
    if (!A || !B || A->Size != 8 || B->Size != 8)
      return std::nullopt;
    if (zero(*B)) {
      const auto *Left = definition(*A);
      if (Left && Left->Opcode == NdOp::INT_AND && Left->NumInputs == 2) {
        auto X = value(Left->Inputs[0], P), Y = value(Left->Inputs[1], P);
        if (X && Y && same(*X, *Y))
          A = X;
      } else if (Left && Left->Opcode == NdOp::INT_SUB &&
                 Left->NumInputs == 2) {
        A = value(Left->Inputs[0], P);
        B = value(Left->Inputs[1], P);
      }
    }
    if (!A || !B || A->Size != 8 || B->Size != 8)
      return std::nullopt;
    return Guard{*A, *B, D->Opcode == NdOp::INT_EQUAL ? EqualEdge : !EqualEdge,
                 &Branch};
  }
  bool guarded(const Path &P, MedVar A, MedVar B, bool Equal) {
    auto X = value(A, P), Y = value(B, P);
    if (!X || !Y)
      return false;
    for (const auto &G : P.Guards)
      if (G.Equal == Equal && ((same(G.A, *X) && same(G.B, *Y)) ||
                               (same(G.A, *Y) && same(G.B, *X)))) {
        UsedGuards.insert(G.Operation);
        return true;
      }
    return false;
  }
  std::optional<Effect> virtualCall(const MedOp &O, const Path &P) {
    if (O.NumInputs != 1 || !P.This || O.DoesNotReturn)
      return std::nullopt;
    auto Target = value(O.Inputs[0], P);
    const auto *Slot = Target ? definition(*Target) : nullptr;
    if (!Slot || Slot->Opcode != NdOp::LOAD || Slot->NumInputs != 1 ||
        Slot->Output.Size != 8)
      return std::nullopt;
    auto Address = value(Slot->Inputs[0], P);
    const auto *Add = Address ? definition(*Address) : nullptr;
    if (!Add || Add->Opcode != NdOp::INT_ADD || Add->NumInputs != 2 ||
        Add->Output.Size != 8 || !Add->Inputs[1].isConst() ||
        Add->Inputs[1].Size != 8)
      return std::nullopt;
    const uint64_t Offset = Add->Inputs[1].ConstVal;
    if (Offset != 8 && Offset != 16)
      return std::nullopt;
    auto Table = value(Add->Inputs[0], P);
    const auto *Load = Table ? definition(*Table) : nullptr;
    if (!Load || Load->Opcode != NdOp::LOAD || Load->NumInputs != 1 ||
        Load->Output.Size != 8)
      return std::nullopt;
    auto Object = value(Load->Inputs[0], P), Argument = value(*P.This, P);
    if (!Object || !Argument || !same(*Object, *Argument))
      return std::nullopt;
    return Effect{Offset == 8 ? Effect::AddRef : Effect::Release, *Object, &O,
                  P.Operations.size() - 1};
  }
  bool walk(int ID, int Pred, Path P) {
    if (!spend() || Paths.size() >= 16 ||
        P.Blocks.size() >= ::neverd::limits::ir::
                               kMedComLibraryRecognitionWalkPBlocksSizeLimit ||
        !P.Blocks.insert(ID).second)
      return false;
    const auto At =
        llvm::find_if(F.Blocks, [&](const auto &B) { return B.Id == ID; });
    if (At == F.Blocks.end() || !At->ExceptionalPreds.empty())
      return false;
    // An imported terminate-only exit does not rejoin normal execution.
    // Keep it in the original IR/source; never absorb a catch, arbitrary
    // cleanup, partial table or in-function exceptional entry into a region.
    for (const auto &Edge : At->ExceptionalSuccs)
      if (!terminalExceptionExit(Edge))
        return false;
    const auto &B = *At;
    for (const auto &Phi : B.Phis) {
      if (Phi.ExceptionalEntry)
        return false;
      const auto Arg = llvm::find_if(
          Phi.Args, [&](const auto &A) { return A.first == Pred; });
      if (Arg == Phi.Args.end())
        return false;
      auto V = value(Arg->second, P);
      if (!V)
        return false;
      P.Phis[key(Phi.Output)] = *V;
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == Receiver &&
          Phi.Output.Size == 8)
        P.This = *V;
    }
    const MedOp *Branch = nullptr;
    bool Returned = false;
    for (const auto &O : B.Ops) {
      if (!spend() || !plain(O))
        return false;
      if (O.Dead)
        continue;
      if (Branch || Returned || O.NumInputs > O.Inputs.size())
        return false;
      P.Operations.push_back(&O);
      if (O.Output.Kind == MedVar::Reg && O.Output.RegOff == Receiver &&
          O.Output.Size == 8)
        P.This = value(O.Output, P);
      if (O.Opcode == NdOp::STORE) {
        if (O.NumInputs != 2 || O.Inputs[0].Size != 8)
          return false;
        auto Address = value(O.Inputs[0], P), Stored = value(O.Inputs[1], P);
        if (!Address || !Stored)
          return false;
        if (stackAddress(*Address, P))
          continue;
        if (!input(*Address, Receiver) || Stored->Size != 8)
          return false;
        P.Effects.push_back(
            {Effect::Store, *Stored, &O, P.Operations.size() - 1});
      } else if (O.Opcode == NdOp::INDIR_CALL) {
        auto Call = virtualCall(O, P);
        if (!Call)
          return false;
        P.Effects.push_back(*Call);
        P.This.reset();
      } else if (O.Opcode == NdOp::COND_BR) {
        if (O.NumInputs != 2 || !O.Inputs[0].isConst())
          return false;
        Branch = &O;
      } else if (O.Opcode == NdOp::RETURN) {
        Returned = true;
        if (O.NumInputs)
          P.Return = value(O.Inputs[0], P);
      } else if (O.Opcode != NdOp::LOAD && O.Opcode != NdOp::BRANCH &&
                 !scalar(O.Opcode)) {
        return false;
      }
    }
    if (Returned) {
      if (!B.Succs.empty())
        return false;
      Paths.push_back(std::move(P));
      return true;
    }
    if (Branch) {
      if (B.Succs.size() != 2 || B.Succs[0] == B.Succs[1])
        return false;
      bool HaveTrue = false, HaveFalse = false;
      for (int Next : B.Succs) {
        const auto Target = llvm::find_if(
            F.Blocks, [&](const auto &Block) { return Block.Id == Next; });
        if (Target == F.Blocks.end())
          return false;
        const bool True = Target->StartAddr == Branch->Inputs[0].ConstVal;
        (True ? HaveTrue : HaveFalse) = true;
        auto G = condition(*Branch, P, True);
        if (!G)
          return false;
        Path Arm = P;
        Arm.Guards.push_back(*G);
        if (!walk(Next, ID, std::move(Arm)))
          return false;
      }
      return HaveTrue && HaveFalse;
    }
    return B.Succs.size() == 1 && walk(B.Succs.front(), ID, std::move(P));
  }

  bool verify(const LibraryFeatureRule &Rule, bool Whole) {
    if (!Valid || F.Blocks.empty() ||
        F.Blocks.size() > ::neverd::limits::ir::
                              kMedComLibraryRecognitionVerifyFBlocksSizeLimit ||
        !F.Blocks.front().Preds.empty())
      return false;
    if (F.ExceptionMetadata &&
        F.ExceptionMetadata->ParseStatus != ExceptionParseStatus::Complete)
      return false;
    Path Start;
    for (const auto &O : F.Blocks.front().Ops)
      if (O.Opcode == NdOp::COPY && O.NumInputs == 1 &&
          input(O.Inputs[0], Receiver)) {
        Start.This = O.Inputs[0];
        break;
      }
    if (!Start.This || !walk(F.Blocks.front().Id, -1, std::move(Start)))
      return false;
    std::set<int> Seen;
    using Policy = LibraryFeatureRule::ComPolicy;
    const MedVar Zero = MedVar::makeConst(0, 8);
    for (const auto &P : Paths) {
      Seen.insert(P.Blocks.begin(), P.Blocks.end());
      std::vector<Effect> Stores, Adds, Releases;
      for (const auto &E : P.Effects)
        (E.Type == Effect::Store    ? Stores
         : E.Type == Effect::AddRef ? Adds
                                    : Releases)
            .push_back(E);
      if (Rule.Policy == Policy::ZeroInitialize) {
        if (!P.Guards.empty() || Stores.size() != 1 || !zero(Stores[0].Value) ||
            !Adds.empty() || !Releases.empty())
          return false;
        if (Whole && (!P.Return || !input(*P.Return, Receiver)))
          return false;
      } else if (Rule.Policy == Policy::StoreBeforeAddref) {
        if (Stores.size() != 1 || !loadInput(Stores[0].Value, Source, P) ||
            !Releases.empty())
          return false;
        const auto New = Stores[0].Value;
        if (guarded(P, New, Zero, true)) {
          if (!Adds.empty())
            return false;
        } else if (!guarded(P, New, Zero, false) || Adds.size() != 1 ||
                   !same(Adds[0].Value, New) ||
                   Stores[0].Position >= Adds[0].Position)
          return false;
        if (Whole && (!P.Return || !input(*P.Return, Receiver)))
          return false;
      } else if (Rule.Policy == Policy::ClearBeforeRelease ||
                 Rule.Policy == Policy::ReleaseWithoutClear) {
        if (!Adds.empty())
          return false;
        std::optional<MedVar> Old;
        for (const auto &G : P.Guards)
          if (zero(G.B) && loadInput(G.A, Receiver, P))
            Old = G.A;
          else if (zero(G.A) && loadInput(G.B, Receiver, P))
            Old = G.B;
        if (!Old)
          return false;
        if (guarded(P, *Old, Zero, true)) {
          if (!Stores.empty() || !Releases.empty())
            return false;
        } else {
          if (!guarded(P, *Old, Zero, false) || Releases.size() != 1 ||
              !same(Releases[0].Value, *Old))
            return false;
          if (Rule.Policy == Policy::ClearBeforeRelease) {
            if (Stores.size() != 1 || !zero(Stores[0].Value) ||
                Stores[0].Position >= Releases[0].Position)
              return false;
          } else if (!Stores.empty())
            return false;
        }
      } else {
        std::optional<MedVar> OldBefore, New;
        for (const auto &G : P.Guards) {
          if (loadInput(G.A, Receiver, P) && loadInput(G.B, Source, P)) {
            OldBefore = G.A;
            New = G.B;
          }
          if (loadInput(G.B, Receiver, P) && loadInput(G.A, Source, P)) {
            OldBefore = G.B;
            New = G.A;
          }
        }
        if (!OldBefore || !New)
          return false;
        if (guarded(P, *OldBefore, *New, true)) {
          if (!P.Effects.empty())
            return false;
        } else {
          if (!guarded(P, *OldBefore, *New, false) || Stores.size() != 1 ||
              !same(Stores[0].Value, *New))
            return false;
          if (guarded(P, *New, Zero, true)) {
            if (!Adds.empty())
              return false;
          } else if (!guarded(P, *New, Zero, false) || Adds.size() != 1 ||
                     !same(Adds[0].Value, *New) ||
                     Adds[0].Position >= Stores[0].Position)
            return false;
          std::optional<MedVar> OldAfter;
          for (const auto &G : P.Guards)
            if (zero(G.B) && loadInput(G.A, Receiver, P))
              OldAfter = G.A;
            else if (zero(G.A) && loadInput(G.B, Receiver, P))
              OldAfter = G.B;
          if (!OldAfter)
            return false;
          const auto *Load = definition(*OldAfter);
          const auto At = llvm::find(P.Operations, Load);
          if (At == P.Operations.end())
            return false;
          const auto Position = size_t(At - P.Operations.begin());
          if (Position >= Stores[0].Position ||
              (!Adds.empty() && Position <= Adds[0].Position))
            return false;
          if (guarded(P, *OldAfter, Zero, true)) {
            if (!Releases.empty())
              return false;
          } else if (!guarded(P, *OldAfter, Zero, false) ||
                     Releases.size() != 1 ||
                     !same(Releases[0].Value, *OldAfter) ||
                     Releases[0].Position <= Stores[0].Position)
            return false;
        }
      }
      // Extra observable memory or control behavior is not part of this finite
      // lifetime policy. Stack saves are compiler transport, never receiver
      // proof.
      for (const auto *O : P.Operations) {
        if (O->Opcode != NdOp::LOAD)
          continue;
        if (O->NumInputs != 1 || O->Output.Size != 8)
          return false;
        auto A = value(O->Inputs[0], P);
        if (!A)
          return false;
        if (stackAddress(*A, P) || input(*A, Receiver) || input(*A, Source))
          continue;
        bool CallLoad = false;
        for (const auto &E : P.Effects) {
          if (E.Type == Effect::Store)
            continue;
          auto Target = value(E.Operation->Inputs[0], P);
          if (Target && same(O->Output, *Target))
            CallLoad = true;
          if (same(*A, E.Value))
            CallLoad = true;
        }
        if (!CallLoad)
          return false;
      }
    }
    return Seen.size() == F.Blocks.size();
  }
  std::vector<LibraryOccurrence> occurrences() const {
    std::set<LibraryOccurrence> Result;
    for (const auto &P : Paths)
      for (const auto *O : P.Operations)
        if (O->Addr != InvalidVA && O->OriginSeq >= 0)
          Result.insert({O->Addr, O->OriginSeq});
    return {Result.begin(), Result.end()};
  }
  std::optional<std::vector<LibraryOccurrence>> inlineRegion() {
    // Trace only the verified effects and the guards used to prove them.
    // Prologue saves, caller arithmetic and return transport stay visible.
    std::set<const MedOp *> Core = UsedGuards;
    for (const auto &P : Paths) {
      std::set<Key> Seen;
      std::function<bool(MedVar, unsigned)> Collect = [&](MedVar V,
                                                          unsigned Depth) {
        if (!spend() ||
            Depth >= ::neverd::limits::ir::
                         kMedComLibraryRecognitionInlineRegionCollectDepthLimit)
          return false;
        auto C = value(V, P);
        if (!C)
          return false;
        if (C->isConst())
          return true;
        if (!Seen.insert(key(*C)).second)
          return true;
        const auto *D = definition(*C);
        if (!D)
          return C->Kind == MedVar::Reg && C->SSAVer == 0;
        if (!plain(*D) || (!scalar(D->Opcode) && D->Opcode != NdOp::LOAD) ||
            llvm::find(P.Operations, D) == P.Operations.end())
          return false;
        if (D->Opcode == NdOp::LOAD && stackAddress(D->Inputs[0], P))
          return false;
        Core.insert(D);
        for (unsigned I = 0; I < D->NumInputs; ++I)
          if (!Collect(D->Inputs[I], Depth + 1))
            return false;
        return true;
      };
      for (const auto &E : P.Effects) {
        Core.insert(E.Operation);
        for (unsigned I = 0; I < E.Operation->NumInputs; ++I)
          if (!Collect(E.Operation->Inputs[I], 0))
            return std::nullopt;
        if (!Collect(E.Value, 0))
          return std::nullopt;
      }
      for (const auto *O : P.Operations)
        if (UsedGuards.contains(O) && !Collect(O->Inputs[1], 0))
          return std::nullopt;
    }
    // A value escaping into an independent observable computation prevents
    // folding, even when the lifetime policy itself was recognized.
    std::function<bool(const MedVar &, unsigned)> Unobserved = [&](const MedVar
                                                                       &V,
                                                                   unsigned
                                                                       Depth) {
      if (!spend() ||
          Depth >=
              ::neverd::limits::ir::
                  kMedComLibraryRecognitionInlineRegionUnobservedDepthLimit ||
          !V.Size)
        return false;
      for (const auto &B : F.Blocks) {
        for (const auto &P : B.Phis)
          for (const auto &[Pred, Input] : P.Args) {
            (void)Pred;
            if (same(Input, V))
              return false;
          }
        for (const auto &O : B.Ops) {
          if (!spend())
            return false;
          if (O.Dead)
            continue;
          for (unsigned I = 0; I < O.NumInputs; ++I)
            if (same(O.Inputs[I], V) &&
                (!scalar(O.Opcode) || !O.Output.Size || same(O.Output, V) ||
                 !Unobserved(O.Output, Depth + 1)))
              return false;
        }
      }
      return true;
    };
    for (const auto &P : Paths)
      for (const auto *O : P.Operations) {
        if (Core.contains(O) || O->Opcode == NdOp::COPY ||
            (scalar(O->Opcode) && Unobserved(O->Output, 0)))
          continue;
        for (unsigned I = 0; I < O->NumInputs; ++I) {
          auto V = value(O->Inputs[I], P);
          if (!V)
            return std::nullopt;
          const auto *D = definition(*V);
          if (!Core.contains(D))
            continue;
          // The original virtual call's machine result may remain live at a
          // void source return. Keep that boundary explicit; folding never
          // rewrites the call, its result assignment, or its caller's uses.
          if ((D->Opcode != NdOp::CALL && D->Opcode != NdOp::INDIR_CALL) ||
              !D->Output.Size || D->OriginSeq < 0 || D->Addr == InvalidVA)
            return std::nullopt;
          const LibraryOccurrence Output{D->Addr, D->OriginSeq};
          if (EscapingResult && *EscapingResult != Output)
            return std::nullopt;
          EscapingResult = Output;
        }
      }
    std::set<LibraryOccurrence> Result;
    for (const auto *O : Core)
      if (O->Addr != InvalidVA && O->OriginSeq >= 0)
        Result.insert({O->Addr, O->OriginSeq});
    return Result.empty() ? std::nullopt
                          : std::optional(std::vector<LibraryOccurrence>(
                                Result.begin(), Result.end()));
  }
  std::optional<LibraryOccurrence> escapingResult() const {
    return EscapingResult;
  }

private:
  bool terminalExceptionExit(const ExceptionalEdge &Edge) const {
    const auto *EH = F.ExceptionMetadata ? &*F.ExceptionMetadata : nullptr;
    const auto *Context = Edge.singleContext();
    if (!EH || EH->ParseStatus != ExceptionParseStatus::Complete || !EH->Cxx ||
        !EH->Cxx->hasValidStateGraph() || !EH->Cxx->TryBlocks.empty() ||
        EH->Cxx->IsCatchFunclet || EH->Cxx->IsSeparated ||
        EH->Cxx->UnwindMap.size() != 1 ||
        Edge.Kind != ExceptionalEdgeKind::CxxCleanup || Edge.BlockId != -1 ||
        !Context || Context->State != 0 || !Edge.TargetVA)
      return false;
    const auto &Action = EH->Cxx->UnwindMap.front();
    if (Action.ActionVA != Edge.TargetVA || Action.ToState != -1 ||
        Action.Kind != CxxUnwindAction::ActionKind::Direct ||
        Action.ObjectOffset)
      return false;
    const auto *Import = Image.findImportStubAt(Edge.TargetVA);
    return Import && Import->Name == "__std_terminate" &&
           llvm::StringRef(Import->Module)
               .equals_insensitive("vcruntime140.dll");
  }
  const MedFunc &F;
  uint64_t Receiver, Source, Stack;
  size_t &Work;
  bool &Exhausted;
  const BinaryImage &Image;
  bool Valid = true;
  std::map<Key, const MedOp *> Definitions;
  std::vector<Path> Paths;
  std::set<const MedOp *> UsedGuards;
  std::optional<LibraryOccurrence> EscapingResult;
};
} // namespace

std::optional<LibraryRecognition> neverd::recognizeMedComLibraryOperation(
    const MedFunc &F, const BinaryImage &Image, const LibraryFeaturePack &Pack,
    const LibraryFeatureRule &Rule,
    llvm::ArrayRef<MedLibraryReceiver> Receivers,
    const std::set<std::string> &Names, size_t &Work, bool &Exhausted) {
  if (Image.Arch != Arch::X64 || Image.Format != BinaryFormat::COFF ||
      F.SkippedSSA)
    return std::nullopt;
  const auto Registers =
      getTargetRegInfo(Image.Arch).integerParamRegs(Image.Format);
  if (Registers.size() < 2)
    return std::nullopt;
  std::string Symbol;
  for (const auto &Name : Rule.WholeFunctionSymbols)
    if (Names.contains(Name)) {
      Symbol = Name;
      break;
    }
  const bool Whole =
      !Symbol.empty() && Rule.supports(LibraryFeatureScope::WholeFunction);
  if (!Whole && !Rule.supports(LibraryFeatureScope::InlineRegion))
    return std::nullopt;
  const auto Layout = Pack.Layouts.find(Rule.Layout);
  if (Layout == Pack.Layouts.end())
    return std::nullopt;
  auto Admitted = [&](llvm::StringRef Type) {
    auto D = Layout->second.DerivedReceivers.find(Type.str());
    return Type == Rule.ReceiverType ||
           (D != Layout->second.DerivedReceivers.end() && D->second == 0);
  };
  bool This = Whole, Source = Whole;
  std::string ReceiverEvidence;
  using Policy = LibraryFeatureRule::ComPolicy;
  const bool NeedsSource = Rule.Policy == Policy::StoreBeforeAddref ||
                           Rule.Policy == Policy::AddrefPublishRelease;
  for (const auto &R : Receivers) {
    if (R.Evidence.empty())
      continue;
    if (R.Register == Registers[0]) {
      if (!Admitted(R.Type) || R.ObjectBytes != Layout->second.ObjectBytes)
        return std::nullopt;
      This = true;
      ReceiverEvidence = R.Evidence;
    }
    if (NeedsSource && R.Register == Registers[1]) {
      if (!Admitted(R.Type) || R.ObjectBytes != Layout->second.ObjectBytes)
        return std::nullopt;
      Source = true;
    }
  }
  if (!This || (NeedsSource && !Source))
    return std::nullopt;
  ComMatcher Matcher(F, Registers[0], Registers[1],
                     getTargetRegInfo(Image.Arch).StackPointer, Work, Exhausted,
                     Image);
  if (!Matcher.verify(Rule, Whole) || Exhausted)
    return std::nullopt;
  auto Result = describeLibraryRecognition(F.Entry, Pack, Rule);
  Result.Scope = Whole ? LibraryFeatureScope::WholeFunction
                       : LibraryFeatureScope::InlineRegion;
  Result.LinkageName = Symbol;
  Result.IdentityEvidence = Whole ? "stated-member-symbol+com-lifetime"
                                  : ReceiverEvidence + "+com-lifetime";
  if (Whole)
    Result.DisplayName = displaySymbolName(Symbol);
  Result.Occurrences = Matcher.occurrences();
  Result.Isolated = Whole;
  if (!Whole)
    if (auto Region = Matcher.inlineRegion()) {
      Result.Occurrences = std::move(*Region);
      Result.Isolated = true;
      Result.ResultOccurrence = Matcher.escapingResult();
    }
  return Result;
}
