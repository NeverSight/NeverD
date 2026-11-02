#ifndef NEVERD_SDK_CAPI_OBJCNATIVESWIFTRECEIVERSOURCES_H
#define NEVERD_SDK_CAPI_OBJCNATIVESWIFTRECEIVERSOURCES_H

#include "../../loader/Swift/SwiftMangledClassMethodABI.h"
#include "ObjCNativeCurrentFunction.h"
#include "ObjCNativeSourceCallCallees.h"
#include "ObjCSourceBindings.h"
#include "SourceExpressionIdentity.h"

#include "neverd/ir/high/AArch64AddressMemory.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/sdk/Limits.h"

namespace neverd::sdk {
namespace native_swift_receiver_detail {
using Calls = std::map<SourceCallOccurrenceKey, const HighExpr *>;
struct BodyProof {
  const BinaryImage &Image;
  const HighFunc &Function;
  std::map<HighSourceLocalIdentity, std::vector<ExprPtr>> Definitions;
  std::set<HighSourceLocalIdentity> AddressTaken, Active;
  std::set<HighSourceLocalIdentity> Comparing;
  size_t Budget = kSourceExpressionWork;

  bool word(const ExprPtr &E) const {
    return E && E->Type && E->Type->Size == 8 &&
           (E->Type->Kind == NdTypeKind::Ptr ||
            E->Type->Kind == NdTypeKind::Int) &&
           E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
           E->MemoryOrdering == NdMemoryOrdering::None &&
           E->MemoryAddressSpace == NdMemoryAddressSpace::Default;
  }
  bool originalMemoryWord(const ExprPtr &E) const {
    if (Image.Arch != Arch::AArch64 || !E || E->SourceCallHint ||
        !aarch64AddressMemoryHighShapeIsValid(*E))
      return false;
    const auto Instruction = aarch64AddressMemoryInstruction(
        E->Operands[kAddressMemoryWordOperand]->ConstVal);
    return Instruction->isLoad() && !Instruction->stackBase() &&
           Instruction->hasRegisterIndex() &&
           Instruction->usesFullWidthIndex() &&
           Instruction->accessBytes() == kAddressMemoryPointerBytes &&
           !Instruction->indexShift();
  }
  template <class Match>
  bool resolve(const ExprPtr &E, const Match &Leaf, unsigned Depth = 0) {
    if (!Budget ||
        Depth >=
            ::neverd::sdk::kObjCNativeSwiftReceiverSourcesResolveDepthLimit ||
        (!word(E) && !originalMemoryWord(E)))
      return false;
    --Budget;
    if (E->Kind == ExprKind::Cast && E->Operands.size() == 1 &&
        (!E->CastTo || equalSourceTypes(E->CastTo, E->Type)) &&
        !E->SourceCallHint)
      return resolve(E->Operands[0], Leaf, Depth + 1);
    if (E->Kind == ExprKind::Var && E->Var.Kind != MedVar::Param &&
        E->Operands.empty() && !E->SourceCallHint) {
      const auto Key = highSourceLocalIdentity(E->Var);
      const auto D = Definitions.find(Key);
      if (E->Var.Size != 8 || AddressTaken.count(Key) ||
          D == Definitions.end() || D->second.size() != 1 ||
          !Active.insert(Key).second)
        return false;
      const bool Valid = resolve(D->second.front(), Leaf, Depth + 1);
      Active.erase(Key);
      return Valid;
    }
    return Leaf(E, Depth);
  }
  bool offset(const ExprPtr &E, va_t Slot, unsigned Depth) {
    return resolve(
        E,
        [&](const ExprPtr &W, unsigned) {
          if (W->Kind == ExprKind::Call && W->SourceCallHint) {
            const auto &B = *W->SourceCallHint;
            return B.CallKind == SourceCallTypeHint::Kind::RuntimeIvarOffset &&
                   B.TargetAddress == Slot && W->Operands.empty() &&
                   !W->CallAddr && !W->IsIndirectCall &&
                   objcSourceCallBound(*W, Image, {});
          }
          return W->Kind == ExprKind::Load && W->Operands.size() == 1 &&
                 !W->SourceCallHint && W->Operands[0] &&
                 W->Operands[0]->Kind == ExprKind::Const &&
                 W->Operands[0]->Operands.empty() &&
                 W->Operands[0]->ConstVal == Slot && word(W->Operands[0]);
        },
        Depth);
  }
  bool receiver(const ExprPtr &E, const ObjCReceiverTypeHint &Root,
                size_t Steps, unsigned Depth = 0) {
    if (Depth >=
        ::neverd::sdk::kObjCNativeSwiftReceiverSourcesReceiverDepthLimit)
      return false;
    return resolve(
        E,
        [&](const ExprPtr &V, unsigned D) {
          if (!Steps)
            return V->Kind == ExprKind::Var && V->Var.Kind == MedVar::Param &&
                   V->Var.Id == static_cast<int>(Root.SourceParameter) &&
                   V->Var.Size == 8 && V->Operands.empty() &&
                   !V->SourceCallHint &&
                   !Definitions.count(highSourceLocalIdentity(V->Var)) &&
                   !AddressTaken.count(highSourceLocalIdentity(V->Var));
          const auto &Step = Root.Steps[Steps - 1];
          if (Step.TheKind != ObjCReceiverTypeHint::TypeStep::Kind::IvarLoad ||
              Step.ByteOffset || Step.OffsetWidth != 8 ||
              !Step.Selector.empty() || V->SourceCallHint)
            return false;
          if (originalMemoryWord(V))
            return receiver(V->Operands[kAddressMemoryBaseOperand], Root,
                            Steps - 1, D + 1) &&
                   offset(V->Operands[kAddressMemoryIndexOperand],
                          Step.OffsetSlot, D + 1);
          if (V->Kind != ExprKind::Load || V->Operands.size() != 1)
            return false;
          return resolve(
              V->Operands[0],
              [&](const ExprPtr &Address, unsigned AD) {
                if (Address->Kind != ExprKind::BinOp ||
                    Address->Op != NdOp::INT_ADD ||
                    Address->Operands.size() != 2 || Address->SourceCallHint)
                  return false;
                return (receiver(Address->Operands[0], Root, Steps - 1,
                                 AD + 1) &&
                        offset(Address->Operands[1], Step.OffsetSlot,
                               AD + 1)) ||
                       (receiver(Address->Operands[1], Root, Steps - 1,
                                 AD + 1) &&
                        offset(Address->Operands[0], Step.OffsetSlot, AD + 1));
              },
              D + 1);
        },
        Depth);
  }
  bool sameArgument(const ExprPtr &E, const ExprPtr &A, BodyProof &Actual,
                    unsigned Depth = 0, bool FollowDefinitions = true) {
    if (!E || !A || !Budget ||
        Depth >= ::neverd::sdk::
                     kObjCNativeSwiftReceiverSourcesSameArgumentDepthLimit)
      return false;
    --Budget;
    HighExpr Left = *E, Right = *A;
    // Binding materializes runtime selector/ivar queries with fresh hint
    // objects. Their complete current declarations still own those leaves.
    if (E->Kind == ExprKind::Call && E->SourceCallHint && A->SourceCallHint &&
        E->SourceCallHint != A->SourceCallHint) {
      const auto &X = *E->SourceCallHint, &Y = *A->SourceCallHint;
      if (!E->Operands.empty() || !A->Operands.empty() ||
          (X.CallKind != SourceCallTypeHint::Kind::RuntimeSelector &&
           X.CallKind != SourceCallTypeHint::Kind::RuntimeIvarOffset &&
           X.CallKind !=
               SourceCallTypeHint::Kind::RuntimeProfileCounterStorage) ||
          X.CallKind != Y.CallKind || X.TargetAddress != Y.TargetAddress ||
          X.TargetName != Y.TargetName || X.OwnerClass != Y.OwnerClass ||
          X.ByteCount != Y.ByteCount ||
          !equalSourceABIs(X.Signature, Y.Signature) ||
          !objcSourceCallBound(*E, Image, {}) ||
          !objcSourceCallBound(*A, Image, {}))
        return false;
      Right.SourceCallHint = Left.SourceCallHint;
    }
    if (E->Operands.size() != A->Operands.size())
      return false;
    Left.Operands.clear();
    Right.Operands.clear();
    if (!sameSourceExpressionIdentity(Left, Right, Budget))
      return false;
    if (E->Kind == ExprKind::Var && highSourceFrameBase(Function, E->Var))
      return highSourceFrameBase(Actual.Function, A->Var) &&
             Function.FrameSize == Actual.Function.FrameSize &&
             Function.FrameHeadroom == Actual.Function.FrameHeadroom &&
             !Definitions.count(highSourceLocalIdentity(E->Var)) &&
             !Actual.Definitions.count(highSourceLocalIdentity(A->Var));
    if (FollowDefinitions && E->Kind == ExprKind::Var &&
        E->Var.Kind != MedVar::Param) {
      const auto Key = highSourceLocalIdentity(E->Var);
      const auto D = Definitions.find(Key),
                 Other = Actual.Definitions.find(Key);
      if (AddressTaken.count(Key) || Actual.AddressTaken.count(Key) ||
          D == Definitions.end() || Other == Actual.Definitions.end() ||
          D->second.size() != 1 || Other->second.size() != 1 ||
          !Comparing.insert(Key).second)
        return false;
      const bool Valid =
          sameArgument(D->second[0], Other->second[0], Actual, Depth + 1);
      Comparing.erase(Key);
      return Valid;
    }
    for (size_t I = 0; I < E->Operands.size(); ++I)
      if (!sameArgument(E->Operands[I], A->Operands[I], Actual, Depth + 1,
                        FollowDefinitions))
        return false;
    return true;
  }
  bool sameCopyBody(BodyProof &Actual) {
    return sameStraightLineSourceBody(
        Function, Actual.Function,
        [&](const ExprPtr &E, const ExprPtr &A) {
          return sameArgument(E, A, Actual, 0, false);
        },
        Budget);
  }
  std::optional<Calls> calls(bool ValueCopies = false) {
    const auto Flow = analyzeHighSourceFlow(Function, false);
    const auto Graph = buildHighSourceFlowGraph(Function);
    if (!Flow.Complete || !Flow.Items.empty() || !Graph.Diagnostics.Complete ||
        !Graph.Diagnostics.Items.empty())
      return std::nullopt;
    Calls Result;
    std::set<const HighStmt *> Statements;
    std::vector<std::pair<ExprPtr, bool>> Pending;
    for (const auto &N : Graph.Nodes) {
      if (N.Test)
        Pending.emplace_back(N.Test, false);
      const auto *S = N.Statement;
      if (!S || !Statements.insert(S).second)
        continue;
      if (S->Kind == StmtKind::Assign && S->Dst && S->Val &&
          (S->Dst->Kind == ExprKind::Var || S->Dst->Kind == ExprKind::Phi)) {
        auto Value = S->Val;
        if (!S->Dst->Type || S->Dst->Type->Size != S->Dst->Var.Size ||
            S->MemoryOrdering != NdMemoryOrdering::None ||
            S->MemoryAddressSpace != NdMemoryAddressSpace::Default)
          Value.reset();
        Definitions[highSourceLocalIdentity(S->Dst->Var)].push_back(Value);
      }
      forEachExpr(*S,
                  [&](const ExprPtr &E) { Pending.emplace_back(E, false); });
    }
    while (!Pending.empty()) {
      if (!Budget)
        return std::nullopt;
      --Budget;
      auto [E, Address] = Pending.back();
      Pending.pop_back();
      if (!E)
        continue;
      Address |= E->Kind == ExprKind::Addr;
      if (Address && E->Kind == ExprKind::Var)
        AddressTaken.insert(highSourceLocalIdentity(E->Var));
      if (ValueCopies && E->SourceCallHint && E->SourceCallHint->ByValueCopy &&
          !E->SourceCallHint->NativeSwiftReceiver) {
        const auto &B = *E->SourceCallHint;
        if (E->Kind != ExprKind::Call || E->IsIndirectCall ||
            !isObjCByValueCopyHint(B, Function.Entry, Image.Arch) ||
            E->CallAddr != B.ByValueCopy->Site.StaticTarget ||
            B.TargetAddress != E->CallAddr ||
            !Result.emplace(B.ByValueCopy->Site, E.get()).second)
          return std::nullopt;
      } else if (!ValueCopies && E->SourceCallHint &&
                 E->SourceCallHint->NativeSwiftReceiver) {
        const auto &B = *E->SourceCallHint;
        if (E->Kind != ExprKind::Call || E->IsIndirectCall || !B.Receiver ||
            B.Receiver->Origin !=
                ObjCReceiverTypeHint::OriginKind::NativeSwiftSelf ||
            B.Receiver->Address != Function.Entry ||
            B.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
            E->CallAddr != B.NativeSwiftReceiver->StaticTarget ||
            B.TargetAddress != E->CallAddr ||
            !Result.emplace(*B.NativeSwiftReceiver, E.get()).second)
          return std::nullopt;
      }
      E->forEachChildExpr(
          [&](const ExprPtr &Child) { Pending.emplace_back(Child, Address); });
    }
    for (const auto &[Site, Call] : Result)
      if (!ValueCopies)
        if (Call->Operands.empty() ||
            !objcReceiverTypeHintValid(Image,
                                       *Call->SourceCallHint->Receiver) ||
            !receiver(Call->Operands[0], *Call->SourceCallHint->Receiver,
                      Call->SourceCallHint->Receiver->Steps.size()))
          return std::nullopt;
    return Result;
  }
};
} // namespace native_swift_receiver_detail

inline bool objCNativeSwiftReceiverSourceCallBound(
    const HighExpr &Expression, const BinaryImage &Image,
    const PipelineResult &Result, const HighFunc &Function,
    const std::map<va_t, const HighFunc *> &Functions) {
  using namespace native_swift_receiver_detail;
  if (!Expression.SourceCallHint ||
      !Expression.SourceCallHint->NativeSwiftReceiver || !Result.Success ||
      Result.SourceImage != &Image || !Function.SourceTypeHint ||
      Function.DoesNotReturn || Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions)
    return false;
  const auto Current =
      native_source_detail::currentFunction(Result, Function.Entry);
  const auto Declaration =
      swiftMangledReceiverClassMethodSourceABI(Image, Function.Entry);
  if (!Current || !Declaration ||
      !validateNativeSwiftReceiverBindings(Image, Current->Low,
                                           *Current->Med) ||
      Function.Params.size() != Declaration->Parameters.size() ||
      !std::equal(Function.Params.begin(), Function.Params.end(),
                  Declaration->Parameters.begin(),
                  [](const auto &P, const auto &D) {
                    return equalSourceTypes(P.Type, D.Type);
                  }) ||
      !equalSourceABIs(*Function.SourceTypeHint, *Declaration) ||
      !equalSourceABIs(*Current->Med->SourceTypeHint, *Declaration))
    return false;
  auto Callees = nativePublicationCallees(Result, *Current->Med);
  for (auto It = Callees.begin(); It != Callees.end();) {
    const auto Callee =
        native_source_detail::currentFunction(Result, It->first);
    if (!Callee || !equalSourceABIs(*Callee->High->SourceTypeHint, It->second))
      It = Callees.erase(It);
    else
      ++It;
  }
  const auto Hints = buildObjCSourceCallHints(Image, *Current->Low, nullptr,
                                              nullptr, &Callees);
  std::map<SourceCallOccurrenceKey, const SourceCallTypeHint *> Expected;
  for (const auto &[Address, Hint] : Hints)
    if (Hint.NativeSwiftReceiver)
      Expected.emplace(*Hint.NativeSwiftReceiver, &Hint);
  if (Expected.empty())
    return false;
  std::set<SourceCallOccurrenceKey> Seen;
  size_t Budget = kSourceExpressionWork;
  for (const auto &B : Current->Med->Blocks)
    for (const auto &Op : B.Ops) {
      if (!Budget--)
        return false;
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      const SourceCallOccurrenceKey Site{
          Op.Addr, Op.OriginSeq, Op.Opcode,
          Op.NumInputs && Op.Inputs[0].isConst() ? Op.Inputs[0].ConstVal : 0};
      const auto Found = Expected.find(Site);
      if (Found == Expected.end()) {
        if (Op.SourceCallHint && Op.SourceCallHint->NativeSwiftReceiver)
          return false;
        continue;
      }
      if (!Op.SourceCallHint ||
          Op.SourceCallHint->NativeSwiftReceiver != Site ||
          Op.SourceCallHint->Receiver != Found->second->Receiver ||
          Op.SourceCallHint->ByValueCopy != Found->second->ByValueCopy ||
          Op.SourceCallHint->Selector != Found->second->Selector ||
          !equalSourceABIs(Op.SourceCallHint->Signature,
                           Found->second->Signature) ||
          Op.NumInputs !=
              sourceABIParameters(Found->second->Signature).size() + 1 ||
          Op.DoesNotReturn || Op.PreservesCallerSaved ||
          !Seen.insert(Site).second)
        return false;
    }
  if (Seen.size() != Expected.size())
    return false;
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Image);
  std::map<va_t, std::string> Names;
  for (const auto &F : Result.MedFuncs)
    Names.emplace(F.Entry, F.Name);
  Converter.setFuncNames(&Names);
  const auto Replay = Converter.convert(*Current->Med, Image.Arch);
  auto BoundReplay =
      bindObjCSourceReferences(Replay, Image, nullptr, &Functions);
  BodyProof Canonical{Image, Replay},
      BoundCanonical{Image, BoundReplay.Function};
  const auto RawExpected = Canonical.calls(),
             BoundExpected = BoundCanonical.calls();
  if (!RawExpected || !BoundExpected ||
      RawExpected->size() != Expected.size() ||
      BoundExpected->size() != Expected.size())
    return false;
  for (const auto *F : {Current->High, &Function}) {
    BodyProof Proof{Image, *F};
    const auto Calls = Proof.calls();
    if (!Calls || Calls->size() != Expected.size())
      return false;
    if (std::any_of(Expected.begin(), Expected.end(), [](const auto &Item) {
          return Item.second->ByValueCopy.has_value();
        })) {
      auto &ExpectedBody = F == Current->High ? Canonical : BoundCanonical;
      if (!ExpectedBody.sameCopyBody(Proof))
        return false;
    }
    for (const auto &[Site, Call] : *Calls) {
      const auto H = Expected.find(Site);
      const auto &ExpectedCalls =
          F == Current->High ? *RawExpected : *BoundExpected;
      auto &ExpectedProof = F == Current->High ? Canonical : BoundCanonical;
      const auto Original = ExpectedCalls.find(Site);
      if (Original == ExpectedCalls.end() ||
          Original->second->Operands.size() != Call->Operands.size())
        return false;
      // The receiver itself is proved from the exact source self and dynamic
      // field path above. All remaining arguments keep their canonical value
      // and any unique local definitions supplying it.
      for (size_t I = 1; I < Call->Operands.size(); ++I)
        if (!ExpectedProof.sameArgument(Original->second->Operands[I],
                                        Call->Operands[I], Proof))
          return false;
      if (H == Expected.end() ||
          Call->SourceCallHint->Receiver != H->second->Receiver ||
          Call->SourceCallHint->ByValueCopy != H->second->ByValueCopy ||
          Call->SourceCallHint->Selector != H->second->Selector ||
          !equalSourceABIs(Call->SourceCallHint->Signature,
                           H->second->Signature))
        return false;
    }
    if (F == &Function) {
      const auto Call =
          Calls->find(*Expression.SourceCallHint->NativeSwiftReceiver);
      if (Call == Calls->end() || Call->second != &Expression)
        return false;
    }
  }
  return objcSourceCallBound(Expression, Image, Functions, nullptr, nullptr,
                             &Function, nullptr, nullptr, true);
}
} // namespace neverd::sdk
#endif
