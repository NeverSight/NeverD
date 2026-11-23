//===- NdOpReturnLowering.cpp - RETURN lowering to HighIR -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// RETURN statement lowering: return-value recovery from registers, call
/// outputs, phi nodes, and predecessor blocks.
///
//===----------------------------------------------------------------------===//

#include "VariadicStackArguments.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/med/MedGoResults.h"
#include "neverd/ir/med/MedReturnArguments.h"
#include "neverd/ir/med/MedReturnValue.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace neverd {

namespace {
/// The parameter that arrives in register \p Reg, as the function's own
/// parameter variable; null when none does.
ExprPtr parameterInRegister(const MedFunc &Med, uint64_t Reg) {
  for (size_t I = 0; I < Med.Params.size(); ++I) {
    if (Med.Params[I].RegOff != Reg || I >= Med.TypedParams.size())
      continue;
    MedVar Parameter = Med.Params[I];
    Parameter.Kind = MedVar::Param;
    Parameter.Id = static_cast<int>(I);
    return HighExpr::makeVar(Parameter, Med.TypedParams[I].Type);
  }
  return nullptr;
}

/// A bare RET does not syntactically read the return-value register. A runtime
/// declaration can still describe a source value occupying that incoming ABI
/// slot (notably Objective-C self on AArch64). Bind it only when the complete
/// function leaves that carrier untouched; a call, partial write, or PHI makes
/// this fallback unavailable. This is a source hint, never safety evidence.
ExprPtr unchangedDeclaredReturnParameter(const MedFunc &Med,
                                         const TargetRegInfo &TRI,
                                         uint64_t ReturnReg) {
  if (!Med.SourceTypeHint || !Med.SourceTypeHint->ReturnType ||
      Med.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void)
    return nullptr;
  auto Overlaps = [&](const MedVar &Value) {
    if (Value.Kind != MedVar::Reg || !Value.Size)
      return false;
    return Value.RegOff >= ReturnReg
               ? Value.RegOff - ReturnReg < TRI.FullRegWidth
               : ReturnReg - Value.RegOff < Value.Size;
  };
  for (const auto &Block : Med.Blocks) {
    for (const auto &Phi : Block.Phis)
      if (Overlaps(Phi.Output))
        return nullptr;
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC)
        return nullptr;
      if (!Overlaps(Op.Output) || isRegisterLowView(Op))
        continue;
      const bool SelfCopy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                            Op.Inputs[0] == Op.Output &&
                            Op.Inputs[0].RegOff == Op.Output.RegOff &&
                            Op.Inputs[0].Size == Op.Output.Size;
      if (!SelfCopy)
        return nullptr;
    }
  }
  return parameterInRegister(Med, ReturnReg);
}

/// A compare reads the low bytes through a view. That view is not a write:
/// the register still holds the wider value the view was taken from. A real
/// 32-bit write is not a view, so the zeros it produces stay the value.
void widenRegisterLowView(const MedFunc &Med, MedVar &V) {
  if (V.Kind != MedVar::Reg)
    return;
  for (const MedBlock &Blk : Med.Blocks)
    for (const MedOp &Op : Blk.Ops) {
      if (!isRegisterLowView(Op) || Op.Output.Kind != V.Kind ||
          Op.Output.RegOff != V.RegOff || Op.Output.Size != V.Size ||
          Op.Output.Id != V.Id || Op.Output.SSAVer != V.SSAVer)
        continue;
      V = Op.Inputs[0];
      return;
    }
}

const MedBlock *onlyPredecessor(const MedFunc &Med, const MedBlock &Block) {
  if (Block.Preds.size() != 1)
    return nullptr;
  const int Id = Block.Preds.front();
  auto It = std::find_if(
      Med.Blocks.begin(), Med.Blocks.end(),
      [Id](const MedBlock &Candidate) { return Candidate.Id == Id; });
  return It == Med.Blocks.end() ? nullptr : &*It;
}
// Whether \p V is the value a register holds at entry to \p Med: named by
// the identity copy that begins a function, or defined nowhere.
bool isEntryIdentity(const MedFunc &Med, const MedVar &V) {
  auto Same = [&](const MedVar &W) {
    return W.Kind == V.Kind && W.RegOff == V.RegOff && W.Id == V.Id &&
           W.SSAVer == V.SSAVer;
  };
  for (const auto &Blk : Med.Blocks) {
    for (const auto &Phi : Blk.Phis)
      if (Same(Phi.Output))
        return false;
    for (const auto &Op : Blk.Ops)
      if (Same(Op.Output))
        return Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
               Same(Op.Inputs[0]);
  }
  return true;
}

} // namespace

void MedToHighConverter::lowerReturn(HighFunc &Func, const MedBlock &CurBlock,
                                     const MedOp &InputOp, const MedFunc &Med) {
  MedOp CurOp = InputOp;
  if (lowerX86RegistrationCatchReturn(Func, CurBlock, CurOp, Med))
    return;
  if (Med.SourceParametersBound && Med.SourceTypeHint)
    if (const auto Error = sourceABIErrorResult(*Med.SourceTypeHint)) {
      if (!SwiftErrorEntryInput || !CurOp.HasSourceErrorResult ||
          !CurOp.NumInputs || CurOp.Inputs[CurOp.NumInputs - 1].Size != 8)
        throw std::invalid_argument("Swift entry has no error-output operand");
      MedVar Slot;
      Slot.Kind = MedVar::Param;
      Slot.Id = static_cast<int>(Error->ParameterIndex);
      Slot.RegOff = Error->Location.RegisterOffset;
      Slot.Size = 8;
      Slot.TheArch = TargetArch;
      HighStmt Store;
      Store.Kind = StmtKind::Store;
      Store.Addr = CurOp.Addr;
      Store.StoreAddr = HighExpr::makeVar(
          Slot, Med.SourceTypeHint->Parameters[Error->ParameterIndex].Type);
      Store.StoreVal =
          sourceScalarValue(CurOp.Inputs[--CurOp.NumInputs], Error->Type);
      Func.Body.push_back(std::move(Store));
    }
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.Addr = CurOp.Addr;
  if (Med.CxxContinuationExitAnalysisComplete && InputOp.NumInputs == 1)
    for (const auto &Exit : Med.CxxContinuationExits)
      if (Exit.ReturnAddr == InputOp.Addr &&
          Exit.ReturnSeq == InputOp.OriginSeq && Exit.BlockId == CurBlock.Id &&
          Exit.ReturnValue == InputOp.Inputs[0])
        if (Exit.Complete)
          S.CxxContinuationReturnTargets = Exit.Targets;
  if (!S.CxxContinuationReturnTargets.empty()) {
    // This is a native pointer-valued dispatch result, even when a generic
    // return-width heuristic found a narrower integer in another register.
    S.RetVal = medvarToExpr(InputOp.Inputs[0]);
    Func.ReturnType = NdType::makeInt(InputOp.Inputs[0].Size, false);
    Func.Body.push_back(std::move(S));
    return;
  }

  // An established void declaration has no return carrier. Searching the
  // machine return register here would invent a value (and may move a prior
  // call into that value), despite the source ABI already proving it absent.
  if (Med.SourceParametersBound && Func.SourceTypeHint &&
      Func.SourceTypeHint->HasExplicitABI && Func.SourceTypeHint->ReturnType &&
      Func.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void &&
      Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Void) {
    Func.Body.push_back(std::move(S));
    return;
  }

  if (Med.SourceParametersBound && Func.SourceTypeHint &&
      Func.ReturnType->Kind == NdTypeKind::Struct) {
    const auto Members = sourceValueMembers(Func.ReturnType);
    std::vector<ExprPtr> Leaves;
    if (CurOp.NumInputs == Members.size())
      for (size_t I = 0; I < Members.size(); ++I)
        Leaves.push_back(sourceScalarValue(CurOp.Inputs[I], Members[I].Type));
    S.RetVal = HighExpr::makeRecord(Func.ReturnType, std::move(Leaves));
    Func.Body.push_back(std::move(S));
    return;
  }

  ExprPtr RetVal;
  if (Med.ExplicitX87ReturnValue && Med.FPReturnViaX87 && CurOp.NumInputs == 1)
    RetVal = sourceFloatValue(CurOp.Inputs[0], 10);
  const auto &TRI = getTargetRegInfo(TargetArch);
  const bool UsesFPReturnReg = Func.ReturnType &&
                               (Func.ReturnType->Kind == NdTypeKind::Float ||
                                Med.ReturnsVectorRegister) &&
                               TRI.hasFPReturnReg() && !Med.FPReturnViaX87;
  const uint64_t ReturnReg =
      UsesFPReturnReg ? TRI.FPReturnReg : TRI.IntReturnReg;
  const bool ExplicitABI =
      Med.SourceTypeHint && Med.SourceTypeHint->HasExplicitABI;
  // Whether op \p Op leaves the \p Size bytes at \p RegOff as they were: it
  // is no call, or a call whose callee provably keeps them.  After any other
  // call they hold what its callee left there.
  auto callKeepsRegister = [&](const MedOp &Op, uint64_t RegOff,
                               uint16_t Size) {
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      return true;
    return Op.PreservesCallerSaved ||
           callPreservesRegisterBytes(TargetArch, Op.CallPreservedGPRs, RegOff,
                                      Size);
  };
  auto ValueFromDefinition = [&](const MedOp &Definition) -> ExprPtr {
    ExprPtr Value;
    if (Definition.Opcode == NdOp::CALL ||
        Definition.Opcode == NdOp::INDIR_CALL ||
        Definition.Opcode == NdOp::INTRINSIC ||
        Definition.Opcode == NdOp::LOAD ||
        Definition.MemoryOrdering != NdMemoryOrdering::None ||
        Definition.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      Value = HighExpr::makeVar(Definition.Output);
    else
      Value = medOpToExpr(Definition);
    return Value ? forceInlineExpr(Value) : nullptr;
  };

  // A struct returned across several registers is the record of the values
  // its field registers hold at this RETURN, each found as the integer pair's
  // high register is below.
  // Parameter-only adapters (including MIPS16 entry transport) leave the
  // result in physical registers. A source return needs its own type hint.
  if ((!Med.SourceParametersBound || !Func.SourceTypeHint) && Func.ReturnType &&
      Func.ReturnType->Kind == NdTypeKind::Struct &&
      (Med.ExplicitMIPSRecordResult || Med.MultiReturn.size() >= 2) &&
      Func.ReturnType->Fields.size() == Med.MultiReturn.size()) {
    // Go's register results are a record because the function's callers
    // read them, and every return hands each one back
    // (materializeGoCallResults): a field the walks below still miss is a
    // value this function returns, printed as unknown, not a sign the record
    // is wrong.
    const bool GoResults =
        Image && Image->ExceptionMetadata.GoModule &&
        Image->ExceptionMetadata.GoModule->RegisterArguments &&
        isGoCode(*Image, Med.Entry) &&
        llvm::all_of(llvm::enumerate(Med.MultiReturn), [&](const auto &E) {
          return !E.value().IsFP &&
                 goResultPosition(TargetArch, E.value().RegOff) ==
                     static_cast<int>(E.index());
        });
    // A narrower Go result is the low bytes of the whole register.
    const uint16_t GoWidth = getTargetRegInfo(TargetArch).FullRegWidth;
    auto Reaching = [&](uint64_t RegOff, uint16_t Size) -> ExprPtr {
      // What the register holds at the end of \p Block: its last write
      // there, or none where a call after that write may change it.  None
      // when the block neither writes nor clobbers it.
      // A Go result is the whole register, which every write of it defines
      // (goReturnedValue): a narrower value, a view copy propagation left
      // on a temporary or the entry copy of the low byte, is no field.
      auto WrittenIn = [&](const MedBlock &Block) -> std::optional<ExprPtr> {
        for (auto RIt = Block.Ops.rbegin(); RIt != Block.Ops.rend(); ++RIt) {
          if (RIt->Opcode != NdOp::RETURN && RIt->Output.Kind == MedVar::Reg &&
              RIt->Output.Size > 0 && RIt->Output.RegOff == RegOff &&
              !isRegisterLowView(*RIt) &&
              (!GoResults || RIt->Output.Size >= GoWidth))
            return ValueFromDefinition(*RIt);
          if (!callKeepsRegister(*RIt, RegOff, Size))
            return ExprPtr();
        }
        if (const PhiNode *Phi = aggregateReturnPhi(Block, RegOff);
            Phi && (!GoResults || Phi->Output.Size >= GoWidth))
          return HighExpr::makeVar(Phi->Output);
        return std::nullopt;
      };
      if (std::optional<ExprPtr> Value = WrittenIn(CurBlock))
        return *Value;
      std::set<const MedBlock *> Seen{&CurBlock};
      for (const MedBlock *Pred = onlyPredecessor(Med, CurBlock);
           Pred && Seen.insert(Pred).second; Pred = onlyPredecessor(Med, *Pred))
        if (std::optional<ExprPtr> Value = WrittenIn(*Pred))
          return *Value;
      MedVar Entry;
      if (reachingRegAtBlockEntry(CurBlock, RegOff, Entry) && Entry.Size > 0) {
        // The reaching helper reports the compare's low view as the write.
        // The field still holds the register that view was taken from.
        widenRegisterLowView(Med, Entry);
        if (Entry.Size > 0 && !isEntryIdentity(Med, Entry) &&
            (!GoResults || Entry.Size >= GoWidth))
          return medvarToExpr(Entry);
      }
      return nullptr;
    };
    // A field narrower than its register (a float in an SSE eightbyte) is
    // merged into what the register held on entry.  Where no parameter
    // arrives there, those bytes hold nothing a caller passed: they are the
    // record's padding, and the field returns them as zero.
    auto OverEntryPadding = [&](const MedReturnReg &Field) -> ExprPtr {
      if (!Field.IsFP || std::any_of(Med.Params.begin(), Med.Params.end(),
                                     [&](const MedVar &P) {
                                       return P.RegOff == Field.RegOff;
                                     }))
        return nullptr;
      const MedOp *Merge = nullptr;
      std::set<const MedBlock *> Seen;
      for (const MedBlock *Block = &CurBlock;
           Block && !Merge && Seen.insert(Block).second;
           Block = onlyPredecessor(Med, *Block)) {
        for (auto RIt = Block->Ops.rbegin(); RIt != Block->Ops.rend(); ++RIt)
          if (RIt->Opcode != NdOp::RETURN && RIt->Output.Kind == MedVar::Reg &&
              RIt->Output.Size > 0 && RIt->Output.RegOff == Field.RegOff &&
              !isRegisterLowView(*RIt)) {
            Merge = &*RIt;
            break;
          }
        if (!Merge && std::any_of(Block->Phis.begin(), Block->Phis.end(),
                                  [&](const PhiNode &Phi) {
                                    return Phi.Output.Kind == MedVar::Reg &&
                                           Phi.Output.RegOff == Field.RegOff;
                                  }))
          return nullptr;
      }
      if (!Merge || Merge->Opcode != NdOp::CONCAT || Merge->NumInputs != 2)
        return nullptr;
      const MedVar &High = Merge->Inputs[0], &Low = Merge->Inputs[1];
      if (!Low.Size || Low.Size >= Field.Size || High.isConst())
        return nullptr;
      const MedOp *Slice = uniqueMedDefinition(High);
      if (!Slice || Slice->Opcode != NdOp::SUBBYTES || Slice->NumInputs != 2 ||
          !Slice->Inputs[1].isConst() ||
          Slice->Inputs[1].ConstVal != Low.Size ||
          Slice->Inputs[0].Kind != MedVar::Reg ||
          Slice->Inputs[0].RegOff != Field.RegOff ||
          !isEntryIdentity(Med, Slice->Inputs[0]))
        return nullptr;
      ExprPtr Bits = sourceBitSlice(medvarToExpr(Low), 0, Low.Size);
      if (!Bits->Type || Bits->Type->Kind != NdTypeKind::Int)
        Bits = HighExpr::makeBitCast(std::move(Bits),
                                     NdType::makeInt(Low.Size, false));
      ExprPtr Padded = HighExpr::makeUnary(NdOp::INT_ZEXT, std::move(Bits));
      Padded->Type = NdType::makeInt(Field.Size, false);
      return Padded;
    };
    const auto Argument = [&](const MedReturnReg &Field) -> ExprPtr {
      if (std::none_of(
              Med.Params.begin(), Med.Params.end(),
              [&](const MedVar &P) { return P.RegOff == Field.RegOff; }))
        return nullptr;
      MedVar Param;
      Param.Kind = MedVar::Param;
      Param.Id = -1;
      Param.RegOff = Field.RegOff;
      Param.Size = Field.Size;
      Param.TheArch = TargetArch;
      return medvarToExpr(Param);
    };
    llvm::DenseMap<int, const MedBlock *> GoBlocks;
    std::vector<ExprPtr> Leaves;
    for (size_t I = 0; I < Med.MultiReturn.size(); ++I) {
      const MedReturnReg &Field = Med.MultiReturn[I];
      ExprPtr Value;
      if (Med.ExplicitMIPSRecordResult) {
        if (CurOp.NumInputs != Med.MultiReturn.size() ||
            CurOp.Inputs[I].Size != Field.Size)
          throw std::runtime_error(
              "A MIPS record return lost a bound machine word");
        Value = medvarToExpr(CurOp.Inputs[I]);
      } else {
        Value = OverEntryPadding(Field);
      }
      if (!Value)
        Value = Reaching(Field.RegOff, Field.Size);
      // A field the function never writes returns the argument its caller
      // passed in the register, which recoverCallAbi made a parameter.
      if (!Value && returnsArgumentUnchanged(
                        Med, Field, getTargetRegInfo(TargetArch),
                        Image ? Image->abiFormat() : BinaryFormat::Unknown))
        Value = Argument(Field);
      // A Go result is what the path to this return leaves in the register
      // (goReturnedValue), as materializeGoCallResults found it: the
      // argument where the path leaves it unchanged, whatever other paths
      // write there (`if n.Bytes == nil { return "" }` returns the nil
      // receiver as the pointer), or the whole register's value past a join.
      if (!Value && GoResults) {
        if (GoBlocks.empty())
          for (const MedBlock &Block : Med.Blocks)
            GoBlocks.try_emplace(Block.Id, &Block);
        const GoReturnedValue Returned =
            goReturnedValue(GoBlocks, CurBlock, Field.RegOff, TargetArch);
        if (Returned.What == GoReturnedValue::Entry)
          Value = Argument(Field);
        else if ((Returned.What == GoReturnedValue::Defined ||
                  Returned.What == GoReturnedValue::Phi) &&
                 Returned.Value)
          Value = HighExpr::makeVar(*Returned.Value);
      }
      if (!Value && GoResults)
        Value = HighExpr::makeUndef(Field.Size);
      if (!Value)
        throw std::runtime_error(
            "cannot recover a field of a multi-register struct return");
      ExprPtr Bits = sourceBitSlice(Value, 0, Field.Size);
      // The register holds the member's bits whatever type computed them:
      // `fmov x0, d0` returns a double in an integer field.
      const TypeRef &Member = Func.ReturnType->Fields[I];
      if (!Bits->Type || !equalSourceTypes(Member, Bits->Type))
        Bits = HighExpr::makeBitCast(std::move(Bits), Member);
      Leaves.push_back(std::move(Bits));
    }
    S.RetVal = HighExpr::makeRecord(Func.ReturnType, std::move(Leaves));
    Func.Body.push_back(std::move(S));
    return;
  }

  if (ExplicitABI && Med.SourceParametersBound &&
      !Med.SourceTypeHint->ReturnComponents.empty() && CurOp.NumInputs == 1 &&
      CurOp.Inputs[0].Size == Func.ReturnType->Size)
    RetVal = medvarToExpr(CurOp.Inputs[0]);

  if (!RetVal && !UsesFPReturnReg && !Med.FPReturnViaX87 && Func.ReturnType &&
      (Func.ReturnType->Kind == NdTypeKind::Int ||
       Func.ReturnType->Kind == NdTypeKind::Ptr) &&
      hasPropagatedIntegerReturnValue(CurOp, TargetArch, Func.ReturnType->Size))
    RetVal = medvarToExpr(CurOp.Inputs[0]);

  if (!RetVal && CurOp.NumInputs >= 1 && CurOp.Inputs[0].Id >= 0 &&
      CurOp.Inputs[0].Kind == MedVar::Reg) {
    uint64_t RO = CurOp.Inputs[0].RegOff;
    // A floating-point result lives in the FP return register; the RETURN's
    // default integer-register operand does not carry it.
    if (!TRI.isReturnControlReg(RO) &&
        (!(ExplicitABI || UsesFPReturnReg) || RO == ReturnReg))
      RetVal = medvarToExpr(CurOp.Inputs[0]);
  }

  if (!RetVal) {
    for (auto RIt = CurBlock.Ops.rbegin(); RIt != CurBlock.Ops.rend(); ++RIt) {
      if (RIt->Opcode == NdOp::RETURN)
        continue;
      if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
          RIt->Output.RegOff == ReturnReg && !isRegisterLowView(*RIt)) {
        // The copy that names the incoming register is not a value this
        // function computed. A later preserving call leaves that value in
        // place; the parameter fallback below names it.
        if (isIncomingRegisterCopy(*RIt))
          continue;
        // Calls and memory reads materialize once. Other definitions can be
        // followed to their right-hand side so a register-only COPY need not
        // become a source variable with no emitted assignment.
        RetVal = ValueFromDefinition(*RIt);
        break;
      }
    }
  }

  if (!RetVal)
    if (const PhiNode *Phi = detail::registerPhi(Med, CurBlock, ReturnReg, TRI))
      RetVal = HighExpr::makeVar(Phi->Output);

  // Predecessors that leave one version of the register established it
  // before they parted: an ARM predicated store, or a branch around a store
  // between a call and the RETURN. A `cbnz w0` after that call is a low view,
  // not a second write. Predecessors that leave different versions need the
  // PHI. The value a register holds at entry is none this function computed.
  auto agreedByPredecessors = [&](const MedBlock &Join) -> ExprPtr {
    if (Join.Preds.size() <= 1)
      return nullptr;
    auto LeftBy = [&](int PredId, MedVar &Out) {
      const auto Pred = std::find_if(Med.Blocks.begin(), Med.Blocks.end(),
                                     [PredId](const MedBlock &Candidate) {
                                       return Candidate.Id == PredId;
                                     });
      if (Pred == Med.Blocks.end())
        return false;
      for (auto RIt = Pred->Ops.rbegin(); RIt != Pred->Ops.rend(); ++RIt)
        if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
            RIt->Output.RegOff == ReturnReg && !isRegisterLowView(*RIt)) {
          Out = RIt->Output;
          return true;
        }
      if (!reachingRegAtBlockEntry(*Pred, ReturnReg, Out))
        return false;
      widenRegisterLowView(Med, Out);
      return Out.Size > 0;
    };
    std::optional<MedVar> Common;
    for (const int PredId : Join.Preds) {
      MedVar Left;
      if (!LeftBy(PredId, Left) ||
          (Common && (Common->Id != Left.Id || Common->SSAVer != Left.SSAVer)))
        return nullptr;
      Common = Left;
    }
    if (!Common || isEntryIdentity(Med, *Common))
      return nullptr;
    for (const auto &Blk : Med.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Output.Kind == MedVar::Reg && Op.Output.Id == Common->Id &&
            Op.Output.SSAVer == Common->SSAVer)
          return ValueFromDefinition(Op);
    return HighExpr::makeVar(*Common);
  };

  if (!RetVal)
    RetVal = agreedByPredecessors(CurBlock);

  if (!RetVal) {
    // A bare RET block inherits the value on its only incoming edge: the last
    // definition on the chain of single predecessors, or the same agreement
    // at the join that chain runs into. A later branch that does not write
    // the register leaves the call result in it. Choosing one predecessor
    // when they disagree would invent semantics.
    std::set<const MedBlock *> Seen{&CurBlock};
    for (const MedBlock *Pred = onlyPredecessor(Med, CurBlock);
         Pred && !RetVal && Seen.insert(Pred).second;
         Pred = onlyPredecessor(Med, *Pred)) {
      for (auto RIt = Pred->Ops.rbegin(); RIt != Pred->Ops.rend(); ++RIt) {
        if (RIt->Output.Kind != MedVar::Reg || RIt->Output.Size == 0 ||
            RIt->Output.RegOff != ReturnReg || isRegisterLowView(*RIt) ||
            isIncomingRegisterCopy(*RIt))
          continue;
        RetVal = ValueFromDefinition(*RIt);
        break;
      }
      if (!RetVal)
        if (const PhiNode *Phi =
                detail::registerPhi(Med, *Pred, ReturnReg, TRI))
          RetVal = HighExpr::makeVar(Phi->Output);
      if (!RetVal)
        RetVal = agreedByPredecessors(*Pred);
    }
  }

  // Copy propagation can leave the integer return register's value as the
  // temporary it was copied from (`CONCAT t = rax.hi, 0` for `xor al, al`)
  // when no definition above names the register.  Where the RETURN operand
  // is that register (TargetRegInfo::ReturnOperandIsValue), the temporary is
  // the value returned and the register's incoming value is not.
  if (!RetVal && TRI.ReturnOperandIsValue && CurOp.NumInputs >= 1 &&
      CurOp.Inputs[0].Id >= 0 && CurOp.Inputs[0].Kind == MedVar::Temp &&
      !ExplicitABI && !UsesFPReturnReg)
    RetVal = medvarToExpr(CurOp.Inputs[0]);
  if (!RetVal)
    RetVal = unchangedDeclaredReturnParameter(Med, TRI, ReturnReg);
  // A RETURN that no path from the entry reaches with a write of the result
  // register returns what the caller passed there, as the bare `ret` beside
  // the tail call of `if (b) return g(b); return a;` does: the parameter that
  // arrives in it.  A register no parameter arrives in holds nothing the
  // function computed.
  if (!RetVal) {
    const auto Param = std::find_if(
        Med.Params.begin(), Med.Params.end(),
        [&](const MedVar &P) { return P.RegOff == ReturnReg && P.Size > 0; });
    if (Param != Med.Params.end() &&
        registerUntouchedFromEntry(Med, CurBlock, ReturnReg, Param->Size,
                                   TargetArch))
      RetVal = parameterInRegister(Med, ReturnReg);
  }

  // A proven register-pair result occupies both integer return
  // registers.  The RETURN operand names only the low register, so recover
  // the high register from the same reaching definitions before constructing
  // the source return value.  Keep the low and high SSA values separate until
  // this boundary; an unrelated earlier write to the high register is not a
  // substitute for the value reaching this RETURN.
  const uint16_t WordBytes = TRI.integerReturnWordBytes();
  const bool ProvenPair = isWholeIntegerReturnPair(Med.MultiReturn, TRI);
  const uint64_t HighRegister =
      ProvenPair ? secondIntegerResultRegister(TRI) : TRI.IntReturnReg2;
  if (Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Int &&
      Func.ReturnType->Size == 2 * WordBytes &&
      (WordBytes == 4 || isMIPS(TargetArch) || ProvenPair) && HighRegister &&
      RetVal && RetVal->Type && RetVal->Type->Size == WordBytes) {
    // A call after the high register's last write leaves what its callee
    // put there, which this function computed none of.
    ExprPtr High;
    bool Clobbered = false;
    for (auto RIt = CurBlock.Ops.rbegin(); RIt != CurBlock.Ops.rend(); ++RIt) {
      if (RIt->Opcode == NdOp::RETURN)
        continue;
      if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
          RIt->Output.RegOff == HighRegister) {
        High = ValueFromDefinition(*RIt);
        break;
      }
      if ((Clobbered = !callKeepsRegister(*RIt, HighRegister, WordBytes)))
        break;
    }
    if (!High && !Clobbered)
      if (const PhiNode *Phi =
              detail::registerPhi(Med, CurBlock, HighRegister, TRI))
        High = HighExpr::makeVar(Phi->Output);
    if (!High && !Clobbered) {
      if (const MedBlock *Pred = onlyPredecessor(Med, CurBlock)) {
        for (auto RIt = Pred->Ops.rbegin(); RIt != Pred->Ops.rend(); ++RIt) {
          if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
              RIt->Output.RegOff == HighRegister) {
            High = ValueFromDefinition(*RIt);
            break;
          }
          if ((Clobbered = !callKeepsRegister(*RIt, HighRegister, WordBytes)))
            break;
        }
      }
    }
    // A join whose predecessors all carry the same version of the high
    // register needs no PHI: its value is the one reaching the block.  The
    // incoming value at entry, named by an identity copy, is none the
    // function computed.
    if (!High && !Clobbered) {
      MedVar Reaching;
      if (reachingRegAtBlockEntry(CurBlock, HighRegister, Reaching) &&
          Reaching.Size > 0) {
        widenRegisterLowView(Med, Reaching);
        if (Reaching.Size > 0 && !isEntryIdentity(Med, Reaching))
          High = medvarToExpr(Reaching);
      }
    }
    if (High) {
      if (TRI.IntegerReturnPairHighFirst)
        std::swap(High, RetVal);
      auto Pair =
          HighExpr::makeBinop(NdOp::CONCAT, sourceBitSlice(High, 0, WordBytes),
                              sourceBitSlice(RetVal, 0, WordBytes));
      Pair->Type = Func.ReturnType;
      RetVal = std::move(Pair);
    } else {
      // The high half is not proved. Publishing the low half as the pair, or
      // a constant left in the other register, would invent the result. This
      // is a failed pair conversion, not an unbound scalar return register.
      throw std::runtime_error(
          "cannot recover the high register of a wide integer return");
    }
  }

  if (!RetVal) {
    uint16_t RetSz = Func.ReturnType && Func.ReturnType->Size
                         ? Func.ReturnType->Size
                         : TRI.FullRegWidth;
    MedVar RV;
    RV.Kind = MedVar::Reg;
    RV.RegOff = ReturnReg;
    RV.Size = RetSz;
    RV.Id = -1;
    RV.SSAVer = 0;
    RetVal = HighExpr::makeVar(RV);
  }

  // The FP return register holds the scalar's bits in its low lane.
  if (UsesFPReturnReg && Func.ReturnType->Kind == NdTypeKind::Float &&
      (!RetVal->Type || RetVal->Type->Kind != NdTypeKind::Float)) {
    const auto Bytes = ExplicitABI
                           ? Med.SourceTypeHint->ReturnLocation.ValueBytes
                           : Func.ReturnType->Size;
    if (Bytes == Func.ReturnType->Size)
      RetVal = HighExpr::makeBitCast(sourceBitSlice(RetVal, 0, Bytes),
                                     Func.ReturnType);
  }

  // A result the type pass bounded by the bytes every path defines (a `bool`
  // left in AL over an undefined RAX) is the low bytes of the register value;
  // the bytes above belong to no result.
  if (!UsesFPReturnReg && Med.DefinedReturnBytes && Func.ReturnType &&
      Func.ReturnType->Kind == NdTypeKind::Int && Func.ReturnType->Size &&
      Func.ReturnType->Size <= Med.DefinedReturnBytes && RetVal->Type &&
      RetVal->Type->Kind == NdTypeKind::Int &&
      Func.ReturnType->Size < RetVal->Type->Size)
    RetVal = sourceBitSlice(RetVal, 0, Func.ReturnType->Size);

  if (Med.FPReturnViaX87 && RetVal && RetVal->Type && Func.ReturnType &&
      RetVal->Type->Kind == NdTypeKind::Float &&
      Func.ReturnType->Kind == NdTypeKind::Float &&
      RetVal->Type->Size != Func.ReturnType->Size) {
    // An IEEE value extended into the x87 carrier and immediately returned
    // at its declared precision is still that value, not eighty raw bits.
    if (RetVal->Kind == ExprKind::UnaryOp &&
        RetVal->Op == NdOp::FLOAT_FLOAT2FLOAT && RetVal->Operands.size() == 1 &&
        equalSourceTypes(RetVal->Operands[0]->Type, Func.ReturnType))
      RetVal = RetVal->Operands[0];
    else {
      RetVal = HighExpr::makeUnary(NdOp::FLOAT_FLOAT2FLOAT, RetVal);
      RetVal->Type = Func.ReturnType;
    }
  }
  S.RetVal = RetVal;
  Func.Body.push_back(std::move(S));
}

} // namespace neverd
