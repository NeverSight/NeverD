//===- HighCVirtualCalls.cpp - Declared virtual slot display -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover source names and compatible prototypes without replacing dispatch.
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/high/HighPointerTypeInference.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/ScopeExit.h"

using namespace neverd;

namespace {
/// Values available within a straight-line source region. Calls, address
/// escapes, control-flow boundaries and dependency writes end the region;
/// stores invalidate copies whose loaded bytes may overlap the write.
/// These substitutions prove receiver identity only; they never rewrite code.
struct ReceiverCopies {
  std::map<HighSourceLocalIdentity, ExprPtr> Values;
  std::set<HighSourceLocalIdentity> Reads;

  void clear() {
    Values.clear();
    Reads.clear();
  }

  ExprPtr resolveImpl(const ExprPtr &E,
                      llvm::DenseMap<const HighExpr *, ExprPtr> &Resolved,
                      unsigned Depth) const {
    if (!E || Depth > limits::ir::kHighPointerTypeMaxExpressionDepth)
      return nullptr;
    if (const auto Found = Resolved.find(E.get()); Found != Resolved.end())
      return Found->second;
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)
      if (auto I = Values.find(highSourceLocalIdentity(E->Var));
          I != Values.end()) {
        auto Result = resolveImpl(I->second, Resolved, Depth + 1);
        Resolved[E.get()] = Result;
        return Result;
      }
    if (E->Kind == ExprKind::Call || E->Kind == ExprKind::Store ||
        E->Kind == ExprKind::Addr || E->Kind == ExprKind::Undef ||
        E->IntrinsicId != Intrinsic::None ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return nullptr;
    ExprPtr Result = E;
    // This temporary graph proves value identity, not source type equality.
    // The vptr read and the call argument may have different scoped debug
    // declarations while still holding the same captured pointer bits.
    if (E->SourcePointerType) {
      Result = std::make_shared<HighExpr>(*E);
      Result->SourcePointerType.reset();
    }
    for (size_t I = 0; I < E->Operands.size(); ++I) {
      auto Child = resolveImpl(E->Operands[I], Resolved, Depth + 1);
      if (!Child)
        return nullptr;
      if (Child != E->Operands[I]) {
        if (Result == E)
          Result = std::make_shared<HighExpr>(*E);
        Result->Operands[I] = std::move(Child);
      }
    }
    Resolved[E.get()] = Result;
    return Result;
  }

  ExprPtr resolve(const ExprPtr &E) const {
    llvm::DenseMap<const HighExpr *, ExprPtr> Resolved;
    return resolveImpl(E, Resolved, 0);
  }

  template <typename Disjoint>
  void store(const ExprPtr &Address, uint64_t Bytes, Disjoint IsDisjoint) {
    const auto Written = resolve(Address);
    if (!Written || !Bytes) {
      clear();
      return;
    }
    std::map<HighSourceLocalIdentity, ExprPtr> Kept;
    for (const auto &[Key, Value] : Values) {
      const auto Resolved = resolve(Value);
      if (!Resolved)
        continue;
      bool Unchanged = true;
      HighExprSet Seen;
      const auto Visit = [&](auto &&Self, const ExprPtr &E) -> void {
        if (!E || !Unchanged || !Seen.insert(E.get()).second)
          return;
        if (E->Kind == ExprKind::Load)
          Unchanged =
              E->Type && E->Operands.size() == 1 && E->Operands[0] &&
              IsDisjoint(*E->Operands[0], *Written, E->Type->Size, Bytes);
        E->forEachChildExpr([&](const ExprPtr &Child) { Self(Self, Child); });
      };
      Visit(Visit, Resolved);
      if (Unchanged)
        Kept.emplace(Key, Resolved);
    }
    Values = std::move(Kept);
  }

  void assign(const HighStmt &S) {
    if (!S.Dst || S.Dst->Kind != ExprKind::Var ||
        S.Dst->Var.Kind == MedVar::Stack || !S.Val || !S.Dst->Type ||
        !S.Val->Type || S.Dst->Type->Size != S.Val->Type->Size ||
        (S.Dst->Type->Kind != NdTypeKind::Int &&
         S.Dst->Type->Kind != NdTypeKind::Ptr) ||
        (S.Val->Type->Kind != NdTypeKind::Int &&
         S.Val->Type->Kind != NdTypeKind::Ptr))
      return;
    const auto Key = highSourceLocalIdentity(S.Dst->Var);
    HighExprSet Seen;
    bool SelfRead = false;
    const auto Visit = [&](auto &&Visit, const ExprPtr &E) -> void {
      if (!E || !Seen.insert(E.get()).second)
        return;
      if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) {
        const auto Input = highSourceLocalIdentity(E->Var);
        SelfRead |= Input == Key;
        Reads.insert(Input);
      }
      E->forEachChildExpr([&](const ExprPtr &Child) { Visit(Visit, Child); });
    };
    Visit(Visit, S.Val);
    if (!SelfRead)
      Values[Key] = S.Val;
  }
};
} // namespace

void HighCWriter::collectVirtualCallees(
    const std::vector<HighFunc> &Functions) {
  VirtualCallees.clear();
  if (!Opts.Image || !Dbg ||
      (Opts.Format != BinaryFormat::ELF && Opts.Format != BinaryFormat::MachO))
    return;
  const auto &Image = *Opts.Image;
  const unsigned Bytes = Image.getPointerSize();
  if (Bytes != 4 && Bytes != 8)
    return;
  std::optional<HighCImageNameIndex> OwnIndex;
  const HighCImageNameIndex *Index = SharedNameIndex;
  const auto LoadAddress = [&](const HighExpr *E) -> const HighExpr * {
    E = peelIntegerViewOps(E);
    return E && E->Kind == ExprKind::Load && E->Type &&
                   E->Type->Size == Bytes && E->Operands.size() == 1 &&
                   E->MemoryOrdering == NdMemoryOrdering::None &&
                   E->MemoryAddressSpace == NdMemoryAddressSpace::Default
               ? peelIntegerViewOps(E->Operands[0].get())
               : nullptr;
  };
  std::map<std::pair<std::string, uint64_t>, std::optional<VirtualCallee>>
      Slots;
  const auto Resolve = [&](const std::string &Class,
                           uint64_t Offset) -> std::optional<VirtualCallee> {
    if (!Index) {
      OwnIndex = collectImageNameIndex(Image);
      Index = &*OwnIndex;
    }
    auto [Cached, Fresh] = Slots.try_emplace({Class, Offset});
    if (!Fresh)
      return Cached->second;
    const auto Slot = Index->VirtualTables.resolve(Image, Class, Offset);
    if (!Slot)
      return std::nullopt;
    const va_t Target = Slot->Target;
    auto Declaration = Dbg->resolveFunction(Target);
    if (!Declaration || Declaration->Addr != Target ||
        Declaration->Name.empty())
      return std::nullopt;
    VirtualCallee Result;
    Result.Function = std::move(*Declaration);
    Result.Symbol = Index->functionNameAt(Image, Target);
    Result.SlotOffset = Offset;
    Result.Table = Slot->Table;
    Result.Slot = Slot->Slot;
    Result.ReceiverType = Class;
    Cached->second = Result;
    return Result;
  };
  const HighFunc *Previous = CurrentFunc;
  for (const HighFunc &Function : Functions) {
    HighExprSet Seen;
    bool HasVirtualShape = false;
    const auto Find = [&](auto &&Self, const ExprPtr &E) -> void {
      if (!E || HasVirtualShape || !Seen.insert(E.get()).second)
        return;
      HasVirtualShape = E->Kind == ExprKind::Call && E->IsIndirectCall &&
                        E->IntrinsicId == Intrinsic::None && !E->SourceCallHint;
      E->forEachChildExpr([&](const ExprPtr &Child) { Self(Self, Child); });
    };
    walkStmts(Function.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) { Find(Find, E); });
    });
    if (!HasVirtualShape)
      continue;
    CurrentFunc = &Function;
    std::vector<TypeRef> Parameters;
    std::optional<HighPointerTypeFacts> Facts;
    ReceiverCopies Copies;
    HighExprSet Rejected;
    const auto Visit = [&](auto &&Self, const ExprPtr &E) -> void {
      if (!E || !Seen.insert(E.get()).second)
        return;
      E->forEachChildExpr([&](const ExprPtr &Child) { Self(Self, Child); });
      if (E->Kind != ExprKind::Call || !E->IsIndirectCall ||
          E->IntrinsicId != Intrinsic::None || E->SourceCallHint ||
          E->Operands.empty())
        return;
      if (Rejected.contains(E.get()))
        return;
      auto Reject = llvm::make_scope_exit([&] {
        Rejected.insert(E.get());
        VirtualCallees.erase(E.get());
      });
      const auto Target = Copies.resolve(E->IndirectTarget);
      const auto Actual = Copies.resolve(E->Operands[0]);
      const HighExpr *Slot = LoadAddress(Target.get());
      uint64_t Offset = 0;
      if (Slot && Slot->Kind == ExprKind::BinOp && Slot->Op == NdOp::INT_ADD &&
          Slot->Operands.size() == 2 && Slot->Operands[1] &&
          Slot->Operands[1]->Kind == ExprKind::Const) {
        Offset = Slot->Operands[1]->ConstVal;
        Slot = peelIntegerViewOps(Slot->Operands[0].get());
      }
      const HighExpr *Receiver = LoadAddress(Slot);
      const HighExpr *Argument = peelIntegerViewOps(Actual.get());
      if (!Receiver || !Argument || !Receiver->structuralEq(*Argument))
        return;
      if (Parameters.empty())
        for (size_t I = 0; I < Function.Params.size(); ++I)
          Parameters.push_back(emittedParamType(Function, I));
      const auto ReceiverType = [&](auto &&Self, const HighExpr *Value,
                                    unsigned Depth = 0) -> TypeRef {
        if (!Value || !Value->Type || Value->Type->Size != Bytes ||
            (Value->Type->Kind != NdTypeKind::Int &&
             Value->Type->Kind != NdTypeKind::Ptr) ||
            Depth > limits::ir::kHighPointerTypeMaxExpressionDepth)
          return nullptr;
        TypeRef Type = Value->Type;
        if (TypeRef Global = globalPointerType(*Value))
          return Global;
        if (Value->Kind == ExprKind::Var || Value->Kind == ExprKind::Phi) {
          const auto &V = Value->Var;
          if (TypeRef Stated = statedVariableType(V))
            Type = std::move(Stated);
          else if (Value->SourcePointerType)
            Type = Value->SourcePointerType;
          else if (V.Kind == MedVar::Param && V.SSAVer == 0 &&
                   V.RenameTag < 0 && V.Id >= 0 &&
                   size_t(V.Id) < Parameters.size())
            Type = Parameters[V.Id];
          else {
            if (!Facts)
              Facts = pointerTypeFacts(Function, Parameters);
            if (const auto F = Facts->find(highSourceLocalIdentity(V));
                F != Facts->end())
              Type = F->second;
          }
        }
        if (const HighExpr *Address = LoadAddress(Value))
          if (const auto Base = typedPointerOffset(*Address))
            if (TypeRef Holder = Self(Self, Base->first, Depth + 1);
                Holder && Holder->Kind == NdTypeKind::Ptr && Holder->Pointee &&
                Holder->Pointee->Kind == NdTypeKind::Struct) {
              Dbg->completeType(Holder);
              Type = Holder->Pointee->displayFieldTypeAt(
                  Base->second, Bytes, true, NdAccessClass::Integer);
            }
        return Type;
      };
      // Prefer the asserted type of the actual call operand. It can be a
      // user-typed local whose value came from an otherwise untyped load.
      const HighExpr *Original = peelIntegerViewOps(E->Operands[0].get());
      const bool Stated = Original &&
                          (Original->Kind == ExprKind::Var ||
                           Original->Kind == ExprKind::Phi) &&
                          statedVariableType(Original->Var);
      TypeRef Type = ReceiverType(ReceiverType, Original);
      if (!Stated && (!Type || Type->Kind != NdTypeKind::Ptr))
        Type = ReceiverType(ReceiverType, Receiver);
      if (!Stated && E->CallSiteAddr)
        if (const auto Local =
                Function.DebugCallReceiverTypes.find(E->CallSiteAddr);
            Local != Function.DebugCallReceiverTypes.end())
          Type = Local->second;
      if (!Type || Type->Kind != NdTypeKind::Ptr || Type->Size != Bytes ||
          !Type->Pointee || Type->Pointee->Kind != NdTypeKind::Struct)
        return;
      auto Callee = Resolve(Type->Pointee->SourceName, Offset);
      if (!Callee)
        return;
      Callee->Caller = Function.Entry;
      // A table names a declaration; it cannot change the native result
      // register class or invent missing call operands. Incompatible cases
      // get a navigation annotation only.
      const auto Prototype = checkedCalleeFunctionType(*E, Callee->Function);
      const auto Compatible = [&](const TypeRef &Native,
                                  const TypeRef &Source) {
        if (!Native || !Source)
          return false;
        if (Source->Kind == NdTypeKind::Void)
          return Native->Kind == NdTypeKind::Void;
        if (Source->Kind == NdTypeKind::Float)
          return Native->Kind == NdTypeKind::Float &&
                 Native->Size == Source->Size;
        return (Source->Kind == NdTypeKind::Int ||
                Source->Kind == NdTypeKind::Ptr || Source->IsEnum) &&
               (Native->Kind == NdTypeKind::Int ||
                Native->Kind == NdTypeKind::Ptr) &&
               (Native->Size == Bytes || Native->Size == Source->Size) &&
               Source->Size <= Bytes;
      };
      Callee->Compatible = Prototype &&
                           Prototype->ParamTypes.size() == E->Operands.size() &&
                           Compatible(E->Type, Prototype->RetType) &&
                           E->Type->Size == Prototype->RetType->Size;
      if (Callee->Compatible)
        for (size_t I = 0; I < E->Operands.size(); ++I)
          Callee->Compatible &=
              Compatible(E->Operands[I]->Type, Prototype->ParamTypes[I]);
      VirtualCallees.emplace(E.get(), std::move(*Callee));
      Reject.release();
    };
    const auto Scan = [&](auto &&Self,
                          const std::vector<HighStmt> &Body) -> void {
      Copies.clear();
      for (const HighStmt &S : Body) {
        ExprPtr StoreAddress, StoreValue;
        if (S.Kind == StmtKind::Store) {
          StoreAddress = S.StoreAddr;
          StoreValue = S.StoreVal;
        } else if (S.Kind == StmtKind::Assign && S.Dst &&
                   S.Dst->Kind == ExprKind::Load &&
                   S.Dst->Operands.size() == 1) {
          StoreAddress = S.Dst->Operands[0];
          StoreValue = S.Val;
        }
        const bool Straight =
            S.Kind == StmtKind::Assign || S.Kind == StmtKind::Call ||
            S.Kind == StmtKind::Return || S.Kind == StmtKind::Store;
        if (!Straight || S.IsPhiCopy)
          Copies.clear();
        if (S.Dst &&
            (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)) {
          const auto Key = highSourceLocalIdentity(S.Dst->Var);
          if (Copies.Reads.count(Key) || Copies.Values.count(Key))
            Copies.clear();
        }
        unsigned Calls = 0;
        bool Effects = S.MemoryOrdering != NdMemoryOrdering::None ||
                       S.MemoryAddressSpace != NdMemoryAddressSpace::Default;
        Seen.clear();
        const auto Inspect = [&](auto &&Inspect, const ExprPtr &E) -> void {
          if (!E)
            return;
          Calls += E->Kind == ExprKind::Call;
          if (!Seen.insert(E.get()).second)
            return;
          Effects |= E->Kind == ExprKind::Store || E->Kind == ExprKind::Addr ||
                     E->IntrinsicId != Intrinsic::None ||
                     !E->IntrinsicOutputs.empty() ||
                     E->MemoryOrdering != NdMemoryOrdering::None ||
                     E->MemoryAddressSpace != NdMemoryAddressSpace::Default;
          E->forEachChildExpr(
              [&](const ExprPtr &Child) { Inspect(Inspect, Child); });
        };
        forEachExpr(S, [&](const ExprPtr &E) { Inspect(Inspect, E); });
        if (Effects || Calls > 1)
          Copies.clear();
        Seen.clear();
        forEachExpr(S, [&](const ExprPtr &E) { Visit(Visit, E); });
        if (Effects || Calls || !Straight)
          Copies.clear();
        else if (StoreAddress)
          Copies.store(StoreAddress,
                       StoreValue && StoreValue->Type ? StoreValue->Type->Size
                                                      : 0,
                       [this](const HighExpr &A, const HighExpr &B,
                              uint64_t ABytes, uint64_t BBytes) {
                         return disjointMemory(A, B, ABytes, BBytes);
                       });
        else if (S.Kind == StmtKind::Assign)
          Copies.assign(S);
        if (!S.Body.empty())
          Self(Self, S.Body);
        if (!S.ElseBody.empty())
          Self(Self, S.ElseBody);
        for (const auto &Case : S.Cases)
          Self(Self, Case.Body);
        if (!S.DefaultBody.empty())
          Self(Self, S.DefaultBody);
        for (const auto &Clause : S.EHClauseBodies)
          Self(Self, Clause);
        if (!Straight)
          Copies.clear();
      }
    };
    Scan(Scan, Function.Body);
  }
  CurrentFunc = Previous;
}
